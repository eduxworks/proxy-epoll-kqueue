/* backend_pool.c — round_robin, weighted y least_conn con exclusión de caídos. */

#include <stdlib.h>
#include <string.h>

#include "backend_pool.h"

struct backend {
    const cfg_server *cfg;
    bool              up;
    int               fails;     /* fallos consecutivos */
    int               successes; /* aciertos consecutivos estando DOWN */
    int               active;    /* conexiones vivas, para least_conn */
    int               current;   /* peso dinámico del weighted suave */
};

struct backend_pool {
    const cfg_backend *cfg;
    backend           *backends;
    size_t             n;
    size_t             rr; /* por dónde iba el reparto */
};

backend_pool *pool_create(const cfg_backend *cfg)
{
    if (cfg == NULL || cfg->n_servers == 0) {
        return NULL;
    }

    backend_pool *p = calloc(1, sizeof *p);
    if (p == NULL) {
        return NULL;
    }

    p->backends = calloc(cfg->n_servers, sizeof *p->backends);
    if (p->backends == NULL) {
        free(p);
        return NULL;
    }

    p->cfg = cfg;
    p->n   = cfg->n_servers;

    for (size_t i = 0; i < p->n; i++) {
        /* Se arranca suponiendo que están vivos: si no lo están, el primer
         * intento fallido lo descubre antes que cualquier sonda. */
        p->backends[i].cfg = &cfg->servers[i];
        p->backends[i].up  = true;
    }

    return p;
}

void pool_destroy(backend_pool *p)
{
    if (p == NULL) {
        return;
    }
    free(p->backends);
    free(p);
}

size_t pool_size(const backend_pool *p)
{
    return p != NULL ? p->n : 0;
}

backend *pool_backend_at(backend_pool *p, size_t i)
{
    if (p == NULL || i >= p->n) {
        return NULL;
    }
    return &p->backends[i];
}

size_t pool_up_count(const backend_pool *p)
{
    if (p == NULL) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < p->n; i++) {
        if (p->backends[i].up) {
            n++;
        }
    }
    return n;
}

/* --- algoritmos ---------------------------------------------------------- */

static backend *pick_round_robin(backend_pool *p)
{
    /* Ignora los pesos a propósito: para eso está "weighted". */
    for (size_t k = 0; k < p->n; k++) {
        size_t   i = (p->rr + k) % p->n;
        backend *b = &p->backends[i];
        if (b->up) {
            p->rr = (i + 1) % p->n;
            return b;
        }
    }
    return NULL;
}

/* Weighted suave (el de nginx): en vez de soltar N seguidas al de peso N,
 * intercala. Con pesos 3:1 da A A B A en lugar de A A A B, que reparte mejor
 * la latencia. */
static backend *pick_weighted(backend_pool *p)
{
    backend *best  = NULL;
    int      total = 0;

    for (size_t i = 0; i < p->n; i++) {
        backend *b = &p->backends[i];
        if (!b->up) {
            continue;
        }
        b->current += b->cfg->weight;
        total += b->cfg->weight;
        if (best == NULL || b->current > best->current) {
            best = b;
        }
    }

    if (best != NULL) {
        best->current -= total;
    }
    return best;
}

static backend *pick_least_conn(backend_pool *p)
{
    backend *best   = NULL;
    size_t   best_i = 0;

    /* El punto de partida se fija ANTES del bucle: si se tocara p->rr mientras
     * se recorre, el índice calculado se movería bajo los pies y habría
     * backends que no se llegan a mirar. */
    size_t start = p->rr;

    for (size_t k = 0; k < p->n; k++) {
        size_t   i = (start + k) % p->n;
        backend *b = &p->backends[i];
        if (!b->up) {
            continue;
        }
        /* Estrictamente menor: a igualdad gana el primero desde el punto de
         * partida, y como este avanza, los empates rotan en vez de recaer
         * siempre en el mismo. */
        if (best == NULL || b->active < best->active) {
            best   = b;
            best_i = i;
        }
    }

    if (best != NULL) {
        p->rr = (best_i + 1) % p->n;
    }
    return best;
}

backend *pool_pick(backend_pool *p)
{
    if (p == NULL || p->n == 0) {
        return NULL;
    }

    switch (p->cfg->balance) {
    case BALANCE_WEIGHTED:
        return pick_weighted(p);
    case BALANCE_LEAST_CONN:
        return pick_least_conn(p);
    case BALANCE_ROUND_ROBIN:
    default:
        return pick_round_robin(p);
    }
}

void pool_report(backend_pool *p, backend *b, bool ok)
{
    if (p == NULL || b == NULL) {
        return;
    }

    if (ok) {
        b->fails = 0;
        if (!b->up && ++b->successes >= p->cfg->health.rise) {
            b->up        = true;
            b->successes = 0;
            /* Entra limpio al reparto: arrastrar el peso dinámico de antes de
             * caerse le daría una ráfaga injusta al volver. */
            b->current = 0;
        }
        return;
    }

    b->successes = 0;
    if (b->up && ++b->fails >= p->cfg->health.fall) {
        b->up    = false;
        b->fails = 0;
    }
}

/* --- estado por backend -------------------------------------------------- */

void backend_conn_opened(backend *b)
{
    if (b != NULL) {
        b->active++;
    }
}

void backend_conn_closed(backend *b)
{
    if (b != NULL && b->active > 0) {
        b->active--;
    }
}

const char *backend_addr(const backend *b)
{
    return b != NULL ? b->cfg->addr : NULL;
}

bool backend_is_up(const backend *b)
{
    return b != NULL && b->up;
}

int backend_active_conns(const backend *b)
{
    return b != NULL ? b->active : 0;
}

int backend_weight(const backend *b)
{
    return b != NULL ? b->cfg->weight : 0;
}

const cfg_backend *pool_config(const backend_pool *p)
{
    return p != NULL ? p->cfg : NULL;
}

const char *backend_host(const backend *b)
{
    return b != NULL ? b->cfg->host : NULL;
}

uint16_t backend_port(const backend *b)
{
    return b != NULL ? b->cfg->port : 0;
}
