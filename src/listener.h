/* listener.h — socket de escucha por frontend (E5, E6).
 *
 * Un listener por [[frontend]], con SO_REUSEPORT para que cada worker abra el
 * suyo sobre el mismo puerto. El índice de frontend viaja con el callback
 * porque es lo que luego elige la tabla de rutas: el aislamiento entre
 * frontends empieza aquí.
 */
#ifndef PROXY_LISTENER_H
#define PROXY_LISTENER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "config.h"
#include "io_event.h"

typedef struct listener listener;

typedef void (*listener_cb)(int client_fd, const struct sockaddr_storage *peer,
                            size_t frontend_index, void *ctx);

/* Abre, configura y pone a escuchar. NULL y err relleno si algo falla. */
listener *listener_open(const cfg_frontend *f, size_t frontend_index, char *err,
                        size_t errlen);

void listener_close(listener *l);

int      listener_fd(const listener *l);
uint16_t listener_port(const listener *l); /* el real, tras el bind */

/* Registra el listener en el bucle. El handler drena accept hasta EAGAIN. */
int listener_attach(listener *l, io_loop *loop, listener_cb cb, void *ctx);

#endif /* PROXY_LISTENER_H */
