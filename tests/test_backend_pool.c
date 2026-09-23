/* test_backend_pool.c — suite cmocka del balanceo (E9, E10). */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "backend_pool.h"

/* cmocka 2.x marca assert_in_range como obsoleta y su sustituta
 * (assert_int_in_range) no existe en la 1.1.7 que trae Ubuntu. Con una
 * comprobación propia el test vale en las dos versiones y el mensaje de fallo
 * dice el valor y el rango, que es lo que uno quiere leer. */
static void assert_between(int value, int lo, int hi)
{
    if (value < lo || value > hi) {
        fail_msg("valor %d fuera del rango [%d, %d]", value, lo, hi);
    }
}

/* La config se construye con el parser real en vez de a mano: así el test
 * también protege el camino TOML -> pool. */
static config *parse(const char *toml)
{
    char    err[256] = { 0 };
    config *cfg      = config_parse(toml, err, sizeof err);
    if (cfg == NULL) {
        fail_msg("config inválida: %s", err);
    }
    return cfg;
}

static const char *TOML_RR3 =
    "[[frontend]]\n"
    "listen = \"0.0.0.0:8080\"\n"
    "  [[frontend.route]]\n"
    "  host = \"a.test\"\n"
    "  backend = \"pool\"\n"
    "[[backend]]\n"
    "name = \"pool\"\n"
    "balance = \"round_robin\"\n"
    "servers = [\n"
    "  { addr = \"127.0.0.1:9001\" },\n"
    "  { addr = \"127.0.0.1:9002\" },\n"
    "  { addr = \"127.0.0.1:9003\" },\n"
    "]\n";

/* Cuenta cuántas veces sale cada backend en n elecciones. */
static void tally(backend_pool *p, int n, int *counts, size_t ncounts)
{
    memset(counts, 0, ncounts * sizeof *counts);
    for (int i = 0; i < n; i++) {
        backend *b = pool_pick(p);
        assert_non_null(b);
        for (size_t j = 0; j < pool_size(p); j++) {
            if (pool_backend_at(p, j) == b) {
                counts[j]++;
                break;
            }
        }
    }
}

static void test_round_robin_is_even(void **state)
{
    (void)state;

    config       *cfg = parse(TOML_RR3);
    backend_pool *p   = pool_create(&cfg->backends[0]);
    assert_non_null(p);
    assert_int_equal(pool_size(p), 3);

    int counts[3];
    tally(p, 300, counts, 3);

    for (int i = 0; i < 3; i++) {
        assert_between(counts[i], 99, 101); /* 100 ±1 */
    }

    pool_destroy(p);
    config_free(cfg);
}

/* E10: un backend caído deja de recibir tráfico, y el resto se reparte lo suyo. */
static void test_down_backend_is_excluded(void **state)
{
    (void)state;

    config       *cfg = parse(TOML_RR3);
    backend_pool *p   = pool_create(&cfg->backends[0]);
    assert_non_null(p);

    backend *victim = pool_backend_at(p, 1);
    assert_true(backend_is_up(victim));

    /* Salud pasiva: `fall` fallos seguidos (3 por defecto) lo sacan. */
    pool_report(p, victim, false);
    assert_true(backend_is_up(victim)); /* aún no */
    pool_report(p, victim, false);
    pool_report(p, victim, false);
    assert_false(backend_is_up(victim));
    assert_int_equal(pool_up_count(p), 2);

    int counts[3];
    tally(p, 300, counts, 3);

    assert_int_equal(counts[1], 0);
    assert_between(counts[0], 149, 151);
    assert_between(counts[2], 149, 151);

    /* Y vuelve tras `rise` aciertos (2 por defecto). */
    pool_report(p, victim, true);
    assert_false(backend_is_up(victim));
    pool_report(p, victim, true);
    assert_true(backend_is_up(victim));

    pool_destroy(p);
    config_free(cfg);
}

static void test_all_down_returns_null(void **state)
{
    (void)state;

    config       *cfg = parse(TOML_RR3);
    backend_pool *p   = pool_create(&cfg->backends[0]);
    assert_non_null(p);

    for (size_t i = 0; i < pool_size(p); i++) {
        backend *b = pool_backend_at(p, i);
        for (int k = 0; k < 3; k++) {
            pool_report(p, b, false);
        }
    }

    assert_int_equal(pool_up_count(p), 0);
    assert_null(pool_pick(p)); /* sin nadie en pie: esto es el 502 */

    pool_destroy(p);
    config_free(cfg);
}

static void test_weighted_respects_weights(void **state)
{
    (void)state;

    static const char *toml =
        "[[frontend]]\n"
        "listen = \"0.0.0.0:8080\"\n"
        "  [[frontend.route]]\n"
        "  host = \"a.test\"\n"
        "  backend = \"pool\"\n"
        "[[backend]]\n"
        "name = \"pool\"\n"
        "balance = \"weighted\"\n"
        "servers = [\n"
        "  { addr = \"127.0.0.1:9001\", weight = 3 },\n"
        "  { addr = \"127.0.0.1:9002\", weight = 1 },\n"
        "]\n";

    config       *cfg = parse(toml);
    backend_pool *p   = pool_create(&cfg->backends[0]);
    assert_non_null(p);

    int counts[2];
    tally(p, 400, counts, 2);

    assert_int_equal(counts[0], 300);
    assert_int_equal(counts[1], 100);

    pool_destroy(p);
    config_free(cfg);
}

static void test_least_conn_prefers_idle(void **state)
{
    (void)state;

    static const char *toml =
        "[[frontend]]\n"
        "listen = \"0.0.0.0:8080\"\n"
        "  [[frontend.route]]\n"
        "  host = \"a.test\"\n"
        "  backend = \"pool\"\n"
        "[[backend]]\n"
        "name = \"pool\"\n"
        "balance = \"least_conn\"\n"
        "servers = [\n"
        "  { addr = \"127.0.0.1:9001\" },\n"
        "  { addr = \"127.0.0.1:9002\" },\n"
        "]\n";

    config       *cfg = parse(toml);
    backend_pool *p   = pool_create(&cfg->backends[0]);
    assert_non_null(p);

    backend *busy = pool_backend_at(p, 0);
    backend *idle = pool_backend_at(p, 1);

    for (int i = 0; i < 5; i++) {
        backend_conn_opened(busy);
    }
    assert_int_equal(backend_active_conns(busy), 5);
    assert_int_equal(backend_active_conns(idle), 0);

    /* Mientras uno siga teniendo menos conexiones, se lleva todas. */
    for (int i = 0; i < 5; i++) {
        assert_ptr_equal(pool_pick(p), idle);
    }

    /* Al igualarse, el desempate no puede dar siempre al mismo. */
    for (int i = 0; i < 5; i++) {
        backend_conn_opened(idle);
    }
    assert_int_equal(backend_active_conns(idle), 5);

    int counts[2];
    tally(p, 100, counts, 2);
    assert_true(counts[0] > 0);
    assert_true(counts[1] > 0);

    backend_conn_closed(busy);
    assert_int_equal(backend_active_conns(busy), 4);

    pool_destroy(p);
    config_free(cfg);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_round_robin_is_even),
        cmocka_unit_test(test_down_backend_is_excluded),
        cmocka_unit_test(test_all_down_returns_null),
        cmocka_unit_test(test_weighted_respects_weights),
        cmocka_unit_test(test_least_conn_prefers_idle),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
