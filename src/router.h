/* router.h — resolución de Host: -> backend (E8) y publicación RCU (E13).
 *
 * Un router se construye desde una config ya validada y es inmutable. Recargar
 * es construir otro y publicarlo con un intercambio atómico: las conexiones en
 * vuelo conservan una referencia al viejo y terminan con la configuración con
 * la que empezaron.
 */
#ifndef PROXY_ROUTER_H
#define PROXY_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_pool.h"
#include "config.h"

typedef struct router router;

/* El router no copia la config: cfg debe vivir mientras el router exista.
 * router_destroy libera ambos, así que la config le pertenece. */
router *router_build(config *cfg);

/* Orden de resolución: exacto -> wildcard más específico -> default -> NULL.
 * NULL significa 502. host puede venir con puerto y en mayúsculas. */
backend_pool *router_lookup(const router *r, size_t frontend_index,
                            const char *host);

/* Recorrido de los pools, para el hilo de sondas activas (E11). */
size_t        router_pool_count(const router *r);
backend_pool *router_pool_at(const router *r, size_t i);

void router_ref(router *r);
void router_unref(router *r); /* libera router y config al llegar a 0 */

/* Normalización del header Host: quita ":puerto", pasa a minúsculas y rechaza
 * lo que no sea [a-z0-9.-]. false => 400. Expuesta porque es parte del
 * contrato de enrutado, no un detalle interno. */
bool router_normalize_host(const char *in, char *out, size_t outlen);

uint32_t router_hash_djb2(const char *s);

/* --- publicación RCU ----------------------------------------------------- */

/* El hueco donde vive el router activo. Lectores y publicador son el mismo
 * hilo (el event loop), que es lo que hace segura esta versión simple:
 * adquirir y publicar nunca se solapan. Si en el futuro un hilo ajeno
 * publicara, haría falta un esquema con épocas o punteros de riesgo. */
typedef struct router_slot router_slot;

router_slot *router_slot_create(router *initial);
void         router_slot_destroy(router_slot *slot);

/* Devuelve el router activo con una referencia tomada: hay que soltarla con
 * router_unref cuando la conexión termina. */
router *router_slot_acquire(router_slot *slot);

/* Publica uno nuevo y suelta la referencia del anterior. */
void router_slot_publish(router_slot *slot, router *next);

#endif /* PROXY_ROUTER_H */
