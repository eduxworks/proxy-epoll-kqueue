/* listener.c — accept loop por puerto frontal. */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "listener.h"
#include "net_compat.h"

struct listener {
    int         fd;
    size_t      frontend_index;
    uint16_t    port;
    listener_cb cb;
    void       *ctx;
};

static void seterr(char *err, size_t errlen, const char *fmt, ...)
{
    if (err == NULL || errlen == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

listener *listener_open(const cfg_frontend *f, size_t frontend_index, char *err,
                        size_t errlen)
{
    if (f == NULL) {
        seterr(err, errlen, "frontend nulo");
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(f->listen_port);

    if (inet_pton(AF_INET, f->listen_host, &addr.sin_addr) != 1) {
        seterr(err, errlen, "listen \"%s\": dirección IPv4 inválida", f->listen);
        return NULL;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        seterr(err, errlen, "socket: %s", strerror(errno));
        return NULL;
    }

    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) < 0) {
        seterr(err, errlen, "SO_REUSEADDR: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    /* Sin esto, el segundo worker que arranque se estrella con EADDRINUSE. */
    if (net_set_reuseport(fd) < 0) {
        seterr(err, errlen, "%s: %s", net_reuseport_name(), strerror(errno));
        close(fd);
        return NULL;
    }
    if (io_set_nonblocking(fd) < 0) {
        seterr(err, errlen, "O_NONBLOCK: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        seterr(err, errlen, "bind %s: %s", f->listen, strerror(errno));
        close(fd);
        return NULL;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        seterr(err, errlen, "listen %s: %s", f->listen, strerror(errno));
        close(fd);
        return NULL;
    }

    listener *l = calloc(1, sizeof *l);
    if (l == NULL) {
        seterr(err, errlen, "sin memoria");
        close(fd);
        return NULL;
    }

    l->fd             = fd;
    l->frontend_index = frontend_index;

    /* Con puerto 0 el kernel elige: hay que preguntarle cuál tocó. */
    struct sockaddr_in bound;
    socklen_t          blen = sizeof bound;
    if (getsockname(fd, (struct sockaddr *)&bound, &blen) == 0) {
        l->port = ntohs(bound.sin_port);
    } else {
        l->port = f->listen_port;
    }

    return l;
}

void listener_close(listener *l)
{
    if (l == NULL) {
        return;
    }
    if (l->fd >= 0) {
        close(l->fd);
    }
    free(l);
}

int listener_fd(const listener *l)
{
    return l != NULL ? l->fd : -1;
}

uint16_t listener_port(const listener *l)
{
    return l != NULL ? l->port : 0;
}

/* Edge-triggered: si no se acepta hasta EAGAIN, las conexiones que llegaron
 * entre medias se quedan en la cola sin que el kernel vuelva a avisar. */
static void on_acceptable(int fd, unsigned events, void *ctx)
{
    listener *l = ctx;

    if (events & IO_ERR) {
        return;
    }

    for (;;) {
        struct sockaddr_storage peer;
        socklen_t               plen = sizeof peer;

        int cfd = net_accept(fd, &peer, &plen);
        if (cfd >= 0) {
            l->cb(cfd, &peer, l->frontend_index, l->ctx);
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break; /* cola vacía: hemos drenado */
        }
        if (errno == EINTR || errno == ECONNABORTED) {
            continue; /* el cliente se fue antes de tiempo: no es asunto nuestro */
        }
        /* EMFILE/ENFILE: quedarse sin descriptores no debe tumbar el worker.
         * Se sale del bucle y se reintenta en el siguiente evento. */
        break;
    }
}

int listener_attach(listener *l, io_loop *loop, listener_cb cb, void *ctx)
{
    if (l == NULL || loop == NULL || cb == NULL) {
        errno = EINVAL;
        return -1;
    }
    l->cb  = cb;
    l->ctx = ctx;
    return io_loop_add(loop, l->fd, IO_READ, on_acceptable, l);
}
