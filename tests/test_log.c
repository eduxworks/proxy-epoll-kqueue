/* test_log.c — anillo de registro asíncrono (E15).
 *
 * Fuera de los 22 casos del README, que no lista una suite para este módulo.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "log.h"

static char tmp_path[256];

static void make_tmp(void)
{
    snprintf(tmp_path, sizeof tmp_path, "/tmp/proxy-log-test-%d-%ld.log",
             (int)getpid(), (long)rand());
    unlink(tmp_path);
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (len != NULL) {
        *len = got;
    }
    return buf;
}

/* Cuenta por longitud, no hasta el primer NUL: si el registro colara un byte
 * nulo dentro de una línea, recorrer con strlen lo ocultaría. */
static int count_lines(const char *s, size_t len)
{
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            n++;
        }
    }
    return n;
}

static bool has_embedded_nul(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\0') {
            return true;
        }
    }
    return false;
}

/* El hilo consumidor es quien escribe: si log_shutdown no lo esperase, el
 * fichero saldría a medias y el fallo sería intermitente. */
static void test_roundtrip_and_flush_on_shutdown(void **state)
{
    (void)state;
    make_tmp();

    assert_int_equal(log_init(tmp_path, LOG_INFO), 0);
    log_write(LOG_INFO, "hola %s numero %d", "mundo", 42);
    log_write(LOG_ERROR, "algo fue mal: %s", "detalle");
    log_shutdown();

    size_t len = 0;
    char *body = slurp(tmp_path, &len);
    assert_non_null(body);

    assert_int_equal(count_lines(body, len), 2);
    assert_false(has_embedded_nul(body, len));
    assert_non_null(strstr(body, "hola mundo numero 42"));
    assert_non_null(strstr(body, "algo fue mal: detalle"));
    assert_non_null(strstr(body, "INFO"));
    assert_non_null(strstr(body, "ERROR"));
    assert_non_null(strstr(body, "Z ")); /* marca de tiempo ISO en UTC */

    free(body);
    unlink(tmp_path);
}

static void test_level_filters(void **state)
{
    (void)state;
    make_tmp();

    assert_int_equal(log_init(tmp_path, LOG_WARN), 0);
    log_write(LOG_DEBUG, "no deberia salir");
    log_write(LOG_INFO, "tampoco esta");
    log_write(LOG_WARN, "esta si");
    log_write(LOG_ERROR, "y esta tambien");
    log_shutdown();

    size_t len = 0;
    char *body = slurp(tmp_path, &len);
    assert_non_null(body);

    assert_int_equal(count_lines(body, len), 2);
    assert_false(has_embedded_nul(body, len));
    assert_null(strstr(body, "no deberia salir"));
    assert_null(strstr(body, "tampoco esta"));
    assert_non_null(strstr(body, "esta si"));

    free(body);
    unlink(tmp_path);
}

/* La ranura es de tamaño fijo: un mensaje enorme se recorta en vez de reservar
 * memoria en el camino caliente. */
static void test_long_message_is_truncated(void **state)
{
    (void)state;
    make_tmp();

    char big[LOG_SLOT_BYTES * 3];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';

    assert_int_equal(log_init(tmp_path, LOG_INFO), 0);
    log_write(LOG_INFO, "%s", big);
    log_shutdown();

    /* longitud real: la linea puede acabar justo en el limite de la ranura */
    size_t len = 0;
    char *body = slurp(tmp_path, &len);
    assert_non_null(body);

    assert_true(len <= LOG_SLOT_BYTES);
    assert_int_equal(count_lines(body, len), 1);
    assert_false(has_embedded_nul(body, len));
    assert_int_equal(body[len - 1], '\n'); /* sigue terminando en linea */

    free(body);
    unlink(tmp_path);
}

static void test_levels_parse_and_name(void **state)
{
    (void)state;

    log_level lvl;
    assert_true(log_level_parse("debug", &lvl));
    assert_int_equal(lvl, LOG_DEBUG);
    assert_true(log_level_parse("error", &lvl));
    assert_int_equal(lvl, LOG_ERROR);
    assert_false(log_level_parse("chatty", &lvl));
    assert_false(log_level_parse(NULL, &lvl));

    assert_string_equal(log_level_name(LOG_WARN), "WARN");
}

/* Escribir sin inicializar no puede reventar: durante el arranque y el apagado
 * hay ventanas en las que el registro todavía no existe. */
static void test_write_without_init_is_safe(void **state)
{
    (void)state;

    log_shutdown(); /* idempotente aunque no se haya iniciado */
    log_write(LOG_ERROR, "al vacio");
    log_shutdown();
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_roundtrip_and_flush_on_shutdown),
        cmocka_unit_test(test_level_filters),
        cmocka_unit_test(test_long_message_is_truncated),
        cmocka_unit_test(test_levels_parse_and_name),
        cmocka_unit_test(test_write_without_init_is_safe),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
