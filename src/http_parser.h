/* http_parser.h — cabecera de petición HTTP/1.x (E7, E14).
 *
 * El proxy es de nivel 7 pero no necesita entender el cuerpo: le basta la
 * cabecera para decidir a dónde va la conexión. A partir de ahí el relay es
 * ciego, así que el parser solo cubre línea de petición y cabeceras.
 *
 * hp_execute admite datos a trozos: se le pasa siempre el buffer acumulado
 * desde el principio y responde HP_NEED_MORE mientras falte el CRLF CRLF.
 */
#ifndef PROXY_HTTP_PARSER_H
#define PROXY_HTTP_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* El límite existe para que nadie agote la memoria del proxy mandando
 * cabeceras sin fin: al superarlo se responde 431 y se cierra. */
#define HP_MAX_HEADER_BYTES 8192
#define HP_MAX_HOST_LEN     256

typedef enum {
    HP_NEED_MORE = 0, /* falta cabecera por leer */
    HP_DONE,          /* cabecera completa y válida */
    HP_ERROR          /* mirar error_code */
} hp_status;

typedef struct {
    hp_status status;
    int       error_code; /* 400, 431 o 505 cuando status == HP_ERROR */

    size_t header_len; /* bytes hasta el final del CRLF CRLF, incluido */

    char host[HP_MAX_HOST_LEN]; /* tal como vino, con puerto si lo traía */
    bool has_host;

    int  minor_version; /* 0 para HTTP/1.0, 1 para HTTP/1.1 */
    bool keep_alive;
    bool has_xff; /* ya traía X-Forwarded-For */
} http_request;

void      hp_init(http_request *req);
hp_status hp_execute(http_request *req, const char *buf, size_t len);

/* Escribe en out la cabecera que va al upstream: las originales menos las
 * X-Forwarded-*, más las nuestras. X-Forwarded-For se ENCADENA al valor previo
 * en vez de sustituirlo, que es lo que permite seguir la traza de proxies.
 * Devuelve bytes escritos, o -1 si no cabe. */
ssize_t hp_rewrite(const http_request *req, const char *buf, size_t len,
                   const char *client_ip, const char *proto, char *out,
                   size_t outlen);

#endif /* PROXY_HTTP_PARSER_H */
