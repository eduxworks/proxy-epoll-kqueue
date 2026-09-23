/* buffer_pool.h — arena de slots para el relay (E16).
 *
 * El camino caliente no llama a malloc. Todos los buffers salen de una arena
 * reservada con mmap al arrancar y se devuelven a una lista libre: el coste de
 * prestar y devolver es constante y no fragmenta.
 *
 * El tamaño de slot es fijo, 16 KB, porque todos los usos son iguales: un
 * sentido del relay de una conexión.
 */
#ifndef PROXY_BUFFER_POOL_H
#define PROXY_BUFFER_POOL_H

#include <stddef.h>

#define BUFPOOL_SLOT_SIZE (16 * 1024)

typedef struct buffer_pool buffer_pool;

/* Reserva n_slots × 16 KB de una vez. NULL si el mmap falla. */
buffer_pool *bufpool_create(size_t n_slots);
void         bufpool_destroy(buffer_pool *p);

/* Presta un slot, o NULL si están todos fuera. Quedarse sin slots no es un
 * error del pool: es la señal de que hay que rechazar la conexión. */
void *bufpool_get(buffer_pool *p);
void  bufpool_put(buffer_pool *p, void *slot);

size_t bufpool_capacity(const buffer_pool *p);
size_t bufpool_available(const buffer_pool *p);

#endif /* PROXY_BUFFER_POOL_H */
