/* log.c — anillo de un productor y un consumidor.
 *
 * El productor escribe la ranura y publica `head` con release; el consumidor
 * lee `head` con acquire. Ese par es lo único que ordena las escrituras del
 * mensaje respecto a su publicación: sin él, el consumidor podría ver el
 * índice nuevo y el contenido viejo.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

typedef struct {
    uint16_t len;
    char     text[LOG_SLOT_BYTES];
} log_slot;

static struct {
    log_slot slots[LOG_RING_SLOTS];

    _Atomic unsigned long head; /* solo lo escribe el productor */
    _Atomic unsigned long tail; /* solo lo escribe el consumidor */
    _Atomic unsigned long dropped;

    _Atomic int level;
    _Atomic bool running;

    int       fd;
    bool      own_fd;
    pthread_t thread;
    bool      started;
} L;

static const char *LEVEL_NAMES[] = { "DEBUG", "INFO", "WARN", "ERROR" };

const char *log_level_name(log_level lvl)
{
    return LEVEL_NAMES[lvl <= LOG_ERROR ? lvl : LOG_ERROR];
}

bool log_level_parse(const char *s, log_level *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    if (strcmp(s, "debug") == 0) { *out = LOG_DEBUG; return true; }
    if (strcmp(s, "info")  == 0) { *out = LOG_INFO;  return true; }
    if (strcmp(s, "warn")  == 0) { *out = LOG_WARN;  return true; }
    if (strcmp(s, "error") == 0) { *out = LOG_ERROR; return true; }
    return false;
}

void log_set_level(log_level lvl)
{
    atomic_store_explicit(&L.level, (int)lvl, memory_order_relaxed);
}

log_level log_get_level(void)
{
    return (log_level)atomic_load_explicit(&L.level, memory_order_relaxed);
}

unsigned long log_dropped(void)
{
    return atomic_load_explicit(&L.dropped, memory_order_relaxed);
}

/* --- consumidor ----------------------------------------------------------- */

/* Vacía lo que haya. Devuelve cuántas líneas escribió. */
static size_t drain(void)
{
    unsigned long tail = atomic_load_explicit(&L.tail, memory_order_relaxed);
    unsigned long head = atomic_load_explicit(&L.head, memory_order_acquire);

    size_t written = 0;
    while (tail != head) {
        log_slot *s = &L.slots[tail % LOG_RING_SLOTS];

        size_t off = 0;
        while (off < s->len) {
            ssize_t n = write(L.fd, s->text + off, s->len - off);
            if (n > 0) {
                off += (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break; /* disco lleno o fd roto: se pierde la línea, no el proxy */
        }

        tail++;
        written++;
        /* Publicar tras cada línea deja sitio al productor cuanto antes. */
        atomic_store_explicit(&L.tail, tail, memory_order_release);
    }
    return written;
}

static void *consumer(void *arg)
{
    (void)arg;

    while (atomic_load_explicit(&L.running, memory_order_acquire)) {
        if (drain() == 0) {
            /* Vacío: se duerme en vez de girar. 2 ms es invisible para un
             * registro y evita quemar una CPU entera esperando. */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }

    drain(); /* lo que quedara al parar */
    return NULL;
}

/* --- API ------------------------------------------------------------------ */

int log_init(const char *path, log_level level)
{
    if (L.started) {
        return 0;
    }

    memset(&L, 0, sizeof L);
    atomic_init(&L.head, 0);
    atomic_init(&L.tail, 0);
    atomic_init(&L.dropped, 0);
    atomic_init(&L.level, (int)level);
    atomic_init(&L.running, true);

    if (path == NULL || strcmp(path, "-") == 0) {
        L.fd     = STDERR_FILENO;
        L.own_fd = false;
    } else {
        L.fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (L.fd < 0) {
            return -1;
        }
        L.own_fd = true;
    }

    if (pthread_create(&L.thread, NULL, consumer, NULL) != 0) {
        if (L.own_fd) {
            close(L.fd);
        }
        return -1;
    }

    L.started = true;
    return 0;
}

void log_shutdown(void)
{
    if (!L.started) {
        return;
    }

    atomic_store_explicit(&L.running, false, memory_order_release);
    pthread_join(L.thread, NULL);

    if (L.own_fd && L.fd >= 0) {
        close(L.fd);
    }
    L.fd      = -1;
    L.started = false;
}

void log_write(log_level lvl, const char *fmt, ...)
{
    if ((int)lvl < atomic_load_explicit(&L.level, memory_order_relaxed)) {
        return;
    }
    if (!L.started) {
        return;
    }

    unsigned long head = atomic_load_explicit(&L.head, memory_order_relaxed);
    unsigned long tail = atomic_load_explicit(&L.tail, memory_order_acquire);

    /* Lleno. Descartar es deliberado: bloquear aquí sería bloquear el bucle de
     * eventos, que es justo lo que este módulo existe para evitar. */
    if (head - tail >= LOG_RING_SLOTS) {
        atomic_fetch_add_explicit(&L.dropped, 1, memory_order_relaxed);
        return;
    }

    log_slot *s = &L.slots[head % LOG_RING_SLOTS];

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    gmtime_r(&ts.tv_sec, &tmv);

    int n = snprintf(s->text, LOG_SLOT_BYTES,
                     "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ %-5s ",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                     tmv.tm_min, tmv.tm_sec, ts.tv_nsec / 1000000,
                     log_level_name(lvl));
    if (n < 0) {
        n = 0;
    }
    if (n > LOG_SLOT_BYTES - 2) {
        n = LOG_SLOT_BYTES - 2;
    }

    /* Se reserva el último byte para el salto de línea. */
    const size_t cap = LOG_SLOT_BYTES - 1;

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(s->text + n, cap - (size_t)n, fmt, ap);
    va_end(ap);
    if (m < 0) {
        m = 0;
    }

    /* vsnprintf devuelve lo que HABRÍA escrito, no lo que escribió, y deja su
     * terminador dentro del buffer. Usar el valor de retorno para colocar el
     * salto de línea metía un byte NUL en medio de la línea cuando el mensaje
     * no cabía. Se usa lo realmente escrito. */
    size_t room    = cap - (size_t)n - 1; /* menos el NUL de vsnprintf */
    size_t written = (size_t)m < room ? (size_t)m : room;

    size_t len     = (size_t)n + written;
    s->text[len++] = '\n';
    s->len         = (uint16_t)len;

    /* Release: el contenido de la ranura queda visible antes que el índice. */
    atomic_store_explicit(&L.head, head + 1, memory_order_release);
}
