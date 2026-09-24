/* stats.h — snapshot JSON por socket UNIX (E17).
 *
 * Se sirve desde el bucle de eventos y sin bloquear, como todo lo demás: un
 * endpoint de diagnóstico que frene el proxy cuando alguien lo consulta sería
 * peor que no tenerlo.
 *
 * Limitación consciente: cada worker es un proceso con su propio estado y un
 * socket UNIX solo lo puede escuchar uno, así que lo publica el worker 0 y las
 * cifras son las suyas, no la suma de todos. Agregarlas exigiría memoria
 * compartida entre procesos, que es justo lo que este diseño evita para no
 * tener contención en el camino caliente.
 */
#ifndef PROXY_STATS_H
#define PROXY_STATS_H

#include <stddef.h>
#include <time.h>

#include "connection.h"
#include "io_event.h"
#include "router.h"

typedef struct stats stats;

/* Abre y registra el socket. NULL si falla (y deja errno). */
stats *stats_open(const char *path, io_loop *loop, router_slot *slot,
                  conn_manager *conns, int worker_id, time_t started);

void stats_close(stats *s);

/* Genera el JSON. Expuesta para poder comprobarla sin levantar un socket.
 * Devuelve los bytes escritos (sin el NUL), o 0 si no cabe. */
size_t stats_render(char *out, size_t outlen, router *r, const conn_manager *conns,
                    int worker_id, time_t started);

#endif /* PROXY_STATS_H */
