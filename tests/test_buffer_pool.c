/* test_buffer_pool.c — arena mmap y lista libre (E16).
 *
 * Fuera de los 22 casos del README, que no lista una suite para este módulo.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "buffer_pool.h"

static void test_get_until_empty_and_recover(void **state)
{
    (void)state;

    buffer_pool *p = bufpool_create(4);
    assert_non_null(p);
    assert_int_equal(bufpool_capacity(p), 4);
    assert_int_equal(bufpool_available(p), 4);

    void *slots[4];
    for (int i = 0; i < 4; i++) {
        slots[i] = bufpool_get(p);
        assert_non_null(slots[i]);
    }
    assert_int_equal(bufpool_available(p), 0);

    /* Agotarse no es un error: es la señal para rechazar la conexión. */
    assert_null(bufpool_get(p));

    bufpool_put(p, slots[2]);
    assert_int_equal(bufpool_available(p), 1);
    assert_non_null(bufpool_get(p));

    for (int i = 0; i < 4; i++) {
        bufpool_put(p, slots[i]);
    }
    assert_int_equal(bufpool_available(p), 4);

    bufpool_destroy(p);
}

/* LIFO: el último devuelto es el siguiente en salir. Mantiene caliente la
 * misma memoria en vez de pasear por toda la arena. */
static void test_free_list_is_lifo(void **state)
{
    (void)state;

    buffer_pool *p = bufpool_create(3);
    assert_non_null(p);

    void *a = bufpool_get(p);
    void *b = bufpool_get(p);
    assert_ptr_not_equal(a, b);

    bufpool_put(p, a);
    bufpool_put(p, b);

    assert_ptr_equal(bufpool_get(p), b);
    assert_ptr_equal(bufpool_get(p), a);

    bufpool_destroy(p);
}

static void test_slots_do_not_overlap(void **state)
{
    (void)state;

    buffer_pool *p = bufpool_create(3);
    assert_non_null(p);

    unsigned char *a = bufpool_get(p);
    unsigned char *b = bufpool_get(p);
    unsigned char *c = bufpool_get(p);
    assert_non_null(a);
    assert_non_null(b);
    assert_non_null(c);

    /* Llenar un slot entero no debe tocar a los vecinos. */
    memset(a, 0xAA, BUFPOOL_SLOT_SIZE);
    memset(b, 0xBB, BUFPOOL_SLOT_SIZE);
    memset(c, 0xCC, BUFPOOL_SLOT_SIZE);

    for (size_t i = 0; i < BUFPOOL_SLOT_SIZE; i++) {
        assert_int_equal(a[i], 0xAA);
        assert_int_equal(b[i], 0xBB);
        assert_int_equal(c[i], 0xCC);
    }

    bufpool_destroy(p);
}

/* Devolver algo ajeno corrompería la lista libre y el fallo saldría mucho
 * después y en otro sitio: se ignora en el momento. */
static void test_foreign_pointer_is_rejected(void **state)
{
    (void)state;

    buffer_pool *p = bufpool_create(2);
    assert_non_null(p);

    void *slot = bufpool_get(p);
    assert_non_null(slot);
    assert_int_equal(bufpool_available(p), 1);

    unsigned char stack_buf[64];
    bufpool_put(p, stack_buf);
    assert_int_equal(bufpool_available(p), 1); /* no lo ha aceptado */

    /* Tampoco un puntero a mitad de slot. */
    bufpool_put(p, (unsigned char *)slot + 7);
    assert_int_equal(bufpool_available(p), 1);

    bufpool_put(p, slot);
    assert_int_equal(bufpool_available(p), 2);

    bufpool_destroy(p);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_get_until_empty_and_recover),
        cmocka_unit_test(test_free_list_is_lifo),
        cmocka_unit_test(test_slots_do_not_overlap),
        cmocka_unit_test(test_foreign_pointer_is_rejected),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
