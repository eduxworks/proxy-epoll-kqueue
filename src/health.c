/* health.c — sondas TCP y HTTP contra cada backend.
 *
 * El hilo no toca el router directamente: pide una referencia bajo cerrojo,
 * sondea con ella y la suelta. Así un reload puede publicar un router nuevo y
 * liberar el viejo sin que el sondeo se quede con memoria liberada en la mano.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "backend_pool.h"
#include "health.h"
#include "log.h"

#define TICK_MS 100

struct health {
    pthread_t       thread;
    pthread_mutex_t lock;   /* protege `current` */
    router         *current;
    _Atomic bool    running;
    _Atomic unsigned long cycles;
    bool            started;
};

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* --- sondas --------------------------------------------------------------- */

/* connect() no bloqueante + poll: es la única forma de acotar de verdad lo que
 * tarda un intento contra una máquina que no responde. */
static int connect_timeout(const char *host, uint16_t port, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        return fd;
    }
    if (errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
    int           n   = poll(&pfd, 1, timeout_ms);
    if (n <= 0) {
        close(fd); /* 0 = timeout: el backend no contesta a tiempo */
        return -1;
    }

    int       soerr = 0;
    socklen_t slen  = sizeof soerr;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0 || soerr != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static bool probe_http(int fd, const char *host, uint16_t port, const char *path,
                       int timeout_ms)
{
    char req[512];
    int  n = snprintf(req, sizeof req,
                      "GET %s HTTP/1.0\r\nHost: %s:%u\r\n"
                      "User-Agent: proxy-health\r\nConnection: close\r\n\r\n",
                      path != NULL ? path : "/", host, (unsigned)port);

    size_t off = 0;
    while (off < (size_t)n) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
        if (poll(&pfd, 1, timeout_ms) <= 0) {
            return false;
        }
        ssize_t w = write(fd, req + off, (size_t)n - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        return false;
    }

    /* Basta la línea de estado: un backend que contesta "HTTP/1.1 200" ya ha
     * demostrado lo que la sonda quiere saber. */
    char   buf[128];
    size_t got = 0;
    while (got < sizeof buf - 1) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, timeout_ms) <= 0) {
            return false;
        }
        ssize_t r = read(fd, buf + got, sizeof buf - 1 - got);
        if (r > 0) {
            got += (size_t)r;
            if (memchr(buf, '\n', got) != NULL) {
                break;
            }
            continue;
        }
        if (r < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        break;
    }
    buf[got] = '\0';

    int code = 0;
    if (sscanf(buf, "HTTP/1.%*d %d", &code) != 1) {
        return false;
    }
    return code >= 200 && code < 400;
}

static bool probe_backend(const cfg_health *h, const char *host, uint16_t port)
{
    int timeout = h->timeout_ms > 0 ? h->timeout_ms : 500;

    int fd = connect_timeout(host, port, timeout);
    if (fd < 0) {
        return false;
    }

    bool ok = true;
    if (h->type == HEALTH_HTTP) {
        ok = probe_http(fd, host, port, h->path, timeout);
    }

    close(fd);
    return ok;
}

/* --- hilo ----------------------------------------------------------------- */

static router *take_router(health *hc)
{
    pthread_mutex_lock(&hc->lock);
    router *r = hc->current;
    router_ref(r);
    pthread_mutex_unlock(&hc->lock);
    return r;
}

static void *worker(void *arg)
{
    health *hc = arg;

    /* Cuándo toca la siguiente sonda de cada backend. Se indexa por pool y
     * posición; al recargar se reinicia, que es lo correcto: son otros pools. */
    long next_at = 0;

    while (atomic_load_explicit(&hc->running, memory_order_acquire)) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = TICK_MS * 1000L * 1000L };
        nanosleep(&ts, NULL);

        long now = now_ms();
        if (now < next_at) {
            continue;
        }

        router *r = take_router(hc);
        if (r == NULL) {
            continue;
        }

        long soonest = now + 60000;

        for (size_t pi = 0; pi < router_pool_count(r); pi++) {
            backend_pool      *p   = router_pool_at(r, pi);
            const cfg_backend *bc  = pool_config(p);
            int                iv  = bc->health.interval_ms > 0 ? bc->health.interval_ms : 2000;

            for (size_t bi = 0; bi < pool_size(p); bi++) {
                backend *b     = pool_backend_at(p, bi);
                bool     was_up = backend_is_up(b);
                bool     ok     = probe_backend(&bc->health, backend_host(b),
                                                backend_port(b));

                backend_probe_result(p, b, ok);

                bool now_up = backend_is_up(b);
                if (now_up != was_up) {
                    log_write(now_up ? LOG_INFO : LOG_WARN,
                              "backend %s del pool %s pasa a %s (sonda activa)",
                              backend_addr(b), bc->name, now_up ? "UP" : "DOWN");
                }
            }

            if (now + iv < soonest) {
                soonest = now + iv;
            }
        }

        next_at = soonest;
        router_unref(r);
        atomic_fetch_add_explicit(&hc->cycles, 1, memory_order_relaxed);
    }

    return NULL;
}

/* --- API ------------------------------------------------------------------ */

health *health_start(router *initial)
{
    if (initial == NULL) {
        return NULL;
    }

    health *hc = calloc(1, sizeof *hc);
    if (hc == NULL) {
        return NULL;
    }

    if (pthread_mutex_init(&hc->lock, NULL) != 0) {
        free(hc);
        return NULL;
    }

    router_ref(initial);
    hc->current = initial;
    atomic_init(&hc->running, true);
    atomic_init(&hc->cycles, 0);

    if (pthread_create(&hc->thread, NULL, worker, hc) != 0) {
        router_unref(initial);
        pthread_mutex_destroy(&hc->lock);
        free(hc);
        return NULL;
    }

    hc->started = true;
    return hc;
}

void health_set_router(health *hc, router *next)
{
    if (hc == NULL || next == NULL) {
        return;
    }

    router_ref(next);

    pthread_mutex_lock(&hc->lock);
    router *old = hc->current;
    hc->current = next;
    pthread_mutex_unlock(&hc->lock);

    router_unref(old);
}

void health_stop(health *hc)
{
    if (hc == NULL || !hc->started) {
        free(hc);
        return;
    }

    atomic_store_explicit(&hc->running, false, memory_order_release);
    pthread_join(hc->thread, NULL);

    router_unref(hc->current);
    pthread_mutex_destroy(&hc->lock);
    free(hc);
}

unsigned long health_cycles(const health *hc)
{
    if (hc == NULL) {
        return 0;
    }
    return atomic_load_explicit(&hc->cycles, memory_order_relaxed);
}
