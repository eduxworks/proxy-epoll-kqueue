/* io_event_internal.h — estructuras que comparten las dos implementaciones.
 *
 * No es API pública: solo la usan io_event_epoll.c, io_event_kqueue.c y
 * io_event_common.c. Aquí tampoco hay nada específico de plataforma.
 */
#ifndef PROXY_IO_EVENT_INTERNAL_H
#define PROXY_IO_EVENT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "io_event.h"

typedef struct {
    io_cb    cb;
    void    *ctx;
    unsigned mask;
    bool     active;
} io_watch;

/* Tabla de observadores indexada por descriptor. POSIX garantiza que open()
 * devuelve el menor descriptor libre, así que los fd son enteros pequeños y
 * densos: un array directo da acceso O(1) sin hash ni colisiones. */
typedef struct {
    io_watch *v;
    size_t    n;
} io_watch_table;

/* Asegura sitio para fd, creciendo al doble. false si no hay memoria. */
bool io_watch_reserve(io_watch_table *t, int fd);

/* Observador de fd, o NULL si está fuera de rango o no está activo. */
io_watch *io_watch_at(io_watch_table *t, int fd);

void io_watch_table_free(io_watch_table *t);

#endif /* PROXY_IO_EVENT_INTERNAL_H */
