/* test_stats.c — snapshot JSON (E17).
 *
 * Fuera de los 22 casos del README. Comprueba el render, que es donde puede
 * salir JSON roto; el transporte por socket lo cubre el e2e.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cmocka.h>

#include "stats.h"

static router *build_router(void)
{
    static const char *toml =
        "[[frontend]]\n"
        "listen = \"127.0.0.1:18082\"\n"
        "  [[frontend.route]]\n"
        "  host    = \"a.test\"\n"
        "  backend = \"pool_a\"\n"
        "[[backend]]\n"
        "name    = \"pool_a\"\n"
        "servers = [\n"
        "  { addr = \"127.0.0.1:9001\", weight = 2 },\n"
        "  { addr = \"127.0.0.1:9002\" },\n"
        "]\n";

    char    err[256] = { 0 };
    config *cfg      = config_parse(toml, err, sizeof err);
    if (cfg == NULL) {
        fail_msg("config inválida: %s", err);
    }
    router *r = router_build(cfg);
    assert_non_null(r);
    return r;
}

/* Comprobación de forma, no de librería JSON: llaves y corchetes equilibrados
 * y sin comas colgando antes de un cierre, que es como se rompe un JSON
 * generado a mano. */
static void assert_wellformed_json(const char *s)
{
    int braces = 0, brackets = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '{') braces++;
        if (*p == '}') braces--;
        if (*p == '[') brackets++;
        if (*p == ']') brackets--;
        assert_true(braces >= 0);
        assert_true(brackets >= 0);

        if (*p == ',') {
            const char *q = p + 1;
            while (*q == ' ' || *q == '\n' || *q == '\t') {
                q++;
            }
            assert_true(*q != '}' && *q != ']');
        }
    }
    assert_int_equal(braces, 0);
    assert_int_equal(brackets, 0);
}

static void test_render_has_expected_shape(void **state)
{
    (void)state;

    router *r = build_router();
    char    out[8192];

    size_t n = stats_render(out, sizeof out, r, NULL, 3, time(NULL) - 42);
    assert_true(n > 0);
    assert_true(n < sizeof out);
    assert_int_equal(strlen(out), n);

    assert_wellformed_json(out);

    assert_non_null(strstr(out, "\"worker\": 3"));
    assert_non_null(strstr(out, "\"uptime_s\": 4")); /* 42 segundos, con margen */
    assert_non_null(strstr(out, "\"pool\": \"pool_a\""));
    assert_non_null(strstr(out, "\"addr\": \"127.0.0.1:9001\""));
    assert_non_null(strstr(out, "\"addr\": \"127.0.0.1:9002\""));
    assert_non_null(strstr(out, "\"up\": true"));
    assert_non_null(strstr(out, "\"weight\": 2"));
    assert_non_null(strstr(out, "\"dropped\":"));

    router_unref(r);
}

/* Sin backends el array debe salir vacío y válido: "[]", no "[,]" ni "[\n]". */
static void test_render_without_router(void **state)
{
    (void)state;

    char   out[4096];
    size_t n = stats_render(out, sizeof out, NULL, NULL, 0, time(NULL));

    assert_true(n > 0);
    assert_wellformed_json(out);
    assert_non_null(strstr(out, "\"backends\": []"));
}

/* Un buffer corto debe devolver 0, no un JSON cortado a la mitad: servir
 * medio snapshot es peor que no servir ninguno. */
static void test_render_refuses_to_truncate(void **state)
{
    (void)state;

    router *r = build_router();

    char out[64];
    assert_int_equal(stats_render(out, sizeof out, r, NULL, 0, time(NULL)), 0);

    router_unref(r);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_render_has_expected_shape),
        cmocka_unit_test(test_render_without_router),
        cmocka_unit_test(test_render_refuses_to_truncate),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
