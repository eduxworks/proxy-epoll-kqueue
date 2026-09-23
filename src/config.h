/* config.h — configuración TOML del proxy (E12).
 *
 * Una config es inmutable en cuanto config_load() la devuelve: recargar no
 * consiste en modificarla sino en construir otra y publicarla (E13, router.h).
 */
#ifndef PROXY_CONFIG_H
#define PROXY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BALANCE_ROUND_ROBIN = 0,
    BALANCE_WEIGHTED,
    BALANCE_LEAST_CONN
} cfg_balance;

typedef enum {
    HEALTH_TCP = 0,
    HEALTH_HTTP
} cfg_health_type;

typedef struct {
    char    *addr; /* tal cual aparece en el TOML: "127.0.0.1:9001" */
    char    *host;
    uint16_t port;
    int      weight;
} cfg_server;

typedef struct {
    cfg_health_type type;
    int             interval_ms;
    int             timeout_ms;
    int             rise;
    int             fall;
    char           *path; /* solo si type == HEALTH_HTTP */
} cfg_health;

typedef struct {
    char       *name;
    cfg_balance balance;
    cfg_server *servers;
    size_t      n_servers;
    cfg_health  health;
} cfg_backend;

typedef struct {
    char  *host;         /* "a.test", "*.b.test" o "default" */
    char  *backend_name;
    size_t backend_index; /* resuelto durante la validación */
} cfg_route;

typedef struct {
    char      *name;
    char      *listen; /* "0.0.0.0:80" */
    char      *listen_host;
    uint16_t   listen_port;
    cfg_route *routes;
    size_t     n_routes;
} cfg_frontend;

typedef struct {
    int   workers;   /* 0 = una por CPU */
    int   max_conns;
    char *stats_socket;
    char *log_file;
    char *log_level;
} cfg_global;

typedef struct {
    cfg_global    global;
    cfg_frontend *frontends;
    size_t        n_frontends;
    cfg_backend  *backends;
    size_t        n_backends;
} config;

/* Devuelven NULL y escriben en err (con la ruta TOML del fallo) si la
 * configuración no es válida. La config parcial se libera sola. */
config *config_load(const char *path, char *err, size_t errlen);
config *config_parse(const char *toml_text, char *err, size_t errlen);

void config_free(config *cfg);

const cfg_backend *config_find_backend(const config *cfg, const char *name);

#endif /* PROXY_CONFIG_H */
