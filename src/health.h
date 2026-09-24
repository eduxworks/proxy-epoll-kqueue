/* health.h — sondas activas en hilo aparte (E11).
 *
 * La salud pasiva solo se entera cuando un cliente real tropieza con un
 * backend caído. Las sondas activas lo descubren antes y, sobre todo, son lo
 * único que permite que un backend **vuelva**: sin ellas, un pool cuyos
 * miembros caen todos a la vez se queda muerto para siempre, porque al no
 * haber a quién enrutar tampoco hay intentos que puedan tener éxito.
 *
 * Corre en su propio hilo porque una sonda espera: conectar a una máquina
 * apagada tarda lo que tarde el timeout, y eso no puede ocurrir dentro del
 * bucle de eventos.
 */
#ifndef PROXY_HEALTH_H
#define PROXY_HEALTH_H

#include "router.h"

typedef struct health health;

/* Toma una referencia al router; la suelta al parar. NULL si falla. */
health *health_start(router *initial);

/* Tras un reload: el hilo pasa a sondear los pools nuevos. Lo llama el bucle
 * de eventos; el relevo va bajo un cerrojo que solo se toca una vez por
 * recarga y otra por ciclo de sondeo, nunca en el camino caliente. */
void health_set_router(health *h, router *next);

void health_stop(health *h);

/* Ciclos completados. Sirve para que un test espere a que haya sondeado de
 * verdad en vez de dormir a ojo. */
unsigned long health_cycles(const health *h);

#endif /* PROXY_HEALTH_H */
