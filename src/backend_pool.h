/* backend_pool.h — elección de servidor de salida (E9, E10).
 *
 * El pool guarda el estado vivo que la config no tiene: quién está caído,
 * cuántas conexiones lleva cada uno y por dónde iba el reparto. La config es
 * inmutable; esto no.
 *
 * Salud pasiva (E11): cada intento de conexión reporta con pool_report(). Un
 * backend que acumula `fall` fallos seguidos sale del reparto sin esperar a la
 * sonda activa, que tarda `interval` en enterarse.
 */
#ifndef PROXY_BACKEND_POOL_H
#define PROXY_BACKEND_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef struct backend      backend;
typedef struct backend_pool backend_pool;

backend_pool *pool_create(const cfg_backend *cfg);
void          pool_destroy(backend_pool *p);

/* Devuelve el siguiente backend sano según el algoritmo configurado, o NULL si
 * no queda ninguno en pie — que es el 502. */
backend *pool_pick(backend_pool *p);

/* Resultado de un intento de conexión o de una sonda. Marca DOWN tras `fall`
 * fallos seguidos y devuelve a UP tras `rise` aciertos. */
void pool_report(backend_pool *p, backend *b, bool ok);

const cfg_backend *pool_config(const backend_pool *p);
size_t   pool_size(const backend_pool *p);
backend *pool_backend_at(backend_pool *p, size_t i);
size_t   pool_up_count(const backend_pool *p);

/* least_conn necesita saber cuántas conexiones vivas tiene cada backend, así
 * que la máquina de estados avisa al abrir y al cerrar. */
void backend_conn_opened(backend *b);
void backend_conn_closed(backend *b);

const char *backend_addr(const backend *b);
const char *backend_host(const backend *b);
uint16_t    backend_port(const backend *b);
bool        backend_is_up(const backend *b);
int         backend_active_conns(const backend *b);
int         backend_weight(const backend *b);

#endif /* PROXY_BACKEND_POOL_H */
