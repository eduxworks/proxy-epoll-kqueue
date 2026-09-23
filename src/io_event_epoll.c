/* io_event_epoll.c — implementación de io_event.h sobre epoll (Linux).
 *
 * Único fichero, junto a io_event_kqueue.c, donde se permite código específico
 * de plataforma. Registra siempre en modo edge-triggered (EPOLLET).
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "io_event_internal.h"

#define IO_MAX_EVENTS 64

struct io_loop {
    int            epfd;
    bool           running;
    io_watch_table watches;
};

/* La máscara portable se traduce a flags de epoll. EPOLLET es innegociable:
 * es lo que obliga a drenar hasta EAGAIN en los handlers. */
static uint32_t to_epoll(unsigned mask)
{
    uint32_t ev = EPOLLET | EPOLLRDHUP;
    if (mask & IO_READ) {
        ev |= EPOLLIN;
    }
    if (mask & IO_WRITE) {
        ev |= EPOLLOUT;
    }
    return ev;
}

static unsigned from_epoll(uint32_t ev)
{
    unsigned mask = 0;
    if (ev & EPOLLIN) {
        mask |= IO_READ;
    }
    if (ev & EPOLLOUT) {
        mask |= IO_WRITE;
    }
    if (ev & (EPOLLRDHUP | EPOLLHUP)) {
        mask |= IO_EOF;
    }
    if (ev & EPOLLERR) {
        mask |= IO_ERR;
    }
    return mask;
}

io_loop *io_loop_create(void)
{
    io_loop *loop = calloc(1, sizeof *loop);
    if (loop == NULL) {
        return NULL;
    }

    loop->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epfd < 0) {
        int saved = errno;
        free(loop);
        errno = saved;
        return NULL;
    }

    loop->running = false;
    return loop;
}

void io_loop_destroy(io_loop *loop)
{
    if (loop == NULL) {
        return;
    }
    if (loop->epfd >= 0) {
        close(loop->epfd);
    }
    io_watch_table_free(&loop->watches);
    free(loop);
}

int io_loop_add(io_loop *loop, int fd, unsigned mask, io_cb cb, void *ctx)
{
    if (loop == NULL || cb == NULL || fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (!io_watch_reserve(&loop->watches, fd)) {
        errno = ENOMEM;
        return -1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events  = to_epoll(mask);
    ev.data.fd = fd; /* el fd, no un puntero: la tabla se realoca al crecer */

    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        return -1;
    }

    io_watch *w = &loop->watches.v[fd];
    w->cb     = cb;
    w->ctx    = ctx;
    w->mask   = mask;
    w->active = true;
    return 0;
}

int io_loop_mod(io_loop *loop, int fd, unsigned mask)
{
    if (loop == NULL) {
        errno = EINVAL;
        return -1;
    }
    io_watch *w = io_watch_at(&loop->watches, fd);
    if (w == NULL) {
        errno = ENOENT;
        return -1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events  = to_epoll(mask);
    ev.data.fd = fd;

    if (epoll_ctl(loop->epfd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        return -1;
    }

    w->mask = mask;
    return 0;
}

int io_loop_del(io_loop *loop, int fd)
{
    if (loop == NULL) {
        errno = EINVAL;
        return -1;
    }
    io_watch *w = io_watch_at(&loop->watches, fd);
    if (w == NULL) {
        errno = ENOENT;
        return -1;
    }

    /* Un fd ya cerrado sale solo del conjunto: ENOENT aquí no es un fallo. */
    if (epoll_ctl(loop->epfd, EPOLL_CTL_DEL, fd, NULL) < 0 && errno != ENOENT) {
        return -1;
    }

    memset(w, 0, sizeof *w);
    return 0;
}

int io_loop_run(io_loop *loop)
{
    if (loop == NULL) {
        errno = EINVAL;
        return -1;
    }

    struct epoll_event evs[IO_MAX_EVENTS];
    loop->running = true;

    while (loop->running) {
        int n = epoll_wait(loop->epfd, evs, IO_MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue; /* una señal no es un error del bucle */
            }
            return -1;
        }

        for (int i = 0; i < n && loop->running; i++) {
            int       fd = evs[i].data.fd;
            io_watch *w  = io_watch_at(&loop->watches, fd);
            if (w != NULL) {
                w->cb(fd, from_epoll(evs[i].events), w->ctx);
            }
        }
    }

    return 0;
}

void io_loop_stop(io_loop *loop)
{
    if (loop != NULL) {
        loop->running = false;
    }
}

const char *io_backend_name(void)
{
    return "epoll";
}
