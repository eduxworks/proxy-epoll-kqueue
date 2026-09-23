/* config.c — parser y validador de la configuración TOML (E12).
 *
 * Criterio: la validación es estricta y falla entera. Un reload solo puede
 * publicar una config completa y coherente (E13); media configuración aplicada
 * es peor que ninguna.
 *
 * Todo error cita la ruta TOML donde está el problema —
 * "frontend[1].route[0]: ..." — porque un mensaje sin ubicación obliga a
 * adivinar en un fichero de cien líneas.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "toml.h"

#include "config.h"

#define DEFAULT_MAX_CONNS    65536
#define DEFAULT_HEALTH_INTERVAL 2000
#define DEFAULT_HEALTH_TIMEOUT   500
#define DEFAULT_HEALTH_RISE        2
#define DEFAULT_HEALTH_FALL        3

static void seterr(char *err, size_t errlen, const char *fmt, ...)
{
    if (err == NULL || errlen == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static char *dup_str(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char  *p = malloc(n);
    if (p != NULL) {
        memcpy(p, s, n);
    }
    return p;
}

/* toml_string_in devuelve memoria ya reservada: la adoptamos tal cual. */
static char *take_string(toml_table_t *t, const char *key)
{
    toml_datum_t d = toml_string_in(t, key);
    return d.ok ? d.u.s : NULL;
}

static bool take_int(toml_table_t *t, const char *key, int64_t *out)
{
    toml_datum_t d = toml_int_in(t, key);
    if (!d.ok) {
        return false;
    }
    *out = d.u.i;
    return true;
}

/* "127.0.0.1:9001" -> host + puerto. Sin soporte IPv6: fuera de alcance. */
static bool split_addr(const char *addr, char **host, uint16_t *port)
{
    const char *colon = strrchr(addr, ':');
    if (colon == NULL || colon == addr || colon[1] == '\0') {
        return false;
    }

    for (const char *p = colon + 1; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }

    long value = strtol(colon + 1, NULL, 10);
    if (value < 1 || value > 65535) {
        return false;
    }

    size_t hostlen = (size_t)(colon - addr);
    char  *h       = malloc(hostlen + 1);
    if (h == NULL) {
        return false;
    }
    memcpy(h, addr, hostlen);
    h[hostlen] = '\0';

    *host = h;
    *port = (uint16_t)value;
    return true;
}

/* Un wildcard válido es "*.dominio": un solo '*', al principio, seguido de '.'
 * y de al menos una etiqueta. */
static bool valid_wildcard(const char *host)
{
    if (host[0] != '*' || host[1] != '.' || host[2] == '\0') {
        return false;
    }
    return strchr(host + 1, '*') == NULL;
}

static bool valid_hostname(const char *host)
{
    if (host[0] == '\0') {
        return false;
    }
    for (const char *p = host; *p != '\0'; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '.' || *p == '-';
        if (!ok) {
            return false;
        }
    }
    return strchr(host, '*') == NULL;
}

static void free_backend(cfg_backend *b)
{
    for (size_t i = 0; i < b->n_servers; i++) {
        free(b->servers[i].addr);
        free(b->servers[i].host);
    }
    free(b->servers);
    free(b->name);
    free(b->health.path);
}

static void free_frontend(cfg_frontend *f)
{
    for (size_t i = 0; i < f->n_routes; i++) {
        free(f->routes[i].host);
        free(f->routes[i].backend_name);
    }
    free(f->routes);
    free(f->name);
    free(f->listen);
    free(f->listen_host);
}

void config_free(config *cfg)
{
    if (cfg == NULL) {
        return;
    }
    for (size_t i = 0; i < cfg->n_backends; i++) {
        free_backend(&cfg->backends[i]);
    }
    for (size_t i = 0; i < cfg->n_frontends; i++) {
        free_frontend(&cfg->frontends[i]);
    }
    free(cfg->backends);
    free(cfg->frontends);
    free(cfg->global.stats_socket);
    free(cfg->global.log_file);
    free(cfg->global.log_level);
    free(cfg);
}

const cfg_backend *config_find_backend(const config *cfg, const char *name)
{
    for (size_t i = 0; i < cfg->n_backends; i++) {
        if (strcmp(cfg->backends[i].name, name) == 0) {
            return &cfg->backends[i];
        }
    }
    return NULL;
}

/* --- [global] ------------------------------------------------------------ */

static bool parse_global(toml_table_t *root, config *cfg, char *err, size_t errlen)
{
    cfg->global.max_conns = DEFAULT_MAX_CONNS;

    toml_table_t *g = toml_table_in(root, "global");
    if (g == NULL) {
        return true; /* todo opcional: los valores por defecto sirven */
    }

    int64_t v;
    if (take_int(g, "workers", &v)) {
        if (v < 0) {
            seterr(err, errlen, "global.workers: debe ser >= 0 (0 = una por CPU)");
            return false;
        }
        cfg->global.workers = (int)v;
    }
    if (take_int(g, "max_conns", &v)) {
        if (v < 1) {
            seterr(err, errlen, "global.max_conns: debe ser >= 1");
            return false;
        }
        cfg->global.max_conns = (int)v;
    }

    cfg->global.stats_socket = take_string(g, "stats_socket");
    cfg->global.log_file     = take_string(g, "log_file");
    cfg->global.log_level    = take_string(g, "log_level");

    if (cfg->global.log_level != NULL) {
        const char *lv = cfg->global.log_level;
        if (strcmp(lv, "debug") != 0 && strcmp(lv, "info") != 0 &&
            strcmp(lv, "warn") != 0 && strcmp(lv, "error") != 0) {
            seterr(err, errlen,
                   "global.log_level: \"%s\" no es debug|info|warn|error", lv);
            return false;
        }
    }
    return true;
}

/* --- [[backend]] --------------------------------------------------------- */

static bool parse_health(toml_table_t *bt, cfg_backend *b, size_t bi,
                         char *err, size_t errlen)
{
    b->health.type        = HEALTH_TCP;
    b->health.interval_ms = DEFAULT_HEALTH_INTERVAL;
    b->health.timeout_ms  = DEFAULT_HEALTH_TIMEOUT;
    b->health.rise        = DEFAULT_HEALTH_RISE;
    b->health.fall        = DEFAULT_HEALTH_FALL;

    toml_table_t *h = toml_table_in(bt, "health");
    if (h == NULL) {
        return true;
    }

    char *type = take_string(h, "type");
    if (type != NULL) {
        if (strcmp(type, "tcp") == 0) {
            b->health.type = HEALTH_TCP;
        } else if (strcmp(type, "http") == 0) {
            b->health.type = HEALTH_HTTP;
        } else {
            seterr(err, errlen, "backend[%zu].health.type: \"%s\" no es tcp|http",
                   bi, type);
            free(type);
            return false;
        }
        free(type);
    }

    b->health.path = take_string(h, "path");

    int64_t v;
    if (take_int(h, "interval", &v)) {
        b->health.interval_ms = (int)v;
    }
    if (take_int(h, "timeout", &v)) {
        b->health.timeout_ms = (int)v;
    }
    if (take_int(h, "rise", &v)) {
        b->health.rise = (int)v;
    }
    if (take_int(h, "fall", &v)) {
        b->health.fall = (int)v;
    }

    if (b->health.interval_ms <= 0 || b->health.timeout_ms <= 0 ||
        b->health.rise <= 0 || b->health.fall <= 0) {
        seterr(err, errlen,
               "backend[%zu].health: interval, timeout, rise y fall deben ser > 0", bi);
        return false;
    }
    /* Sondar más rápido de lo que se tarda en rendirse solapa sondas sobre el
     * mismo backend y falsea el conteo de rise/fall. */
    if (b->health.timeout_ms >= b->health.interval_ms) {
        seterr(err, errlen, "backend[%zu].health: timeout (%d) debe ser < interval (%d)",
               bi, b->health.timeout_ms, b->health.interval_ms);
        return false;
    }
    return true;
}

static bool parse_servers(toml_table_t *bt, cfg_backend *b, size_t bi,
                          char *err, size_t errlen)
{
    toml_array_t *arr = toml_array_in(bt, "servers");
    if (arr == NULL) {
        seterr(err, errlen, "backend[%zu] (\"%s\"): falta la lista servers", bi,
               b->name ? b->name : "?");
        return false;
    }

    int n = toml_array_nelem(arr);
    if (n < 1) {
        seterr(err, errlen, "backend[%zu] (\"%s\"): servers no puede estar vacío",
               bi, b->name ? b->name : "?");
        return false;
    }

    b->servers = calloc((size_t)n, sizeof *b->servers);
    if (b->servers == NULL) {
        seterr(err, errlen, "sin memoria");
        return false;
    }
    b->n_servers = (size_t)n;

    for (int i = 0; i < n; i++) {
        toml_table_t *st = toml_table_at(arr, i);
        if (st == NULL) {
            seterr(err, errlen, "backend[%zu].servers[%d]: debe ser una tabla {addr=...}",
                   bi, i);
            return false;
        }

        cfg_server *s = &b->servers[i];
        s->addr       = take_string(st, "addr");
        if (s->addr == NULL) {
            seterr(err, errlen, "backend[%zu].servers[%d]: falta addr", bi, i);
            return false;
        }
        if (!split_addr(s->addr, &s->host, &s->port)) {
            seterr(err, errlen,
                   "backend[%zu].servers[%d]: addr \"%s\" no tiene forma IP:puerto",
                   bi, i, s->addr);
            return false;
        }

        int64_t w = 1;
        take_int(st, "weight", &w);
        if (w < 1) {
            seterr(err, errlen, "backend[%zu].servers[%d]: weight debe ser >= 1", bi, i);
            return false;
        }
        s->weight = (int)w;
    }
    return true;
}

static bool parse_backends(toml_table_t *root, config *cfg, char *err, size_t errlen)
{
    toml_array_t *arr = toml_array_in(root, "backend");
    if (arr == NULL || toml_array_nelem(arr) < 1) {
        seterr(err, errlen, "hace falta al menos un [[backend]]");
        return false;
    }

    int n      = toml_array_nelem(arr);
    cfg->backends = calloc((size_t)n, sizeof *cfg->backends);
    if (cfg->backends == NULL) {
        seterr(err, errlen, "sin memoria");
        return false;
    }
    cfg->n_backends = (size_t)n;

    for (int i = 0; i < n; i++) {
        cfg_backend  *b  = &cfg->backends[i];
        toml_table_t *bt = toml_table_at(arr, i);
        if (bt == NULL) {
            seterr(err, errlen, "backend[%d]: entrada mal formada", i);
            return false;
        }

        b->name = take_string(bt, "name");
        if (b->name == NULL) {
            seterr(err, errlen, "backend[%d]: falta name", i);
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(cfg->backends[j].name, b->name) == 0) {
                seterr(err, errlen, "backend[%d]: name \"%s\" duplicado (ya en backend[%d])",
                       i, b->name, j);
                return false;
            }
        }

        char *balance = take_string(bt, "balance");
        if (balance != NULL) {
            if (strcmp(balance, "round_robin") == 0) {
                b->balance = BALANCE_ROUND_ROBIN;
            } else if (strcmp(balance, "weighted") == 0) {
                b->balance = BALANCE_WEIGHTED;
            } else if (strcmp(balance, "least_conn") == 0) {
                b->balance = BALANCE_LEAST_CONN;
            } else {
                seterr(err, errlen,
                       "backend[%d].balance: \"%s\" no es round_robin|weighted|least_conn",
                       i, balance);
                free(balance);
                return false;
            }
            free(balance);
        }

        if (!parse_servers(bt, b, (size_t)i, err, errlen)) {
            return false;
        }
        if (!parse_health(bt, b, (size_t)i, err, errlen)) {
            return false;
        }
    }
    return true;
}

/* --- [[frontend]] -------------------------------------------------------- */

static bool parse_routes(toml_table_t *ft, cfg_frontend *f, size_t fi,
                         const config *cfg, char *err, size_t errlen)
{
    toml_array_t *arr = toml_array_in(ft, "route");
    if (arr == NULL || toml_array_nelem(arr) < 1) {
        seterr(err, errlen, "frontend[%zu] (\"%s\"): hace falta al menos una [[frontend.route]]",
               fi, f->name ? f->name : "?");
        return false;
    }

    int n     = toml_array_nelem(arr);
    f->routes = calloc((size_t)n, sizeof *f->routes);
    if (f->routes == NULL) {
        seterr(err, errlen, "sin memoria");
        return false;
    }
    f->n_routes = (size_t)n;

    for (int i = 0; i < n; i++) {
        cfg_route    *r  = &f->routes[i];
        toml_table_t *rt = toml_table_at(arr, i);
        if (rt == NULL) {
            seterr(err, errlen, "frontend[%zu].route[%d]: entrada mal formada", fi, i);
            return false;
        }

        r->host = take_string(rt, "host");
        if (r->host == NULL) {
            seterr(err, errlen, "frontend[%zu].route[%d]: falta host", fi, i);
            return false;
        }

        bool is_default  = strcmp(r->host, "default") == 0;
        bool is_wildcard = r->host[0] == '*';
        if (!is_default) {
            if (is_wildcard) {
                if (!valid_wildcard(r->host)) {
                    seterr(err, errlen,
                           "frontend[%zu].route[%d]: wildcard \"%s\" mal formado, se espera *.dominio",
                           fi, i, r->host);
                    return false;
                }
            } else if (!valid_hostname(r->host)) {
                seterr(err, errlen, "frontend[%zu].route[%d]: host \"%s\" no es un dominio válido",
                       fi, i, r->host);
                return false;
            }
        }

        for (int j = 0; j < i; j++) {
            if (strcmp(f->routes[j].host, r->host) == 0) {
                seterr(err, errlen,
                       "frontend[%zu].route[%d]: host \"%s\" duplicado (ya en route[%d])",
                       fi, i, r->host, j);
                return false;
            }
        }

        r->backend_name = take_string(rt, "backend");
        if (r->backend_name == NULL) {
            seterr(err, errlen, "frontend[%zu].route[%d]: falta backend", fi, i);
            return false;
        }

        const cfg_backend *target = config_find_backend(cfg, r->backend_name);
        if (target == NULL) {
            seterr(err, errlen, "frontend[%zu].route[%d]: backend \"%s\" no existe",
                   fi, i, r->backend_name);
            return false;
        }
        r->backend_index = (size_t)(target - cfg->backends);
    }
    return true;
}

static bool parse_frontends(toml_table_t *root, config *cfg, char *err, size_t errlen)
{
    toml_array_t *arr = toml_array_in(root, "frontend");
    if (arr == NULL || toml_array_nelem(arr) < 1) {
        seterr(err, errlen, "hace falta al menos un [[frontend]]");
        return false;
    }

    int n          = toml_array_nelem(arr);
    cfg->frontends = calloc((size_t)n, sizeof *cfg->frontends);
    if (cfg->frontends == NULL) {
        seterr(err, errlen, "sin memoria");
        return false;
    }
    cfg->n_frontends = (size_t)n;

    for (int i = 0; i < n; i++) {
        cfg_frontend *f  = &cfg->frontends[i];
        toml_table_t *ft = toml_table_at(arr, i);
        if (ft == NULL) {
            seterr(err, errlen, "frontend[%d]: entrada mal formada", i);
            return false;
        }

        f->name   = take_string(ft, "name");
        f->listen = take_string(ft, "listen");
        if (f->listen == NULL) {
            seterr(err, errlen, "frontend[%d]: falta listen", i);
            return false;
        }
        if (!split_addr(f->listen, &f->listen_host, &f->listen_port)) {
            seterr(err, errlen, "frontend[%d]: listen \"%s\" no tiene forma IP:puerto",
                   i, f->listen);
            return false;
        }
        /* Dos frontends en el mismo puerto compiten por las mismas conexiones:
         * cuál gana depende del orden de bind, así que se rechaza. */
        for (int j = 0; j < i; j++) {
            if (strcmp(cfg->frontends[j].listen, f->listen) == 0) {
                seterr(err, errlen, "frontend[%d]: listen \"%s\" duplicado (ya en frontend[%d])",
                       i, f->listen, j);
                return false;
            }
        }

        if (!parse_routes(ft, f, (size_t)i, cfg, err, errlen)) {
            return false;
        }
    }
    return true;
}

/* --- entrada ------------------------------------------------------------- */

static config *build_from_root(toml_table_t *root, char *err, size_t errlen)
{
    config *cfg = calloc(1, sizeof *cfg);
    if (cfg == NULL) {
        seterr(err, errlen, "sin memoria");
        return NULL;
    }

    /* Los backends van primero: las rutas los referencian por nombre. */
    if (!parse_global(root, cfg, err, errlen) ||
        !parse_backends(root, cfg, err, errlen) ||
        !parse_frontends(root, cfg, err, errlen)) {
        config_free(cfg);
        return NULL;
    }
    return cfg;
}

config *config_parse(const char *toml_text, char *err, size_t errlen)
{
    if (toml_text == NULL) {
        seterr(err, errlen, "config vacía");
        return NULL;
    }

    char *copy = dup_str(toml_text); /* toml_parse escribe sobre el buffer */
    if (copy == NULL) {
        seterr(err, errlen, "sin memoria");
        return NULL;
    }

    char          terr[256] = { 0 };
    toml_table_t *root      = toml_parse(copy, terr, (int)sizeof terr);
    free(copy);

    if (root == NULL) {
        seterr(err, errlen, "TOML inválido: %s", terr);
        return NULL;
    }

    config *cfg = build_from_root(root, err, errlen);
    toml_free(root);
    return cfg;
}

config *config_load(const char *path, char *err, size_t errlen)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        seterr(err, errlen, "no se puede abrir %s", path);
        return NULL;
    }

    char          terr[256] = { 0 };
    toml_table_t *root      = toml_parse_file(fp, terr, (int)sizeof terr);
    fclose(fp);

    if (root == NULL) {
        seterr(err, errlen, "%s: TOML inválido: %s", path, terr);
        return NULL;
    }

    config *cfg = build_from_root(root, err, errlen);
    toml_free(root);
    return cfg;
}
