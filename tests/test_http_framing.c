/* test_http_framing.c — delimitación de respuestas y cuerpos chunked.
 *
 * Fuera de los 22 casos del README, que no lista una suite para esto. Es lo
 * que sostiene el keep-alive: si el proxy no sabe dónde acaba una respuesta,
 * la siguiente empieza donde no debe y el cliente recibe basura.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "http_parser.h"

static void parse_resp(http_response *res, const char *text, bool head)
{
    hp_response_init(res);
    assert_int_equal(hp_response_execute(res, text, strlen(text), head), HP_DONE);
}

static void test_content_length(void **state)
{
    (void)state;

    http_response res;
    parse_resp(&res,
               "HTTP/1.1 200 OK\r\n"
               "Content-Type: text/plain\r\n"
               "Content-Length: 1234\r\n"
               "\r\n",
               false);

    assert_int_equal(res.status_code, 200);
    assert_int_equal(res.body_mode, HP_BODY_LENGTH);
    assert_int_equal((int)res.content_length, 1234);
    assert_true(res.keep_alive);
}

static void test_chunked_beats_length(void **state)
{
    (void)state;

    /* Si vienen los dos, quien los interprete distinto ve mensajes distintos:
     * chunked manda, que es lo que dice la norma. */
    http_response res;
    parse_resp(&res,
               "HTTP/1.1 200 OK\r\n"
               "Content-Length: 10\r\n"
               "Transfer-Encoding: chunked\r\n"
               "\r\n",
               false);

    assert_int_equal(res.body_mode, HP_BODY_CHUNKED);
}

/* Respuestas que no llevan cuerpo aunque declaren longitud: creerse el
 * Content-Length aquí desincronizaría la conexión entera. */
static void test_bodyless_responses(void **state)
{
    (void)state;

    http_response res;

    parse_resp(&res, "HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\n", false);
    assert_int_equal(res.body_mode, HP_BODY_NONE);

    parse_resp(&res, "HTTP/1.1 304 Not Modified\r\nContent-Length: 99\r\n\r\n", false);
    assert_int_equal(res.body_mode, HP_BODY_NONE);

    /* La respuesta a HEAD trae Content-Length pero no cuerpo. */
    parse_resp(&res, "HTTP/1.1 200 OK\r\nContent-Length: 4096\r\n\r\n", true);
    assert_int_equal(res.body_mode, HP_BODY_NONE);
}

static void test_no_delimiter_forces_close(void **state)
{
    (void)state;

    /* Sin Content-Length ni chunked, el final es el cierre: no hay manera de
     * reutilizar la conexión por mucho que diga Connection. */
    http_response res;
    parse_resp(&res, "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\n\r\n", false);

    assert_int_equal(res.body_mode, HP_BODY_UNTIL_CLOSE);
    assert_false(res.keep_alive);

    parse_resp(&res, "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\n",
               false);
    assert_int_equal(res.body_mode, HP_BODY_LENGTH);
    assert_false(res.keep_alive);
}

static void test_response_needs_full_header(void **state)
{
    (void)state;

    static const char *text = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";

    http_response res;
    hp_response_init(&res);

    /* A trozos: hasta el CRLF CRLF no se puede decidir nada. */
    for (size_t i = 1; i < strlen(text) - 2; i++) {
        assert_int_equal(hp_response_execute(&res, text, i, false), HP_NEED_MORE);
    }
    assert_int_equal(hp_response_execute(&res, text, strlen(text), false), HP_DONE);
    assert_int_equal((int)res.header_len, (int)(strlen(text) - 2));
}

static void test_chunked_scanner(void **state)
{
    (void)state;

    static const char *body = "4\r\nWiki\r\n"
                              "5\r\npedia\r\n"
                              "0\r\n"
                              "\r\n";

    hp_chunked ch;
    hp_chunked_init(&ch);

    size_t used = hp_chunked_feed(&ch, body, strlen(body));
    assert_true(ch.done);
    assert_false(ch.error);
    assert_int_equal((int)used, (int)strlen(body));

    /* Byte a byte debe dar el mismo resultado: el escáner no puede depender
     * de cómo se parta el flujo, que es justo lo que no controlamos. */
    hp_chunked_init(&ch);
    for (size_t i = 0; i < strlen(body) && !ch.done; i++) {
        hp_chunked_feed(&ch, body + i, 1);
    }
    assert_true(ch.done);
    assert_false(ch.error);
}

static void test_chunked_extensions_and_garbage(void **state)
{
    (void)state;

    /* Los dígitos hexadecimales de una extensión no son parte del tamaño. */
    static const char *with_ext = "4;nombre=ff\r\nWiki\r\n0\r\n\r\n";

    hp_chunked ch;
    hp_chunked_init(&ch);
    hp_chunked_feed(&ch, with_ext, strlen(with_ext));
    assert_true(ch.done);
    assert_false(ch.error);

    /* Basura donde debería ir el tamaño: se rechaza en vez de inventar. */
    static const char *garbage = "zz\r\nhola\r\n";
    hp_chunked_init(&ch);
    hp_chunked_feed(&ch, garbage, strlen(garbage));
    assert_true(ch.error);
}

/* La cabecera Connection es de salto a salto: describe el enlace con quien la
 * manda, no el siguiente tramo, así que el proxy la descarta y pone la suya. */
static void test_rewrite_replaces_hop_headers(void **state)
{
    (void)state;

    static const char *text = "GET / HTTP/1.1\r\n"
                              "Host: a.test\r\n"
                              "Connection: close\r\n"
                              "Proxy-Connection: keep-alive\r\n"
                              "Accept: */*\r\n"
                              "\r\n";

    http_request req;
    hp_init(&req);
    assert_int_equal(hp_execute(&req, text, strlen(text)), HP_DONE);
    assert_false(req.keep_alive); /* el cliente pidió cerrar SU conexión */

    char    out[2048];
    ssize_t n = hp_rewrite(&req, text, strlen(text), "203.0.113.7", "http", true,
                           out, sizeof out);
    assert_true(n > 0);
    out[n] = '\0';

    assert_non_null(strstr(out, "Connection: keep-alive\r\n"));
    assert_null(strstr(out, "Connection: close\r\n"));
    assert_null(strstr(out, "Proxy-Connection"));
    assert_non_null(strstr(out, "Accept: */*\r\n")); /* las demás se conservan */

    /* Y al revés: si no queremos reutilizar, se lo decimos al upstream. */
    n = hp_rewrite(&req, text, strlen(text), "203.0.113.7", "http", false, out,
                   sizeof out);
    assert_true(n > 0);
    out[n] = '\0';
    assert_non_null(strstr(out, "Connection: close\r\n"));
}

static void test_request_body_framing(void **state)
{
    (void)state;

    http_request req;

    hp_init(&req);
    static const char *get = "GET / HTTP/1.1\r\nHost: a.test\r\n\r\n";
    assert_int_equal(hp_execute(&req, get, strlen(get)), HP_DONE);
    assert_int_equal(req.body_mode, HP_BODY_NONE);
    assert_false(req.is_head);

    hp_init(&req);
    static const char *post = "POST /x HTTP/1.1\r\nHost: a.test\r\n"
                              "Content-Length: 11\r\n\r\nhola mundo!";
    assert_int_equal(hp_execute(&req, post, strlen(post)), HP_DONE);
    assert_int_equal(req.body_mode, HP_BODY_LENGTH);
    assert_int_equal((int)req.content_length, 11);

    hp_init(&req);
    static const char *head = "HEAD / HTTP/1.1\r\nHost: a.test\r\n\r\n";
    assert_int_equal(hp_execute(&req, head, strlen(head)), HP_DONE);
    assert_true(req.is_head);

    /* Un Content-Length con basura no se interpreta "a lo que parezca": de ahí
     * salen las discrepancias que permiten colar una petición dentro de otra. */
    hp_init(&req);
    static const char *bad = "POST / HTTP/1.1\r\nHost: a.test\r\n"
                             "Content-Length: 12abc\r\n\r\n";
    assert_int_equal(hp_execute(&req, bad, strlen(bad)), HP_ERROR);
    assert_int_equal(req.error_code, 400);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_content_length),
        cmocka_unit_test(test_chunked_beats_length),
        cmocka_unit_test(test_bodyless_responses),
        cmocka_unit_test(test_no_delimiter_forces_close),
        cmocka_unit_test(test_response_needs_full_header),
        cmocka_unit_test(test_chunked_scanner),
        cmocka_unit_test(test_chunked_extensions_and_garbage),
        cmocka_unit_test(test_rewrite_replaces_hop_headers),
        cmocka_unit_test(test_request_body_framing),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
