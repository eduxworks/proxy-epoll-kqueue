/* buffer_pool.c — arena mmap con lista libre intrusiva.
 *
 * La lista libre no necesita memoria aparte: mientras un slot está libre nadie
 * mira su contenido, así que el puntero al siguiente se guarda dentro del
 * propio slot. Coste de estructura: cero.
 */

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "buffer_pool.h"

struct buffer_pool {
    unsigned char *arena;
    size_t         bytes;
    size_t         capacity;
    size_t         available;
    void          *free_list;
};

buffer_pool *bufpool_create(size_t n_slots)
{
    if (n_slots == 0) {
        return NULL;
    }

    buffer_pool *p = calloc(1, sizeof *p);
    if (p == NULL) {
        return NULL;
    }

    p->bytes = n_slots * BUFPOOL_SLOT_SIZE;
    p->arena = mmap(NULL, p->bytes, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p->arena == MAP_FAILED) {
        free(p);
        return NULL;
    }

    p->capacity  = n_slots;
    p->available = n_slots;

    /* Se encadenan al revés para que el primer get() devuelva el slot 0: no
     * cambia nada funcional, pero hace los volcados legibles al depurar. */
    p->free_list = NULL;
    for (size_t i = n_slots; i > 0; i--) {
        void *slot = p->arena + (i - 1) * BUFPOOL_SLOT_SIZE;
        memcpy(slot, &p->free_list, sizeof p->free_list);
        p->free_list = slot;
    }

    return p;
}

void bufpool_destroy(buffer_pool *p)
{
    if (p == NULL) {
        return;
    }
    if (p->arena != NULL && p->arena != MAP_FAILED) {
        munmap(p->arena, p->bytes);
    }
    free(p);
}

void *bufpool_get(buffer_pool *p)
{
    if (p == NULL || p->free_list == NULL) {
        return NULL;
    }

    void *slot = p->free_list;
    memcpy(&p->free_list, slot, sizeof p->free_list);
    p->available--;
    return slot;
}

void bufpool_put(buffer_pool *p, void *slot)
{
    if (p == NULL || slot == NULL) {
        return;
    }

    /* Devolver algo que no salió de esta arena corrompería la lista libre en
     * silencio y el fallo aparecería mucho después, en otro sitio. */
    unsigned char *s = slot;
    if (s < p->arena || s >= p->arena + p->bytes ||
        (size_t)(s - p->arena) % BUFPOOL_SLOT_SIZE != 0) {
        return;
    }

    memcpy(slot, &p->free_list, sizeof p->free_list);
    p->free_list = slot;
    p->available++;
}

size_t bufpool_capacity(const buffer_pool *p)
{
    return p != NULL ? p->capacity : 0;
}

size_t bufpool_available(const buffer_pool *p)
{
    return p != NULL ? p->available : 0;
}
