/* test_config.c — suite cmocka del parser y validador TOML (E12).
 *
 * Cada caso de error comprueba además que el mensaje cita la ruta TOML: un
 * "configuración inválida" a secas obliga a buscar a ojo en el fichero.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "config.h"

static const char *VALID_TOML =
    "[global]\n"
    "workers = 0\n"
    "max_conns = 4096\n"
    "log_level = \"info\"\n"
    "stats_socket = \"/tmp/proxy-stats.sock\"\n"
    "\n"
    "[[frontend]]\n"
    "name = \"public\"\n"
    "listen = \"0.0.0.0:8080\"\n"
    "  [[frontend.route]]\n"
    "  host = \"a.test\"\n"
    "  backend = \"pool_a\"\n"
    "  [[frontend.route]]\n"
    "  host = \"*.b.test\"\n"
    "  backend = \"pool_b\"\n"
    "  [[frontend.route]]\n"
    "  host = \"default\"\n"
    "  backend = \"pool_a\"\n"
    "\n"
    "[[frontend]]\n"
    "name = \"internal\"\n"
    "listen = \"127.0.0.1:8081\"\n"
    "  [[frontend.route]]\n"
    "  host = \"admin.test\"\n"
    "  backend = \"pool_b\"\n"
    "\n"
    "[[backend]]\n"
    "name = \"pool_a\"\n"
    "balance = \"round_robin\"\n"
    "servers = [\n"
    "  { addr = \"127.0.0.1:9001\", weight = 1 },\n"
    "  { addr = \"127.0.0.1:9002\", weight = 3 },\n"
    "]\n"
    "\n"
    "[[backend]]\n"
    "name = \"pool_b\"\n"
    "balance = \"least_conn\"\n"
    "servers = [ { addr = \"127.0.0.1:9003\" } ]\n"
    "  [backend.health]\n"
    "  type = \"http\"\n"
    "  interval = 2000\n"
    "  timeout = 500\n"
    "  rise = 2\n"
    "  fall = 3\n"
    "  path = \"/healthz\"\n";

static void test_valid_config(void **state)
{
    (void)state;

    char    err[256] = { 0 };
    config *cfg      = config_parse(VALID_TOML, err, sizeof err);

    assert_non_null(cfg);
    assert_string_equal(err, "");

    assert_int_equal(cfg->global.workers, 0);
    assert_int_equal(cfg->global.max_conns, 4096);
    assert_string_equal(cfg->global.log_level, "info");

    assert_int_equal(cfg->n_frontends, 2);
    assert_int_equal(cfg->n_backends, 2);

    /* frontend 0: tres rutas, la wildcard y la default incluidas */
    const cfg_frontend *pub = &cfg->frontends[0];
    assert_string_equal(pub->listen_host, "0.0.0.0");
    assert_int_equal(pub->listen_port, 8080);
    assert_int_equal(pub->n_routes, 3);
    assert_string_equal(pub->routes[1].host, "*.b.test");

    /* las rutas quedan resueltas a índice de backend durante la validación */
    assert_int_equal(pub->routes[0].backend_index, 0);
    assert_int_equal(pub->routes[1].backend_index, 1);
    assert_int_equal(pub->routes[2].backend_index, 0);

    const cfg_backend *a = &cfg->backends[0];
    assert_string_equal(a->name, "pool_a");
    assert_int_equal(a->balance, BALANCE_ROUND_ROBIN);
    assert_int_equal(a->n_servers, 2);
    assert_string_equal(a->servers[0].host, "127.0.0.1");
    assert_int_equal(a->servers[0].port, 9001);
    assert_int_equal(a->servers[0].weight, 1); /* weight omitido => 1 */
    assert_int_equal(a->servers[1].weight, 3);

    const cfg_backend *b = &cfg->backends[1];
    assert_int_equal(b->balance, BALANCE_LEAST_CONN);
    assert_int_equal(b->health.type, HEALTH_HTTP);
    assert_string_equal(b->health.path, "/healthz");
    assert_int_equal(b->health.rise, 2);
    assert_int_equal(b->health.fall, 3);

    assert_ptr_equal(config_find_backend(cfg, "pool_b"), b);
    assert_null(config_find_backend(cfg, "no_existe"));

    config_free(cfg);
}

static void test_unknown_backend(void **state)
{
    (void)state;

    static const char *toml =
        "[[frontend]]\n"
        "listen = \"0.0.0.0:8080\"\n"
        "  [[frontend.route]]\n"
        "  host = \"a.test\"\n"
        "  backend = \"pool_a\"\n"
        "  [[frontend.route]]\n"
        "  host = \"b.test\"\n"
        "  backend = \"pool_fantasma\"\n"
        "[[backend]]\n"
        "name = \"pool_a\"\n"
        "servers = [ { addr = \"127.0.0.1:9001\" } ]\n";

    char    err[256] = { 0 };
    config *cfg      = config_parse(toml, err, sizeof err);

    assert_null(cfg);
    assert_non_null(strstr(err, "frontend[0].route[1]"));
    assert_non_null(strstr(err, "pool_fantasma"));
    assert_non_null(strstr(err, "no existe"));
}

static void test_duplicate_listen(void **state)
{
    (void)state;

    static const char *toml =
        "[[frontend]]\n"
        "listen = \"0.0.0.0:8080\"\n"
        "  [[frontend.route]]\n"
        "  host = \"a.test\"\n"
        "  backend = \"pool_a\"\n"
        "[[frontend]]\n"
        "listen = \"0.0.0.0:8080\"\n"
        "  [[frontend.route]]\n"
        "  host = \"b.test\"\n"
        "  backend = \"pool_a\"\n"
        "[[backend]]\n"
        "name = \"pool_a\"\n"
        "servers = [ { addr = \"127.0.0.1:9001\" } ]\n";

    char    err[256] = { 0 };
    config *cfg      = config_parse(toml, err, sizeof err);

    assert_null(cfg);
    assert_non_null(strstr(err, "frontend[1]"));
    assert_non_null(strstr(err, "duplicado"));
}

static void test_malformed_wildcard(void **state)
{
    (void)state;

    static const char *toml =
        "[[frontend]]\n"
        "listen = \"0.0.0.0:8080\"\n"
        "  [[frontend.route]]\n"
        "  host = \"*b.test\"\n" /* falta el punto tras el asterisco */
        "  backend = \"pool_a\"\n"
        "[[backend]]\n"
        "name = \"pool_a\"\n"
        "servers = [ { addr = \"127.0.0.1:9001\" } ]\n";

    char    err[256] = { 0 };
    config *cfg      = config_parse(toml, err, sizeof err);

    assert_null(cfg);
    assert_non_null(strstr(err, "frontend[0].route[0]"));
    assert_non_null(strstr(err, "wildcard"));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_valid_config),
        cmocka_unit_test(test_unknown_backend),
        cmocka_unit_test(test_duplicate_listen),
        cmocka_unit_test(test_malformed_wildcard),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
