/* test_io_event.c — suite cmocka de la abstracción epoll/kqueue.
 *
 * Estos tests corren igual en Linux, FreeBSD y macOS: verifican la API común,
 * no la implementación. Si el mismo binario de tests pasa en las tres
 * plataformas, E3 está demostrado y no solo afirmado.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "io_event.h"

static void test_loop_create_destroy(void **state)
{
    (void)state;

    io_loop *loop = io_loop_create();
    assert_non_null(loop);
    io_loop_destroy(loop);

    /* destruir NULL debe ser inocuo, para simplificar las rutas de error */
    io_loop_destroy(NULL);
}

static void test_backend_name(void **state)
{
    (void)state;

    const char *name = io_backend_name();
    assert_non_null(name);

#if defined(__linux__)
    assert_string_equal(name, "epoll");
#else
    assert_string_equal(name, "kqueue");
#endif
}

static void test_set_nonblocking(void **state)
{
    (void)state;

    int fds[2];
    assert_int_equal(pipe(fds), 0);

    assert_int_equal(io_set_nonblocking(fds[0]), 0);
    assert_true((fcntl(fds[0], F_GETFL, 0) & O_NONBLOCK) != 0);

    /* idempotente: llamarlo dos veces no debe fallar */
    assert_int_equal(io_set_nonblocking(fds[0]), 0);

    close(fds[0]);
    close(fds[1]);
}

static void test_add_del_unknown_fd(void **state)
{
    (void)state;

    io_loop *loop = io_loop_create();
    assert_non_null(loop);

    /* borrar algo no registrado es ENOENT, no un cierre en falso */
    assert_int_equal(io_loop_del(loop, 3), -1);
    assert_int_equal(errno, ENOENT);

    assert_int_equal(io_loop_mod(loop, 3, IO_READ), -1);
    assert_int_equal(errno, ENOENT);

    io_loop_destroy(loop);
}

/* --- despertar real del bucle ------------------------------------------- */

typedef struct {
    io_loop *loop;
    int      calls;
    size_t   bytes;
    unsigned last_events;
} wake_ctx;

/* Handler modelo: drena hasta EAGAIN, como exige edge-triggered (E4). */
static void on_readable(int fd, unsigned events, void *ctx)
{
    wake_ctx *w = ctx;
    w->calls++;
    w->last_events = events;

    for (;;) {
        char    buf[32];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            w->bytes += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    io_loop_stop(w->loop);
}

static void test_wakeup_on_pending_data(void **state)
{
    (void)state;

    int fds[2];
    assert_int_equal(pipe(fds), 0);
    assert_int_equal(io_set_nonblocking(fds[0]), 0);

    io_loop *loop = io_loop_create();
    assert_non_null(loop);

    wake_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.loop = loop;

    assert_int_equal(io_loop_add(loop, fds[0], IO_READ, on_readable, &ctx), 0);

    /* Escribimos ANTES de correr: el evento ya está pendiente, así que
     * io_loop_run no puede quedarse esperando indefinidamente. */
    assert_int_equal(write(fds[1], "hola", 4), 4);

    assert_int_equal(io_loop_run(loop), 0);

    assert_int_equal(ctx.calls, 1);
    assert_int_equal(ctx.bytes, 4);
    assert_true((ctx.last_events & IO_READ) != 0);

    assert_int_equal(io_loop_del(loop, fds[0]), 0);
    io_loop_destroy(loop);
    close(fds[0]);
    close(fds[1]);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_loop_create_destroy),
        cmocka_unit_test(test_backend_name),
        cmocka_unit_test(test_set_nonblocking),
        cmocka_unit_test(test_add_del_unknown_fd),
        cmocka_unit_test(test_wakeup_on_pending_data),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
