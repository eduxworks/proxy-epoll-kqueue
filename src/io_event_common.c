/* io_event_common.c — parte de la abstracción que es idéntica en las tres
 * plataformas: la tabla de observadores y O_NONBLOCK. Sin #ifdef. */

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "io_event_internal.h"

bool io_watch_reserve(io_watch_table *t, int fd)
{
    if (fd < 0) {
        return false;
    }

    size_t need = (size_t)fd + 1;
    if (need <= t->n) {
        return true;
    }

    size_t cap = t->n ? t->n : 64;
    while (cap < need) {
        cap *= 2;
    }

    io_watch *v = realloc(t->v, cap * sizeof *v);
    if (v == NULL) {
        return false;
    }

    memset(v + t->n, 0, (cap - t->n) * sizeof *v);
    t->v = v;
    t->n = cap;
    return true;
}

io_watch *io_watch_at(io_watch_table *t, int fd)
{
    if (fd < 0 || (size_t)fd >= t->n || !t->v[fd].active) {
        return NULL;
    }
    return &t->v[fd];
}

void io_watch_table_free(io_watch_table *t)
{
    free(t->v);
    t->v = NULL;
    t->n = 0;
}

int io_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if ((flags & O_NONBLOCK) != 0) {
        return 0;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
