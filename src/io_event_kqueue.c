/* io_event_kqueue.c — implementación de io_event.h sobre kqueue (macOS/FreeBSD).
 *
 * Único fichero, junto a io_event_epoll.c, donde se permite código específico
 * de plataforma. Registra siempre con EV_CLEAR, el equivalente de EPOLLET.
 *
 * Diferencia estructural con epoll: kqueue no tiene una máscara por descriptor
 * sino un filtro por tipo de evento, así que cada fd ocupa dos entradas
 * (EVFILT_READ y EVFILT_WRITE) que se habilitan o deshabilitan por separado.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/types.h>
#include <unistd.h>

#include "io_event_internal.h"

#define IO_MAX_EVENTS 64

struct io_loop {
    int            kq;
    bool           running;
    io_watch_table watches;
};

/* Aplica la máscara como dos cambios: un filtro de lectura y uno de escritura,
 * cada uno habilitado o no. EV_CLEAR = edge-triggered. */
static int apply_mask(int kq, int fd, unsigned mask, bool adding)
{
    struct kevent ch[2];
    unsigned short base = (unsigned short)(adding ? EV_ADD : 0) | EV_CLEAR;

    EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ,
           (unsigned short)(base | ((mask & IO_READ) ? EV_ENABLE : EV_DISABLE)),
           0, 0, NULL);
    EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE,
           (unsigned short)(base | ((mask & IO_WRITE) ? EV_ENABLE : EV_DISABLE)),
           0, 0, NULL);

    return kevent(kq, ch, 2, NULL, 0, NULL);
}

io_loop *io_loop_create(void)
{
    io_loop *loop = calloc(1, sizeof *loop);
    if (loop == NULL) {
        return NULL;
    }

    loop->kq = kqueue();
    if (loop->kq < 0) {
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
    if (loop->kq >= 0) {
        close(loop->kq);
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
    if (apply_mask(loop->kq, fd, mask, true) < 0) {
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
    if (apply_mask(loop->kq, fd, mask, false) < 0) {
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

    struct kevent ch[2];
    EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

    /* Un fd ya cerrado desaparece solo de la kqueue: ENOENT no es un fallo. */
    if (kevent(loop->kq, ch, 2, NULL, 0, NULL) < 0 && errno != ENOENT) {
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

    struct kevent evs[IO_MAX_EVENTS];
    loop->running = true;

    while (loop->running) {
        int n = kevent(loop->kq, NULL, 0, evs, IO_MAX_EVENTS, NULL);
        if (n < 0) {
            if (errno == EINTR) {
                continue; /* una señal no es un error del bucle */
            }
            return -1;
        }

        for (int i = 0; i < n && loop->running; i++) {
            int       fd = (int)evs[i].ident;
            io_watch *w  = io_watch_at(&loop->watches, fd);
            if (w == NULL) {
                continue;
            }

            unsigned mask = 0;
            if (evs[i].filter == EVFILT_READ) {
                mask |= IO_READ;
            } else if (evs[i].filter == EVFILT_WRITE) {
                mask |= IO_WRITE;
            }
            if (evs[i].flags & EV_EOF) {
                mask |= IO_EOF;
            }
            if (evs[i].flags & EV_ERROR) {
                mask |= IO_ERR;
            }

            w->cb(fd, mask, w->ctx);
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
    return "kqueue";
}
