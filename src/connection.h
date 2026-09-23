/* connection.h — máquina de estados cliente <-> upstream (E4, E7, E14).
 *
 *   ACCEPTED -> READING_REQUEST -> CONNECTING -> PROXYING -> CLOSING
 *                     |               |             |
 *            error de parseo     sin backend     EOF de un lado
 *                     `-------------- 4xx / 502 --------------'
 *
 * Cada conexión toma una referencia al router al nacer y la suelta al morir:
 * es lo que permite recargar la configuración sin cortar lo que está en vuelo
 * (E13). Una conexión que empezó con la config vieja termina con la vieja.
 */
#ifndef PROXY_CONNECTION_H
#define PROXY_CONNECTION_H

#include <stddef.h>
#include <sys/socket.h>

#include "buffer_pool.h"
#include "io_event.h"
#include "router.h"

typedef struct conn_manager conn_manager;

conn_manager *conn_manager_create(io_loop *loop, router_slot *slot,
                                  buffer_pool *bufs);
void          conn_manager_destroy(conn_manager *m);

/* Toma posesión de client_fd: lo cerrará ella, pase lo que pase. */
void conn_accepted(conn_manager *m, int client_fd,
                   const struct sockaddr_storage *peer, size_t frontend_index);

size_t conn_manager_active(const conn_manager *m);

#endif /* PROXY_CONNECTION_H */
