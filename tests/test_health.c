/* test_health.c — sondas activas (E11).
 *
 * Fuera de los 22 casos del README. Lo que se comprueba es lo que las sondas
 * aportan frente a la salud pasiva: que un backend caído se detecta sin que
 * ningún cliente tropiece, y sobre todo que uno que vuelve se readmite —cosa
 * imposible solo con salud pasiva, porque a un pool entero caído ya no se le
 * enruta nada y por tanto no hay intentos que puedan salir bien.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cmocka.h>

#include "health.h"
#include "log.h"

/* Un socket que escucha y no hace nada más: para una sonda TCP, existir basta. */
static int open_listener(uint16_t *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert_true(fd >= 0);

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;

    assert_int_equal(bind(fd, (struct sockaddr *)&a, sizeof a), 0);
    assert_int_equal(listen(fd, 16), 0);

    socklen_t len = sizeof a;
    assert_int_equal(getsockname(fd, (struct sockaddr *)&a, &len), 0);
    *port = ntohs(a.sin_port);
    return fd;
}

static router *build_router(uint16_t port)
{
    char toml[1024];
    snprintf(toml, sizeof toml,
             "[[frontend]]\n"
             "listen = \"127.0.0.1:18081\"\n"
             "  [[frontend.route]]\n"
             "  host    = \"a.test\"\n"
             "  backend = \"pool\"\n"
             "[[backend]]\n"
             "name    = \"pool\"\n"
             "servers = [ { addr = \"127.0.0.1:%u\" } ]\n"
             "  [backend.health]\n"
             "  type     = \"tcp\"\n"
             "  interval = 100\n"
             "  timeout  = 50\n"
             "  rise     = 1\n"
             "  fall     = 1\n",
             (unsigned)port);

    char    err[256] = { 0 };
    config *cfg      = config_parse(toml, err, sizeof err);
    if (cfg == NULL) {
        fail_msg("config inválida: %s", err);
    }
    router *r = router_build(cfg);
    assert_non_null(r);
    return r;
}

/* Espera a que el estado sea el esperado, o se rinde. Sondear lleva tiempo y
 * dormir una cantidad fija a ojo produce tests que fallan un día de cada
 * veinte en una máquina cargada. */
static bool wait_for_state(router *r, bool want_up, int max_ms)
{
    backend_pool *p = router_pool_at(r, 0);
    for (int waited = 0; waited < max_ms; waited += 20) {
        if (backend_is_up(pool_backend_at(p, 0)) == want_up) {
            return true;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 20L * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return false;
}

static void test_detects_down_and_recovery(void **state)
{
    (void)state;

    uint16_t port = 0;
    int      lfd  = open_listener(&port);

    router *r  = build_router(port);
    health *hc = health_start(r);
    assert_non_null(hc);

    /* Con el puerto escuchando, la sonda lo confirma arriba. */
    assert_true(wait_for_state(r, true, 2000));

    /* Se cierra el socket: nadie escucha, la sonda debe verlo caer. */
    close(lfd);
    assert_true(wait_for_state(r, false, 3000));

    /* Y al volver a escuchar en el MISMO puerto, la sonda lo readmite. Esto es
     * lo que la salud pasiva no puede hacer: sin tráfico que enrutar no hay
     * forma de descubrir que el backend ya responde. */
    int again = socket(AF_INET, SOCK_STREAM, 0);
    assert_true(again >= 0);
    int on = 1;
    setsockopt(again, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons(port);
    assert_int_equal(bind(again, (struct sockaddr *)&a, sizeof a), 0);
    assert_int_equal(listen(again, 16), 0);

    assert_true(wait_for_state(r, true, 3000));

    health_stop(hc);
    close(again);
    router_unref(r);
}

/* Tras una recarga el hilo debe pasar a sondear los pools nuevos; si siguiera
 * con los viejos estaría escribiendo en memoria que el reload va a liberar. */
static void test_router_handover(void **state)
{
    (void)state;

    uint16_t port = 0;
    int      lfd  = open_listener(&port);

    router *first = build_router(port);
    health *hc    = health_start(first);
    assert_non_null(hc);
    assert_true(wait_for_state(first, true, 2000));

    router *second = build_router(port);
    health_set_router(hc, second);

    /* La referencia del reload la suelta quien publica; aquí basta con que el
     * hilo siga vivo y sondeando el nuevo. */
    router_unref(first);
    assert_true(wait_for_state(second, true, 2000));

    unsigned long c0 = health_cycles(hc);
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 400L * 1000 * 1000 };
    nanosleep(&ts, NULL);
    assert_true(health_cycles(hc) > c0);

    health_stop(hc);
    close(lfd);
    router_unref(second);
}

static void test_stop_without_start_is_safe(void **state)
{
    (void)state;
    health_stop(NULL);
    assert_null(health_start(NULL));
}

int main(void)
{
    /* Las sondas registran los cambios de estado; sin registro iniciado esas
     * llamadas son inocuas, pero así también se ejercita ese camino. */
    log_init("-", LOG_ERROR);

    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_detects_down_and_recovery),
        cmocka_unit_test(test_router_handover),
        cmocka_unit_test(test_stop_without_start_is_safe),
    };

    int rc = cmocka_run_group_tests(tests, NULL, NULL);
    log_shutdown();
    return rc;
}
