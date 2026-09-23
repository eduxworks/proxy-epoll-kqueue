/* net_compat.h — las diferencias de sockets entre Linux, FreeBSD y macOS.
 *
 * Igual que io_event_*.c aísla epoll de kqueue, este módulo aísla las dos
 * llamadas de red que no son iguales en los tres sistemas. Fuera de
 * net_compat.c no hay ningún #ifdef de plataforma.
 */
#ifndef PROXY_NET_COMPAT_H
#define PROXY_NET_COMPAT_H

#include <stdbool.h>
#include <sys/socket.h>
#include <sys/types.h>

/* Permite que varios procesos escuchen el mismo puerto (E5).
 * Ojo: la opción no significa lo mismo en todas partes, ver
 * net_reuseport_balances(). Devuelve 0 o -1 (errno). */
int net_set_reuseport(int fd);

/* Nombre de la opción realmente usada: "SO_REUSEPORT" o "SO_REUSEPORT_LB". */
const char *net_reuseport_name(void);

/* true si el kernel reparte las conexiones entrantes entre los procesos que
 * comparten el puerto. En macOS es false: la opción permite el bind múltiple
 * pero no balancea, así que allí un worker por CPU no basta y hace falta un
 * aceptador que distribuya. */
bool net_reuseport_balances(void);

/* accept con O_NONBLOCK y FD_CLOEXEC, en una sola llamada donde el sistema la
 * ofrece (accept4) y en tres donde no (macOS). Devuelve el fd o -1 (errno). */
int net_accept(int lfd, struct sockaddr_storage *peer, socklen_t *plen);

#endif /* PROXY_NET_COMPAT_H */
