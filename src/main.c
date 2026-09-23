/* main.c — arranque, workers y recarga en caliente.
 *
 * Un proceso por CPU (E5), cada uno con su propio bucle de eventos y su propio
 * socket de escucha sobre el mismo puerto gracias a SO_REUSEPORT. No hay estado
 * compartido entre workers: nada de bloqueos, nada de contención.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "buffer_pool.h"
#include "config.h"
#include "connection.h"
#include "io_event.h"
#include "listener.h"
#include "net_compat.h"
#include "router.h"

#define PROXY_VERSION "0.1.0"

#define MIN_SLOTS_PER_WORKER 256
#define MAX_SLOTS_PER_WORKER 4096

/* Lo único que toca el handler de señal. */
static int sig_pipe[2] = { -1, -1 };

/* Estado del worker en curso. Global porque el callback del self-pipe no
 * recibe contexto, y porque cada worker es un proceso con un solo bucle. */
static struct {
    io_loop     *loop;
    router_slot *slot;
    const char  *config_path;
    listener   **ls;
    size_t       n_ls;
    int          id;
} W;

/* --- señales -------------------------------------------------------------- */

static void on_signal(int signo)
{
    /* write() es async-signal-safe; log(), malloc() y printf() no. */
    unsigned char b = (unsigned char)signo;
    ssize_t       n = write(sig_pipe[1], &b, 1);
    (void)n;
}

/* E13: se parsea y valida en memoria nueva; si algo falla se conserva la
 * configuración anterior, porque un reload roto no puede degradar el servicio. */
static void reload_config(void)
{
    char    err[512] = { 0 };
    config *cfg      = config_load(W.config_path, err, sizeof err);
    if (cfg == NULL) {
        fprintf(stderr, "[worker %d] recarga rechazada: %s\n", W.id, err);
        return;
    }

    /* Los listeners ya están abiertos y las conexiones vivas se enrutan por
     * índice de frontend: si la lista cambia, ese índice pasaría a significar
     * otra cosa. Cambiar de puertos en caliente exige abrir y cerrar sockets,
     * que aún no se hace, así que se rechaza en vez de enrutar mal. */
    if (cfg->n_frontends != W.n_ls) {
        fprintf(stderr,
                "[worker %d] recarga rechazada: el número de frontends cambió "
                "(%zu -> %zu); reinicia para eso\n",
                W.id, W.n_ls, cfg->n_frontends);
        config_free(cfg);
        return;
    }
    for (size_t i = 0; i < W.n_ls; i++) {
        if (cfg->frontends[i].listen_port != listener_port(W.ls[i])) {
            fprintf(stderr,
                    "[worker %d] recarga rechazada: frontend[%zu] cambió de puerto "
                    "(%u -> %u); reinicia para eso\n",
                    W.id, i, (unsigned)listener_port(W.ls[i]),
                    (unsigned)cfg->frontends[i].listen_port);
            config_free(cfg);
            return;
        }
    }

    router *next = router_build(cfg);
    if (next == NULL) {
        fprintf(stderr, "[worker %d] recarga rechazada: no se pudo construir el router\n",
                W.id);
        config_free(cfg);
        return;
    }

    /* Intercambio atómico. Las conexiones en vuelo conservan su referencia al
     * router viejo y terminan con la configuración con la que empezaron. */
    router_slot_publish(W.slot, next);
    fprintf(stderr, "[worker %d] configuración recargada\n", W.id);
}

static void handle_signal(unsigned char signo)
{
    switch (signo) {
    case SIGHUP:
        reload_config();
        break;
    case SIGINT:
    case SIGTERM:
        io_loop_stop(W.loop);
        break;
    default:
        break;
    }
}

/* Edge-triggered: drenar hasta EAGAIN o el pipe no vuelve a despertar. */
static void on_signal_readable(int fd, unsigned events, void *ctx)
{
    (void)events;
    (void)ctx;

    for (;;) {
        unsigned char buf[64];
        ssize_t       n = read(fd, buf, sizeof buf);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                handle_signal(buf[i]);
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

static int setup_signals(io_loop *loop)
{
    if (pipe(sig_pipe) < 0) {
        perror("pipe");
        return -1;
    }
    if (io_set_nonblocking(sig_pipe[0]) < 0 ||
        io_set_nonblocking(sig_pipe[1]) < 0) {
        perror("O_NONBLOCK");
        return -1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sa.sa_flags   = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGHUP, &sa, NULL) < 0 || sigaction(SIGINT, &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction");
        return -1;
    }

    /* Un upstream que cierra de golpe no debe matar el worker: se detecta por
     * EPIPE en el write, que la máquina de estados ya trata. */
    signal(SIGPIPE, SIG_IGN);

    return io_loop_add(loop, sig_pipe[0], IO_READ, on_signal_readable, NULL);
}

/* --- worker --------------------------------------------------------------- */

static conn_manager *g_conns;

static void on_new_connection(int client_fd, const struct sockaddr_storage *peer,
                              size_t frontend_index, void *ctx)
{
    (void)ctx;
    conn_accepted(g_conns, client_fd, peer, frontend_index);
}

static size_t slots_for(const config *cfg, int workers)
{
    /* Dos slots por conexión (un sentido cada uno), repartidos entre workers y
     * acotados: la arena se reserva entera al arrancar, así que un max_conns
     * generoso no debe traducirse en gigabytes por proceso. */
    size_t per_worker = (size_t)cfg->global.max_conns / (size_t)workers;
    size_t slots      = per_worker * 2;
    if (slots < MIN_SLOTS_PER_WORKER) {
        slots = MIN_SLOTS_PER_WORKER;
    }
    if (slots > MAX_SLOTS_PER_WORKER) {
        slots = MAX_SLOTS_PER_WORKER;
    }
    return slots;
}

/* Adopta cfg: se libera con el router. */
static int run_worker(config *cfg, const char *config_path, int id, int workers)
{
    int rc = EXIT_FAILURE;

    io_loop      *loop   = NULL;
    router       *rt     = NULL;
    router_slot  *slot   = NULL;
    buffer_pool  *bufs   = NULL;
    conn_manager *conns  = NULL;
    listener    **ls     = NULL;
    size_t        n_ls   = 0;
    size_t        n_slots = slots_for(cfg, workers);

    rt = router_build(cfg);
    if (rt == NULL) {
        fprintf(stderr, "[worker %d] no se pudo construir el router\n", id);
        config_free(cfg);
        return EXIT_FAILURE;
    }
    /* A partir de aquí cfg pertenece al router. */

    slot = router_slot_create(rt);
    loop = io_loop_create();
    bufs = bufpool_create(n_slots);
    if (slot == NULL || loop == NULL || bufs == NULL) {
        fprintf(stderr, "[worker %d] sin memoria al arrancar\n", id);
        goto done;
    }

    conns = conn_manager_create(loop, slot, bufs);
    if (conns == NULL) {
        goto done;
    }
    g_conns = conns;

    ls = calloc(cfg->n_frontends, sizeof *ls);
    if (ls == NULL) {
        goto done;
    }

    for (size_t i = 0; i < cfg->n_frontends; i++) {
        char err[256] = { 0 };
        ls[i]         = listener_open(&cfg->frontends[i], i, err, sizeof err);
        if (ls[i] == NULL) {
            fprintf(stderr, "[worker %d] %s\n", id, err);
            goto done;
        }
        n_ls = i + 1;

        if (listener_attach(ls[i], loop, on_new_connection, NULL) < 0) {
            fprintf(stderr, "[worker %d] no se pudo registrar el listener\n", id);
            goto done;
        }
    }

    W.loop        = loop;
    W.slot        = slot;
    W.config_path = config_path;
    W.ls          = ls;
    W.n_ls        = n_ls;
    W.id          = id;

    if (setup_signals(loop) < 0) {
        goto done;
    }

    if (id == 0) {
        fprintf(stderr, "proxy %s · %s · %s%s · %zu slots de %d KB por worker\n",
                PROXY_VERSION, io_backend_name(), net_reuseport_name(),
                net_reuseport_balances() ? "" : " (sin reparto del kernel)",
                n_slots, BUFPOOL_SLOT_SIZE / 1024);
        for (size_t i = 0; i < n_ls; i++) {
            fprintf(stderr, "  escuchando en %s\n", cfg->frontends[i].listen);
        }
    }

    rc = io_loop_run(loop) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;

done:
    for (size_t i = 0; i < n_ls; i++) {
        listener_close(ls[i]);
    }
    free(ls);
    conn_manager_destroy(conns);
    bufpool_destroy(bufs);
    router_slot_destroy(slot); /* suelta la última referencia del router */
    io_loop_destroy(loop);
    if (sig_pipe[0] >= 0) {
        close(sig_pipe[0]);
        close(sig_pipe[1]);
    }
    return rc;
}

/* --- arranque ------------------------------------------------------------- */

static volatile sig_atomic_t g_last_signal;

static void on_parent_signal(int signo)
{

    g_last_signal = signo;
}

static int decide_workers(const config *cfg)
{
    int n = cfg->global.workers;
    if (n <= 0) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        n         = cpus > 0 ? (int)cpus : 1;
    }

    /* En macOS SO_REUSEPORT permite el bind múltiple pero el kernel no reparte:
     * varios workers se traducirían en uno atendiendo y el resto mirando. Más
     * vale un worker y decirlo que fingir paralelismo. */
    if (n > 1 && !net_reuseport_balances()) {
        fprintf(stderr,
                "aviso: %s no reparte carga en esta plataforma; se usa 1 worker\n",
                net_reuseport_name());
        n = 1;
    }
    return n;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Uso: %s -c <config.toml>\n"
            "  -c <ruta>   fichero de configuración TOML\n"
            "  -v          versión y backend de E/S\n"
            "  -h          esta ayuda\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int         opt;

    while ((opt = getopt(argc, argv, "c:vh")) != -1) {
        switch (opt) {
        case 'c':
            config_path = optarg;
            break;
        case 'v':
            printf("proxy %s (backend de E/S: %s, reparto: %s)\n", PROXY_VERSION,
                   io_backend_name(), net_reuseport_name());
            return EXIT_SUCCESS;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (config_path == NULL) {
        fprintf(stderr, "Falta -c <config.toml>\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    char    err[512] = { 0 };
    config *cfg      = config_load(config_path, err, sizeof err);
    if (cfg == NULL) {
        fprintf(stderr, "configuración inválida: %s\n", err);
        return EXIT_FAILURE;
    }

    int workers = decide_workers(cfg);
    if (workers == 1) {
        return run_worker(cfg, config_path, 0, 1);
    }

    /* El clamp no es defensivo de mas: sin el, el compilador no puede probar
     * que el tamano del calloc sea razonable y -Werror lo rechaza. */
    size_t nworkers = workers > 0 ? (size_t)workers : 1;
    pid_t *pids     = calloc(nworkers, sizeof *pids);
    if (pids == NULL) {
        config_free(cfg);
        return EXIT_FAILURE;
    }

    for (int i = 0; i < workers; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            break;
        }
        if (pid == 0) {
            /* El hijo hereda su propia copia de cfg: cada worker recarga y
             * libera la suya sin tocar la de nadie. */
            free(pids);
            _exit(run_worker(cfg, config_path, i, workers));
        }
        pids[i] = pid;
    }

    /* El padre solo repite señales y espera. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_parent_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    config_free(cfg); /* la copia del padre ya no sirve de nada */

    int alive = workers;
    while (alive > 0) {
        int   status;
        pid_t done = wait(&status);
        if (done > 0) {
            alive--;
            continue;
        }
        if (errno == EINTR) {
            int sig = (int)g_last_signal;
            for (int i = 0; i < workers; i++) {
                if (pids[i] > 0) {
                    kill(pids[i], sig);
                }
            }
            g_last_signal = 0;
            continue;
        }
        break;
    }

    free(pids);
    return EXIT_SUCCESS;
}
