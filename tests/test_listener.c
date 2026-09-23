/* test_listener.c — socket de escucha y accept sobre el bucle de eventos.
 *
 * No entra en los 22 casos del README (que no lista una suite de listener),
 * pero sí comprueba en las tres plataformas lo que más difiere entre ellas:
 * SO_REUSEPORT y el accept no bloqueante.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "listener.h"
#include "net_compat.h"

/* Frontend mínimo a mano: puerto 0 para que lo elija el kernel y el test no
 * dependa de que un puerto fijo esté libre en la máquina de CI. */
static cfg_frontend make_frontend(uint16_t port)
{
    static char name[]   = "test";
    static char listen[] = "127.0.0.1:0";
    static char host[]   = "127.0.0.1";

    cfg_frontend f;
    memset(&f, 0, sizeof f);
    f.name        = name;
    f.listen      = listen;
    f.listen_host = host;
    f.listen_port = port;
    return f;
}

static void test_open_binds_and_reports_port(void **state)
{
    (void)state;

    cfg_frontend f         = make_frontend(0);
    char         err[256]  = { 0 };
    listener    *l         = listener_open(&f, 0, err, sizeof err);

    assert_non_null(l);
    assert_string_equal(err, "");
    assert_true(listener_fd(l) >= 0);
    assert_true(listener_port(l) > 0); /* el kernel eligió uno */

    listener_close(l);
}

/* E5: dos procesos (aquí, dos sockets) comparten puerto. Si esto falla, un
 * worker por CPU es imposible. */
static void test_reuseport_allows_two_listeners(void **state)
{
    (void)state;

    cfg_frontend f        = make_frontend(0);
    char         err[256] = { 0 };

    listener *a = listener_open(&f, 0, err, sizeof err);
    assert_non_null(a);

    uint16_t port = listener_port(a);
    assert_true(port > 0);

    /* el segundo pide explícitamente el puerto que ya tiene el primero */
    cfg_frontend f2 = make_frontend(port);
    listener    *b  = listener_open(&f2, 0, err, sizeof err);
    assert_non_null(b);
    assert_int_equal(listener_port(b), port);

    /* Deja constancia de qué opción usa esta plataforma y si reparte carga:
     * en macOS no lo hace, y eso cambia el diseño del worker. */
    assert_non_null(net_reuseport_name());
    printf("        [%s, reparte carga: %s]\n", net_reuseport_name(),
           net_reuseport_balances() ? "sí" : "no");

    listener_close(b);
    listener_close(a);
}

typedef struct {
    io_loop *loop;
    int      accepted_fd;
    int      calls;
    size_t   frontend_index;
} accept_ctx;

static void on_accept(int client_fd, const struct sockaddr_storage *peer,
                      size_t frontend_index, void *ctx)
{
    (void)peer;
    accept_ctx *a = ctx;

    a->calls++;
    a->accepted_fd    = client_fd;
    a->frontend_index = frontend_index;
    io_loop_stop(a->loop);
}

static void test_accepts_a_connection(void **state)
{
    (void)state;

    cfg_frontend f        = make_frontend(0);
    char         err[256] = { 0 };
    listener    *l        = listener_open(&f, 7, err, sizeof err);
    assert_non_null(l);

    io_loop *loop = io_loop_create();
    assert_non_null(loop);

    accept_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.loop        = loop;
    ctx.accepted_fd = -1;

    assert_int_equal(listener_attach(l, loop, on_accept, &ctx), 0);

    /* Cliente conectado ANTES de correr el bucle: el evento ya está pendiente
     * y io_loop_run no puede quedarse esperando para siempre. */
    int c = socket(AF_INET, SOCK_STREAM, 0);
    assert_true(c >= 0);

    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port   = htons(listener_port(l));
    assert_int_equal(inet_pton(AF_INET, "127.0.0.1", &to.sin_addr), 1);
    assert_int_equal(connect(c, (struct sockaddr *)&to, sizeof to), 0);

    assert_int_equal(io_loop_run(loop), 0);

    assert_int_equal(ctx.calls, 1);
    assert_true(ctx.accepted_fd >= 0);
    /* el índice de frontend viaja con la conexión: es lo que elige la tabla */
    assert_int_equal(ctx.frontend_index, 7);

    close(ctx.accepted_fd);
    close(c);
    io_loop_del(loop, listener_fd(l));
    io_loop_destroy(loop);
    listener_close(l);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_open_binds_and_reports_port),
        cmocka_unit_test(test_reuseport_allows_two_listeners),
        cmocka_unit_test(test_accepts_a_connection),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
