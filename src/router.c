/* router.c — tabla de enrutado por Host y publicación RCU.
 *
 * Una tabla por frontend: dos frontends pueden servir el mismo dominio hacia
 * backends distintos, y el que escucha en un puerto no debe resolver los
 * dominios del otro.
 */

#include <ctype.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "router.h"

typedef struct route_entry {
    char               *host;
    const cfg_backend  *target;
    struct route_entry *next;
} route_entry;

typedef struct {
    char              *suffix; /* ".b.test" para el patrón "*.b.test" */
    size_t             labels; /* cuántas etiquetas: mide la especificidad */
    const cfg_backend *target;
} wildcard_entry;

typedef struct {
    route_entry      **buckets;
    size_t             nbuckets;
    route_entry       *entries; /* arena; los next apuntan dentro */
    size_t             n_entries;
    wildcard_entry    *wild;
    size_t             n_wild;
    const cfg_backend *fallback; /* ruta "default", o NULL */
} route_table;

struct router {
    atomic_int   refcount;
    config      *cfg;
    route_table *tables;
    size_t       n_tables;
};

struct router_slot {
    _Atomic(router *) current;
};

uint32_t router_hash_djb2(const char *s)
{
    uint32_t h = 5381;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        h = h * 33u + *p;
    }
    return h;
}

bool router_normalize_host(const char *in, char *out, size_t outlen)
{
    if (in == NULL || out == NULL || outlen == 0) {
        return false;
    }

    /* El puerto del header Host no participa en el enrutado: "a.test:8080" y
     * "a.test" son el mismo destino. */
    const char *end = strchr(in, ':');
    size_t      n   = end != NULL ? (size_t)(end - in) : strlen(in);

    if (n == 0 || n >= outlen) {
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        c               = (unsigned char)tolower(c);
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '-';
        if (!ok) {
            return false;
        }
        out[i] = (char)c;
    }
    out[n] = '\0';
    return true;
}

static size_t count_labels(const char *suffix)
{
    size_t n = 0;
    for (const char *p = suffix; *p != '\0'; p++) {
        if (*p == '.') {
            n++;
        }
    }
    return n;
}

static size_t next_pow2(size_t want)
{
    size_t n = 8;
    while (n < want) {
        n *= 2;
    }
    return n;
}

static void table_free(route_table *t)
{
    for (size_t i = 0; i < t->n_entries; i++) {
        free(t->entries[i].host);
    }
    free(t->entries);
    free(t->buckets);
    for (size_t i = 0; i < t->n_wild; i++) {
        free(t->wild[i].suffix);
    }
    free(t->wild);
}

/* Ordena wildcards de más a menos específico, para que "*.a.b.test" gane a
 * "*.b.test" cuando ambos encajan. Son pocos: una inserción directa basta. */
static void sort_wildcards(wildcard_entry *w, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        wildcard_entry key = w[i];
        size_t         j   = i;
        while (j > 0 && w[j - 1].labels < key.labels) {
            w[j] = w[j - 1];
            j--;
        }
        w[j] = key;
    }
}

static bool table_build(route_table *t, const cfg_frontend *f, const config *cfg)
{
    size_t n_exact = 0, n_wild = 0;
    for (size_t i = 0; i < f->n_routes; i++) {
        const char *h = f->routes[i].host;
        if (strcmp(h, "default") == 0) {
            continue;
        }
        if (h[0] == '*') {
            n_wild++;
        } else {
            n_exact++;
        }
    }

    t->nbuckets = next_pow2(n_exact * 2 + 1);
    t->buckets  = calloc(t->nbuckets, sizeof *t->buckets);
    t->entries  = n_exact > 0 ? calloc(n_exact, sizeof *t->entries) : NULL;
    t->wild     = n_wild > 0 ? calloc(n_wild, sizeof *t->wild) : NULL;
    if (t->buckets == NULL || (n_exact > 0 && t->entries == NULL) ||
        (n_wild > 0 && t->wild == NULL)) {
        return false;
    }

    for (size_t i = 0; i < f->n_routes; i++) {
        const cfg_route   *r      = &f->routes[i];
        const cfg_backend *target = &cfg->backends[r->backend_index];

        if (strcmp(r->host, "default") == 0) {
            t->fallback = target;
            continue;
        }

        if (r->host[0] == '*') {
            wildcard_entry *w = &t->wild[t->n_wild++];
            w->suffix         = strdup(r->host + 1); /* "*.b.test" -> ".b.test" */
            if (w->suffix == NULL) {
                return false;
            }
            w->labels = count_labels(w->suffix);
            w->target = target;
            continue;
        }

        route_entry *e = &t->entries[t->n_entries++];
        e->host        = strdup(r->host);
        if (e->host == NULL) {
            return false;
        }
        e->target = target;

        size_t b   = router_hash_djb2(e->host) & (t->nbuckets - 1);
        e->next    = t->buckets[b];
        t->buckets[b] = e;
    }

    sort_wildcards(t->wild, t->n_wild);
    return true;
}

router *router_build(config *cfg)
{
    if (cfg == NULL) {
        return NULL;
    }

    router *r = calloc(1, sizeof *r);
    if (r == NULL) {
        return NULL;
    }

    r->tables = calloc(cfg->n_frontends, sizeof *r->tables);
    if (r->tables == NULL) {
        free(r);
        return NULL;
    }
    r->n_tables = cfg->n_frontends;
    r->cfg      = cfg;
    atomic_init(&r->refcount, 1);

    for (size_t i = 0; i < cfg->n_frontends; i++) {
        if (!table_build(&r->tables[i], &cfg->frontends[i], cfg)) {
            for (size_t j = 0; j <= i; j++) {
                table_free(&r->tables[j]);
            }
            free(r->tables);
            free(r);
            return NULL;
        }
    }

    return r;
}

const cfg_backend *router_lookup(const router *r, size_t frontend_index,
                                 const char *host)
{
    if (r == NULL || frontend_index >= r->n_tables || host == NULL) {
        return NULL;
    }

    const route_table *t = &r->tables[frontend_index];

    char norm[256];
    if (!router_normalize_host(host, norm, sizeof norm)) {
        return NULL;
    }

    /* 1. coincidencia exacta */
    size_t b = router_hash_djb2(norm) & (t->nbuckets - 1);
    for (const route_entry *e = t->buckets[b]; e != NULL; e = e->next) {
        if (strcmp(e->host, norm) == 0) {
            return e->target;
        }
    }

    /* 2. wildcard más específico: la lista ya está ordenada */
    size_t hlen = strlen(norm);
    for (size_t i = 0; i < t->n_wild; i++) {
        size_t slen = strlen(t->wild[i].suffix);
        /* "*.b.test" no debe casar con "b.test": hace falta algo delante. */
        if (hlen > slen && strcmp(norm + (hlen - slen), t->wild[i].suffix) == 0) {
            return t->wild[i].target;
        }
    }

    /* 3. default, o NULL => 502 */
    return t->fallback;
}

void router_ref(router *r)
{
    if (r != NULL) {
        atomic_fetch_add_explicit(&r->refcount, 1, memory_order_relaxed);
    }
}

void router_unref(router *r)
{
    if (r == NULL) {
        return;
    }
    if (atomic_fetch_sub_explicit(&r->refcount, 1, memory_order_acq_rel) != 1) {
        return;
    }

    for (size_t i = 0; i < r->n_tables; i++) {
        table_free(&r->tables[i]);
    }
    free(r->tables);
    config_free(r->cfg);
    free(r);
}

/* --- publicación RCU ----------------------------------------------------- */

router_slot *router_slot_create(router *initial)
{
    router_slot *s = calloc(1, sizeof *s);
    if (s == NULL) {
        return NULL;
    }
    atomic_init(&s->current, initial);
    return s;
}

void router_slot_destroy(router_slot *slot)
{
    if (slot == NULL) {
        return;
    }
    router *r = atomic_exchange_explicit(&slot->current, NULL, memory_order_acq_rel);
    router_unref(r);
    free(slot);
}

router *router_slot_acquire(router_slot *slot)
{
    if (slot == NULL) {
        return NULL;
    }
    router *r = atomic_load_explicit(&slot->current, memory_order_acquire);
    router_ref(r);
    return r;
}

void router_slot_publish(router_slot *slot, router *next)
{
    if (slot == NULL) {
        return;
    }
    router *old = atomic_exchange_explicit(&slot->current, next, memory_order_acq_rel);
    /* No se libera aquí si hay conexiones en vuelo: cada una tiene su
     * referencia y la última en terminar es la que libera de verdad. */
    router_unref(old);
}
