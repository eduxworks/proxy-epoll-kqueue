/* test_router.c — suite cmocka del enrutado por Host (E8).
 *
 * El orden de resolución es exacto -> wildcard más específico -> default ->
 * NULL (502), y estos casos lo fijan uno a uno.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "router.h"

/* Construye router a partir de TOML; el router adopta la config. */
static router *build(const char *toml)
{
    char    err[256] = { 0 };
    config *cfg      = config_parse(toml, err, sizeof err);
    if (cfg == NULL) {
        fail_msg("config inválida: %s", err);
    }
    router *r = router_build(cfg);
    assert_non_null(r);
    return r;
}

static const char *TOML_BASE =
    "[[frontend]]\n"
    "listen = \"0.0.0.0:8080\"\n"
    "  [[frontend.route]]\n"
    "  host = \"exacto.b.test\"\n"
    "  backend = \"pool_exacto\"\n"
    "  [[frontend.route]]\n"
    "  host = \"*.b.test\"\n"
    "  backend = \"pool_ancho\"\n"
    "  [[frontend.route]]\n"
    "  host = \"*.sub.b.test\"\n"
    "  backend = \"pool_estrecho\"\n"
    "  [[frontend.route]]\n"
    "  host = \"default\"\n"
    "  backend = \"pool_default\"\n"
    "[[backend]]\n"
    "name = \"pool_exacto\"\n"
    "servers = [ { addr = \"127.0.0.1:9001\" } ]\n"
    "[[backend]]\n"
    "name = \"pool_ancho\"\n"
    "servers = [ { addr = \"127.0.0.1:9002\" } ]\n"
    "[[backend]]\n"
    "name = \"pool_estrecho\"\n"
    "servers = [ { addr = \"127.0.0.1:9003\" } ]\n"
    "[[backend]]\n"
    "name = \"pool_default\"\n"
    "servers = [ { addr = \"127.0.0.1:9004\" } ]\n";

static void test_exact_beats_wildcard(void **state)
{
    (void)state;

    router *r = build(TOML_BASE);

    /* "exacto.b.test" encaja también en "*.b.test": debe ganar el exacto. */
    backend_pool *b = router_lookup(r, 0, "exacto.b.test");
    assert_non_null(b);
    assert_string_equal(pool_config(b)->name, "pool_exacto");

    router_unref(r);
}

static void test_most_specific_wildcard_wins(void **state)
{
    (void)state;

    router *r = build(TOML_BASE);

    /* "x.sub.b.test" encaja en "*.b.test" y en "*.sub.b.test". */
    backend_pool *b = router_lookup(r, 0, "x.sub.b.test");
    assert_non_null(b);
    assert_string_equal(pool_config(b)->name, "pool_estrecho");

    /* "x.b.test" solo encaja en el ancho */
    b = router_lookup(r, 0, "x.b.test");
    assert_non_null(b);
    assert_string_equal(pool_config(b)->name, "pool_ancho");

    /* el wildcard exige algo delante: "b.test" pelado cae en default */
    b = router_lookup(r, 0, "b.test");
    assert_non_null(b);
    assert_string_equal(pool_config(b)->name, "pool_default");

    router_unref(r);
}

static void test_default_route(void **state)
{
    (void)state;

    router            *r = build(TOML_BASE);
    backend_pool *b = router_lookup(r, 0, "cualquier.cosa.test");

    assert_non_null(b);
    assert_string_equal(pool_config(b)->name, "pool_default");

    router_unref(r);
}

static void test_no_route_is_null(void **state)
{
    (void)state;

    static const char *toml =
        "[[frontend]]\n"
        "listen = \"0.0.0.0:8080\"\n"
        "  [[frontend.route]]\n"
        "  host = \"solo.test\"\n"
        "  backend = \"pool_a\"\n"
        "[[backend]]\n"
        "name = \"pool_a\"\n"
        "servers = [ { addr = \"127.0.0.1:9001\" } ]\n";

    router *r = build(toml);

    /* sin ruta default, lo desconocido es NULL => 502 */
    assert_null(router_lookup(r, 0, "otro.test"));
    assert_non_null(router_lookup(r, 0, "solo.test"));

    /* un frontend que no existe tampoco resuelve */
    assert_null(router_lookup(r, 1, "solo.test"));

    router_unref(r);
}

static void test_host_normalization(void **state)
{
    (void)state;

    router *r = build(TOML_BASE);

    /* mayúsculas y puerto no cambian el destino */
    backend_pool *b = router_lookup(r, 0, "EXACTO.B.TEST:8080");
    assert_non_null(b);
    assert_string_equal(pool_config(b)->name, "pool_exacto");

    char norm[256];
    assert_true(router_normalize_host("A.Test:8080", norm, sizeof norm));
    assert_string_equal(norm, "a.test");

    /* Host vacío o con bytes fuera de [a-z0-9.-] => 400, no enrutado */
    assert_false(router_normalize_host("", norm, sizeof norm));
    assert_false(router_normalize_host(":8080", norm, sizeof norm));
    assert_false(router_normalize_host("mal_host.test", norm, sizeof norm));
    assert_false(router_normalize_host("a.test", norm, 3)); /* no cabe */

    router_unref(r);
}

/* Con 64 dominios en una tabla de ~128 cubos, el cumpleaños garantiza
 * colisiones de djb2: esto ejercita el encadenado, no solo el hash. */
static void test_hash_collisions_resolve(void **state)
{
    (void)state;

    enum { N = 64 };
    char *toml = malloc(64 * 1024);
    assert_non_null(toml);

    size_t off = (size_t)snprintf(toml, 64 * 1024,
                                  "[[frontend]]\nlisten = \"0.0.0.0:8080\"\n");
    for (int i = 0; i < N; i++) {
        off += (size_t)snprintf(toml + off, 64 * 1024 - off,
                                "  [[frontend.route]]\n"
                                "  host = \"h%d.test\"\n"
                                "  backend = \"pool_%d\"\n",
                                i, i % 2);
    }
    for (int i = 0; i < 2; i++) {
        off += (size_t)snprintf(toml + off, 64 * 1024 - off,
                                "[[backend]]\nname = \"pool_%d\"\n"
                                "servers = [ { addr = \"127.0.0.1:900%d\" } ]\n",
                                i, i + 1);
    }

    router *r = build(toml);

    for (int i = 0; i < N; i++) {
        char host[64];
        char want[64];
        snprintf(host, sizeof host, "h%d.test", i);
        snprintf(want, sizeof want, "pool_%d", i % 2);

        backend_pool *b = router_lookup(r, 0, host);
        assert_non_null(b);
        assert_string_equal(pool_config(b)->name, want);
    }

    /* uno que no está sigue sin resolver */
    assert_null(router_lookup(r, 0, "h999.test"));

    router_unref(r);
    free(toml);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_exact_beats_wildcard),
        cmocka_unit_test(test_most_specific_wildcard_wins),
        cmocka_unit_test(test_default_route),
        cmocka_unit_test(test_no_route_is_null),
        cmocka_unit_test(test_host_normalization),
        cmocka_unit_test(test_hash_collisions_resolve),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
