/* bench_backend.c — backend de carga para el benchmark (E2).
 *
 * echo_server, el del e2e, hace fork por conexión: con -c400 serían cientos de
 * procesos y el que estaríamos midiendo sería él, no el proxy. Este usa el
 * mismo bucle de eventos que el proxy y responde algo fijo, de modo que el
 * cuello de botella quede donde queremos medirlo.
 *
 * Responde siempre lo mismo, con Content-Length y keep-alive, que es lo que
 * permite al proxy reutilizar la conexión de salida.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "io_event.h"
#include "net_compat.h"

static const char RESP[] = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/plain\r\n"
                           "Content-Length: 13\r\n"
                           "Connection: keep-alive\r\n"
                           "\r\n"
                           "hola, mundo!\n";
#define RESP_LEN (sizeof RESP - 1)

#define IN_CAP 4096

typedef struct {
    bool   used;
    size_t in_len;
    size_t pending; /* respuestas aún por escribir */
    size_t out_off; /* dentro de la respuesta en curso */
    char   in[IN_CAP];
} bconn;

static bconn   *conns;
static size_t   conns_cap;
static io_loop *loop;

static bool reserve(int fd)
{
    if (fd < 0) {
        return false;
    }
    size_t need = (size_t)fd + 1;
    if (need <= conns_cap) {
        return true;
    }
    size_t cap = conns_cap ? conns_cap : 256;
    while (cap < need) {
        cap *= 2;
    }
    bconn *v = realloc(conns, cap * sizeof *v);
    if (v == NULL) {
        return false;
    }
    memset(v + conns_cap, 0, (cap - conns_cap) * sizeof *v);
    conns     = v;
    conns_cap = cap;
    return true;
}

static void close_conn(int fd)
{
    io_loop_del(loop, fd);
    close(fd);
    if ((size_t)fd < conns_cap) {
        conns[fd].used    = false;
        conns[fd].in_len  = 0;
        conns[fd].pending = 0;
        conns[fd].out_off = 0;
    }
}

/* Escribe las respuestas pendientes. false si el socket murió. */
static bool flush_conn(int fd)
{
    bconn *c = &conns[fd];

    while (c->pending > 0) {
        ssize_t n = write(fd, RESP + c->out_off, RESP_LEN - c->out_off);
        if (n > 0) {
            c->out_off += (size_t)n;
            if (c->out_off == RESP_LEN) {
                c->out_off = 0;
                c->pending--;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true; /* se reintenta con IO_WRITE */
        }
        return false;
    }
    return true;
}

static void on_conn(int fd, unsigned events, void *ctx)
{
    (void)ctx;
    bconn *c = &conns[fd];

    if (events & IO_ERR) {
        close_conn(fd);
        return;
    }

    if (events & IO_READ) {
        for (;;) {
            if (c->in_len == IN_CAP) {
                c->in_len = 0; /* cabecera absurda: se descarta */
            }
            ssize_t n = read(fd, c->in + c->in_len, IN_CAP - c->in_len);
            if (n > 0) {
                c->in_len += (size_t)n;
                continue;
            }
            if (n == 0) {
                close_conn(fd);
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            close_conn(fd);
            return;
        }

        /* Una respuesta por cada cabecera completa recibida. */
        size_t scan = 0;
        while (c->in_len - scan >= 4) {
            if (memcmp(c->in + scan, "\r\n\r\n", 4) == 0) {
                c->pending++;
                scan += 4;
                continue;
            }
            scan++;
        }
        if (scan > 0) {
            size_t rest = c->in_len - scan;
            memmove(c->in, c->in + scan, rest);
            c->in_len = rest;
        }
    }

    if (!flush_conn(fd)) {
        close_conn(fd);
        return;
    }

    io_loop_mod(loop, fd, c->pending > 0 ? (IO_READ | IO_WRITE) : IO_READ);
}

static void on_accept(int lfd, unsigned events, void *ctx)
{
    (void)events;
    (void)ctx;

    for (;;) {
        struct sockaddr_storage peer;
        socklen_t               plen = sizeof peer;

        int fd = net_accept(lfd, &peer, &plen);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            return;
        }

        if (!reserve(fd)) {
            close(fd);
            continue;
        }

        int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);

        conns[fd].used    = true;
        conns[fd].in_len  = 0;
        conns[fd].pending = 0;
        conns[fd].out_off = 0;

        if (io_loop_add(loop, fd, IO_READ, on_conn, NULL) < 0) {
            close(fd);
        }
    }
}

static int open_listener(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    net_set_reuseport(fd);
    io_set_nonblocking(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 1024) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

static void on_term(int signo)
{
    (void)signo;
    if (loop != NULL) {
        io_loop_stop(loop);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <puerto> [workers]\n", argv[0]);
        return EXIT_FAILURE;
    }

    uint16_t port    = (uint16_t)atoi(argv[1]);
    int      workers = argc > 2 ? atoi(argv[2]) : 1;
    if (workers < 1) {
        workers = 1;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    setpgid(0, 0); /* para llevarse a los hijos al salir */

    for (int i = 1; i < workers; i++) {
        if (fork() == 0) {
            break;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    loop = io_loop_create();
    if (loop == NULL) {
        perror("io_loop_create");
        return EXIT_FAILURE;
    }

    int lfd = open_listener(port);
    if (lfd < 0) {
        return EXIT_FAILURE;
    }
    if (io_loop_add(loop, lfd, IO_READ, on_accept, NULL) < 0) {
        perror("io_loop_add");
        return EXIT_FAILURE;
    }

    io_loop_run(loop);

    close(lfd);
    io_loop_destroy(loop);
    free(conns);
    return EXIT_SUCCESS;
}
