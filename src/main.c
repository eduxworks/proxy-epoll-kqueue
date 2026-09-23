/* main.c — punto de entrada del proxy.
 *
 * Esqueleto: por ahora monta el bucle de eventos y el self-pipe de señales
 * (E13), que es la pieza que condiciona el resto del diseño. El listener, el
 * router y el pool llegan en sus propios módulos.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "io_event.h"

#define PROXY_VERSION "0.1.0"

/* El handler de señal solo puede tocar esto, y solo con write(). */
static int sig_pipe[2] = { -1, -1 };

static io_loop *g_loop; /* lo necesita el callback del self-pipe */

/* Truco del self-pipe: la única función async-signal-safe que usamos aquí es
 * write(). Nada de log(), malloc() ni printf() dentro de un handler. */
static void on_signal(int signo)
{
    unsigned char b = (unsigned char)signo;
    ssize_t       n = write(sig_pipe[1], &b, 1);
    (void)n; /* si el pipe está lleno ya hay una señal pendiente por atender */
}

static void handle_signal(unsigned char signo)
{
    switch (signo) {
    case SIGHUP:
        /* E13: aquí irá config_load() + router_build() + swap atómico.
         * Un reload que falle debe conservar la configuración anterior. */
        printf("SIGHUP recibida: recarga de configuración (pendiente)\n");
        fflush(stdout);
        break;
    case SIGINT:
    case SIGTERM:
        printf("Señal %d: cerrando\n", (int)signo);
        fflush(stdout);
        io_loop_stop(g_loop);
        break;
    default:
        break;
    }
}

/* Edge-triggered: hay que drenar hasta EAGAIN o el pipe no vuelve a despertar. */
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
        break; /* n == 0, o EAGAIN/EWOULDBLOCK: no queda nada por leer */
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

    if (sigaction(SIGHUP, &sa, NULL) < 0 ||
        sigaction(SIGINT, &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction");
        return -1;
    }

    /* Un upstream que cierra no debe matar el proceso: se detecta por EPIPE. */
    signal(SIGPIPE, SIG_IGN);

    return io_loop_add(loop, sig_pipe[0], IO_READ, on_signal_readable, NULL);
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
            printf("proxy %s (backend de E/S: %s)\n", PROXY_VERSION,
                   io_backend_name());
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

    g_loop = io_loop_create();
    if (g_loop == NULL) {
        perror("io_loop_create");
        return EXIT_FAILURE;
    }

    if (setup_signals(g_loop) < 0) {
        io_loop_destroy(g_loop);
        return EXIT_FAILURE;
    }

    printf("proxy %s — backend %s, config %s\n", PROXY_VERSION,
           io_backend_name(), config_path);
    printf("Esqueleto: sin listeners todavía. SIGHUP recarga, SIGINT/SIGTERM salen.\n");
    fflush(stdout);

    int rc = io_loop_run(g_loop);
    if (rc < 0) {
        perror("io_loop_run");
    }

    io_loop_del(g_loop, sig_pipe[0]);
    close(sig_pipe[0]);
    close(sig_pipe[1]);
    io_loop_destroy(g_loop);

    return rc < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
