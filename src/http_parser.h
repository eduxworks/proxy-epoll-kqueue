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
#include <stdint.h>
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

/* Cómo se sabe dónde acaba el cuerpo. Sin esto no hay keep-alive: si no puedes
 * delimitar una respuesta, no sabes dónde empieza la siguiente y lo único
 * seguro es cerrar. */
typedef enum {
    HP_BODY_NONE = 0,   /* 1xx, 204, 304, respuesta a HEAD */
    HP_BODY_LENGTH,     /* Content-Length */
    HP_BODY_CHUNKED,    /* Transfer-Encoding: chunked */
    HP_BODY_UNTIL_CLOSE /* sin delimitador: acaba al cerrar el socket */
} hp_body_mode;

typedef struct {
    hp_status status;
    int       error_code; /* 400, 431 o 505 cuando status == HP_ERROR */

    size_t header_len; /* bytes hasta el final del CRLF CRLF, incluido */

    char host[HP_MAX_HOST_LEN]; /* tal como vino, con puerto si lo traía */
    bool has_host;

    int  minor_version; /* 0 para HTTP/1.0, 1 para HTTP/1.1 */
    bool keep_alive;
    bool has_xff; /* ya traía X-Forwarded-For */
    bool is_head; /* la respuesta a HEAD no lleva cuerpo, diga lo que diga */

    hp_body_mode body_mode;
    uint64_t     content_length;
} http_request;

void      hp_init(http_request *req);
hp_status hp_execute(http_request *req, const char *buf, size_t len);

/* --- respuesta ------------------------------------------------------------ */

typedef struct {
    hp_status status;
    int       error_code;

    size_t header_len;
    int    status_code;
    int    minor_version;

    hp_body_mode body_mode;
    uint64_t     content_length;
    bool         keep_alive; /* lo que permite la respuesta del upstream */
} http_response;

void      hp_response_init(http_response *res);
hp_status hp_response_execute(http_response *res, const char *buf, size_t len,
                              bool head_request);

/* --- recuento de un cuerpo chunked ---------------------------------------- */

/* Escáner incremental: no des-trocea nada, solo cuenta para saber dónde acaba
 * el cuerpo. El proxy reenvía los bytes tal cual. */
typedef struct {
    int      state;
    uint64_t remaining;
    int      line_len;
    bool     done;
    bool     error;
} hp_chunked;

void hp_chunked_init(hp_chunked *ch);

/* Consume hasta len bytes y devuelve cuántos pertenecían al cuerpo. Si el
 * cuerpo termina antes, el resto son bytes de después (que en este proxy no
 * deberían existir: no se encadenan peticiones). */
size_t hp_chunked_feed(hp_chunked *ch, const char *buf, size_t len);

/* Escribe en out la cabecera que va al upstream: las originales menos las
 * X-Forwarded-* y las de salto (Connection, Proxy-Connection), más las
 * nuestras. X-Forwarded-For se ENCADENA al valor previo en vez de sustituirlo,
 * que es lo que permite seguir la traza de proxies.
 *
 * Connection es de salto a salto: la del cliente describe su enlace con el
 * proxy, no el del proxy con el upstream, así que se descarta y se emite la
 * que decidimos nosotros. Devuelve bytes escritos, o -1 si no cabe. */
ssize_t hp_rewrite(const http_request *req, const char *buf, size_t len,
                   const char *client_ip, const char *proto,
                   bool upstream_keep_alive, char *out, size_t outlen);

#endif /* PROXY_HTTP_PARSER_H */
