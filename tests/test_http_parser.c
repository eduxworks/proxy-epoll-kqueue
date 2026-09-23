/* test_http_parser.c — suite cmocka del parser de cabeceras (E7, E14). */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "http_parser.h"

static int count_occurrences(const char *haystack, const char *needle)
{
    int    n   = 0;
    size_t len = strlen(needle);
    for (const char *p = strstr(haystack, needle); p != NULL;
         p = strstr(p + len, needle)) {
        n++;
    }
    return n;
}

static const char *REQ =
    "GET /index.html HTTP/1.1\r\n"
    "Host: a.test\r\n"
    "User-Agent: wrk/4.2\r\n"
    "Accept: */*\r\n"
    "\r\n";

static void test_complete_request(void **state)
{
    (void)state;

    http_request req;
    hp_init(&req);

    assert_int_equal(hp_execute(&req, REQ, strlen(REQ)), HP_DONE);
    assert_true(req.has_host);
    assert_string_equal(req.host, "a.test");
    assert_int_equal(req.minor_version, 1);
    assert_true(req.keep_alive);
    assert_false(req.has_xff);
    assert_int_equal(req.header_len, strlen(REQ));
}

/* Con datos a trozos el parser no puede decidir hasta ver el CRLF CRLF. */
static void test_split_in_two(void **state)
{
    (void)state;

    size_t total = strlen(REQ);
    size_t cut   = 20; /* en mitad de la línea de petición */

    http_request req;
    hp_init(&req);

    assert_int_equal(hp_execute(&req, REQ, cut), HP_NEED_MORE);
    assert_int_equal(hp_execute(&req, REQ, total), HP_DONE);
    assert_string_equal(req.host, "a.test");
}

/* El caso duro: un byte por llamada. Si el parser guardase estado a medias
 * entre llamadas, aquí se rompería. */
static void test_split_byte_by_byte(void **state)
{
    (void)state;

    size_t       total = strlen(REQ);
    http_request req;
    hp_init(&req);

    hp_status st = HP_NEED_MORE;
    for (size_t i = 1; i <= total; i++) {
        st = hp_execute(&req, REQ, i);
        if (i < total) {
            assert_int_equal(st, HP_NEED_MORE);
        }
    }

    assert_int_equal(st, HP_DONE);
    assert_string_equal(req.host, "a.test");
}

static void test_missing_host_is_400(void **state)
{
    (void)state;

    static const char *no_host = "GET / HTTP/1.1\r\n"
                                 "User-Agent: curl\r\n"
                                 "\r\n";

    http_request req;
    hp_init(&req);

    assert_int_equal(hp_execute(&req, no_host, strlen(no_host)), HP_ERROR);
    assert_int_equal(req.error_code, 400);

    /* Dos Host distintos también son 400: elegir uno es contrabando. */
    static const char *dup = "GET / HTTP/1.1\r\n"
                             "Host: a.test\r\n"
                             "Host: b.test\r\n"
                             "\r\n";
    hp_init(&req);
    assert_int_equal(hp_execute(&req, dup, strlen(dup)), HP_ERROR);
    assert_int_equal(req.error_code, 400);
}

static void test_host_with_port(void **state)
{
    (void)state;

    static const char *with_port = "GET / HTTP/1.1\r\n"
                                   "Host: A.Test:8080\r\n"
                                   "\r\n";

    http_request req;
    hp_init(&req);

    assert_int_equal(hp_execute(&req, with_port, strlen(with_port)), HP_DONE);
    /* El parser no normaliza: entrega el Host tal cual y de eso se encarga
     * el router, que es quien conoce las reglas de enrutado. */
    assert_string_equal(req.host, "A.Test:8080");
}

static void test_oversized_headers_are_431(void **state)
{
    (void)state;

    size_t cap = HP_MAX_HEADER_BYTES + 1024;
    char  *big = malloc(cap);
    assert_non_null(big);

    size_t off = (size_t)snprintf(big, cap, "GET / HTTP/1.1\r\nHost: a.test\r\n");
    while (off < HP_MAX_HEADER_BYTES + 512) {
        off += (size_t)snprintf(big + off, cap - off, "X-Relleno-%zu: %s\r\n", off,
                                "0123456789012345678901234567890123456789");
    }

    http_request req;
    hp_init(&req);

    /* Sin CRLF CRLF todavía, pero ya pasado el límite: se corta sin esperar. */
    assert_int_equal(hp_execute(&req, big, off), HP_ERROR);
    assert_int_equal(req.error_code, 431);

    free(big);
}

/* E14: la cadena de proxies se mantiene. Sustituir el valor previo borraría
 * la IP real del cliente, que es justo lo que la cabecera sirve para saber. */
static void test_xff_is_appended_not_replaced(void **state)
{
    (void)state;

    static const char *with_xff = "GET /a HTTP/1.1\r\n"
                                  "Host: a.test\r\n"
                                  "X-Forwarded-For: 10.0.0.1, 10.0.0.2\r\n"
                                  "X-Real-IP: 10.0.0.1\r\n"
                                  "Accept: */*\r\n"
                                  "\r\n";

    http_request req;
    hp_init(&req);
    assert_int_equal(hp_execute(&req, with_xff, strlen(with_xff)), HP_DONE);
    assert_true(req.has_xff);

    char    out[2048];
    ssize_t n = hp_rewrite(&req, with_xff, strlen(with_xff), "203.0.113.7", "http",
                           out, sizeof out);
    assert_true(n > 0);
    out[n] = '\0';

    assert_non_null(strstr(out, "X-Forwarded-For: 10.0.0.1, 10.0.0.2, 203.0.113.7\r\n"));
    assert_non_null(strstr(out, "X-Forwarded-Proto: http\r\n"));
    assert_non_null(strstr(out, "X-Real-IP: 203.0.113.7\r\n"));

    /* el X-Real-IP que traía el cliente no sobrevive: lo pone el proxy */
    assert_null(strstr(out, "X-Real-IP: 10.0.0.1\r\n"));

    /* y solo sale una X-Forwarded-For: la original se consume al encadenar */
    assert_int_equal(count_occurrences(out, "X-Forwarded-For:"), 1);
    assert_int_equal(count_occurrences(out, "X-Real-IP:"), 1);

    assert_non_null(strstr(out, "Accept: */*\r\n"));
    assert_int_equal(strncmp(out, "GET /a HTTP/1.1\r\n", 17), 0);

    /* sin X-Forwarded-For previa se emite solo la IP del cliente */
    hp_init(&req);
    assert_int_equal(hp_execute(&req, REQ, strlen(REQ)), HP_DONE);
    n = hp_rewrite(&req, REQ, strlen(REQ), "198.51.100.9", "http", out, sizeof out);
    assert_true(n > 0);
    out[n] = '\0';
    assert_non_null(strstr(out, "X-Forwarded-For: 198.51.100.9\r\n"));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_complete_request),
        cmocka_unit_test(test_split_in_two),
        cmocka_unit_test(test_split_byte_by_byte),
        cmocka_unit_test(test_missing_host_is_400),
        cmocka_unit_test(test_host_with_port),
        cmocka_unit_test(test_oversized_headers_are_431),
        cmocka_unit_test(test_xff_is_appended_not_replaced),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
