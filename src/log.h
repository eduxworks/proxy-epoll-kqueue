/* log.h — registro asíncrono (E15).
 *
 * El bucle de eventos no escribe a disco: deja el mensaje en un buffer
 * circular y sigue. Un hilo aparte lo vacía al fichero. Escribir desde el loop
 * lo bloquearía en cada `write()` lento, y con 300.000 peticiones por segundo
 * eso no es un detalle.
 *
 * El anillo es de 4096 ranuras de 512 B, como fija el README: 2 MB por worker,
 * reservados una sola vez.
 *
 * Un productor y un consumidor. Cada worker es un proceso con su propio bucle
 * y su propio hilo de registro, así que no hay productores concurrentes y el
 * anillo no necesita cerrojos.
 */
#ifndef PROXY_LOG_H
#define PROXY_LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#define LOG_RING_SLOTS 4096
#define LOG_SLOT_BYTES 512

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} log_level;

/* path NULL o "-" => stderr. Devuelve 0, o -1 con errno. */
int  log_init(const char *path, log_level level);

/* Vacía lo pendiente y para el hilo. Idempotente. */
void log_shutdown(void);

/* Nunca bloquea. Si el anillo está lleno el mensaje se descarta y se cuenta:
 * perder líneas de registro es preferible a frenar el proxy. */
void log_write(log_level lvl, const char *fmt, ...);

void      log_set_level(log_level lvl);
log_level log_get_level(void);

/* Mensajes descartados por anillo lleno. Es la señal de que el disco no sigue
 * el ritmo, y va al snapshot de estadísticas. */
unsigned long log_dropped(void);

/* "debug"|"info"|"warn"|"error" -> enum. false si no encaja. */
bool log_level_parse(const char *s, log_level *out);
const char *log_level_name(log_level lvl);

#endif /* PROXY_LOG_H */
