/* stats.c — socket UNIX que escupe un JSON y cierra.
 *
 * El snapshot se genera entero al aceptar y se envía por el bucle como
 * cualquier otra escritura: si el cliente lee despacio, se reintenta con
 * IO_WRITE en vez de bloquear.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "backend_pool.h"
#include "log.h"
#include "stats.h"

#define STATS_MAX_JSON 65536

typedef struct stats_conn {
    struct stats_conn *next;
    stats             *owner;
    int                fd;
    char              *body;
    size_t             len;
    size_t             off;
} stats_conn;

struct stats {
    int           fd;
    char         *path;
    io_loop      *loop;
    router_slot  *slot;
    conn_manager *conns;
    int           worker_id;
    time_t        started;
    stats_conn   *clients;
};

/* --- render --------------------------------------------------------------- */

#define APPEND(...)                                                     \
    do {                                                                \
        int _w = snprintf(out + used, outlen - used, __VA_ARGS__);       \
        if (_w < 0 || (size_t)_w >= outlen - used) {                     \
            return 0;                                                   \
        }                                                               \
        used += (size_t)_w;                                             \
    } while (0)

size_t stats_render(char *out, size_t outlen, router *r, const conn_manager *conns,
                    int worker_id, time_t started)
{
    if (out == NULL || outlen == 0) {
        return 0;
    }

    size_t used = 0;
    long   up   = (long)(time(NULL) - started);

    APPEND("{\n  \"worker\": %d,\n  \"uptime_s\": %ld,\n", worker_id, up);
    APPEND("  \"connections\": { \"active\": %zu, \"accepted\": %lu, "
           "\"requests\": %lu, \"errors\": %lu },\n",
           conn_manager_active(conns), conn_manager_accepted(conns),
           conn_manager_requests(conns), conn_manager_errors(conns));
    APPEND("  \"log\": { \"dropped\": %lu },\n", log_dropped());
    APPEND("  \"backends\": [");

    bool first = true;
    for (size_t pi = 0; r != NULL && pi < router_pool_count(r); pi++) {
        backend_pool      *p  = router_pool_at(r, pi);
        const cfg_backend *bc = pool_config(p);

        for (size_t bi = 0; bi < pool_size(p); bi++) {
            backend *b = pool_backend_at(p, bi);
            APPEND("%s\n    { \"pool\": \"%s\", \"addr\": \"%s\", \"up\": %s, "
                   "\"active\": %d, \"weight\": %d }",
                   first ? "" : ",", bc->name, backend_addr(b),
                   backend_is_up(b) ? "true" : "false", backend_active_conns(b),
                   backend_weight(b));
            first = false;
        }
    }

    APPEND("%s]\n}\n", first ? "" : "\n  ");
    return used;
}

#undef APPEND

/* --- conexiones del socket ------------------------------------------------ */

static void client_close(stats_conn *c)
{
    stats *s = c->owner;

    for (stats_conn **pp = &s->clients; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    }

    io_loop_del(s->loop, c->fd);
    close(c->fd);
    free(c->body);
    free(c);
}

static void on_client_writable(int fd, unsigned events, void *ctx)
{
    (void)events;
    stats_conn *c = ctx;

    while (c->off < c->len) {
        ssize_t n = write(fd, c->body + c->off, c->len - c->off);
        if (n > 0) {
            c->off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return; /* se reintenta cuando vuelva a avisar */
        }
        break;
    }

    client_close(c);
}

static void on_accept(int fd, unsigned events, void *ctx)
{
    (void)events;
    stats *s = ctx;

    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            return;
        }

        if (io_set_nonblocking(cfd) < 0) {
            close(cfd);
            continue;
        }

        char *body = malloc(STATS_MAX_JSON);
        if (body == NULL) {
            close(cfd);
            continue;
        }

        /* El router se toma con referencia: un reload a mitad de snapshot no
         * puede liberar los pools que estamos recorriendo. */
        router *r   = router_slot_acquire(s->slot);
        size_t  len = stats_render(body, STATS_MAX_JSON, r, s->conns, s->worker_id,
                                   s->started);
        router_unref(r);

        if (len == 0) {
            free(body);
            close(cfd);
            continue;
        }

        stats_conn *c = calloc(1, sizeof *c);
        if (c == NULL) {
            free(body);
            close(cfd);
            continue;
        }

        c->owner = s;
        c->fd    = cfd;
        c->body  = body;
        c->len   = len;

        c->next    = s->clients;
        s->clients = c;

        if (io_loop_add(s->loop, cfd, IO_WRITE, on_client_writable, c) < 0) {
            client_close(c);
            continue;
        }

        /* Casi siempre cabe entero en el buffer del socket: se intenta ya y así
         * la mayoría de consultas se resuelven sin un segundo evento. */
        on_client_writable(cfd, IO_WRITE, c);
    }
}

/* --- API ------------------------------------------------------------------ */

stats *stats_open(const char *path, io_loop *loop, router_slot *slot,
                  conn_manager *conns, int worker_id, time_t started)
{
    if (path == NULL || loop == NULL || slot == NULL) {
        errno = EINVAL;
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(addr.sun_path, path, strlen(path));

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return NULL;
    }

    /* Un socket UNIX huérfano de una ejecución anterior impide el bind, y el
     * error sería "Address already in use" sin que haya nadie escuchando. */
    unlink(path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        listen(fd, 16) < 0 || io_set_nonblocking(fd) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }

    stats *s = calloc(1, sizeof *s);
    if (s == NULL) {
        close(fd);
        return NULL;
    }

    s->fd        = fd;
    s->path      = strdup(path);
    s->loop      = loop;
    s->slot      = slot;
    s->conns     = conns;
    s->worker_id = worker_id;
    s->started   = started;

    if (io_loop_add(loop, fd, IO_READ, on_accept, s) < 0) {
        int saved = errno;
        close(fd);
        free(s->path);
        free(s);
        errno = saved;
        return NULL;
    }

    return s;
}

void stats_close(stats *s)
{
    if (s == NULL) {
        return;
    }

    while (s->clients != NULL) {
        client_close(s->clients);
    }

    if (s->fd >= 0) {
        io_loop_del(s->loop, s->fd);
        close(s->fd);
    }
    if (s->path != NULL) {
        unlink(s->path);
        free(s->path);
    }
    free(s);
}
