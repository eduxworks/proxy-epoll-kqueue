/* http_parser.c — línea de petición y cabeceras HTTP/1.x.
 *
 * Solo se acepta CRLF como fin de línea. Ser indulgente con LF suelto invita a
 * discrepancias entre lo que ve el proxy y lo que ve el upstream, que es de
 * donde salen los ataques de contrabando de peticiones.
 */

#include <stdio.h>
#include <string.h>

#include "http_parser.h"

static bool ieq(const char *a, size_t alen, const char *lit)
{
    size_t n = strlen(lit);
    if (alen != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char ca = a[i];
        char cb = lit[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

static const char *find_crlf(const char *p, const char *end)
{
    for (const char *q = p; q + 1 < end; q++) {
        if (q[0] == '\r' && q[1] == '\n') {
            return q;
        }
    }
    return NULL;
}

static const char *find_header_end(const char *buf, size_t len)
{
    if (len < 4) {
        return NULL;
    }
    for (size_t i = 0; i + 3 < len; i++) {
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
            return buf + i + 4;
        }
    }
    return NULL;
}

void hp_init(http_request *req)
{
    memset(req, 0, sizeof *req);
    req->status        = HP_NEED_MORE;
    req->minor_version = 1;
    req->keep_alive    = true;
    req->body_mode     = HP_BODY_NONE;
}

/* Devuelve false si el valor no es un entero decimal limpio: un
 * Content-Length con basura no se interpreta "a lo que parezca", porque de ahí
 * salen las discrepancias que permiten colar una petición dentro de otra. */
static bool parse_u64(const char *s, size_t len, uint64_t *out)
{
    if (len == 0 || len > 19) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return true;
}

static hp_status fail(http_request *req, int code)
{
    req->status     = HP_ERROR;
    req->error_code = code;
    return HP_ERROR;
}

/* Divide "Nombre: valor" recortando el espacio opcional tras los dos puntos. */
static bool split_header(const char *line, const char *eol, const char **name,
                         size_t *namelen, const char **value, size_t *valuelen)
{
    const char *colon = memchr(line, ':', (size_t)(eol - line));
    if (colon == NULL || colon == line) {
        return false;
    }

    *name    = line;
    *namelen = (size_t)(colon - line);

    const char *v = colon + 1;
    while (v < eol && (*v == ' ' || *v == '\t')) {
        v++;
    }
    const char *e = eol;
    while (e > v && (e[-1] == ' ' || e[-1] == '\t')) {
        e--;
    }

    *value    = v;
    *valuelen = (size_t)(e - v);
    return true;
}

hp_status hp_execute(http_request *req, const char *buf, size_t len)
{
    if (req->status != HP_NEED_MORE) {
        return req->status;
    }

    const char *header_end = find_header_end(buf, len);
    if (header_end == NULL) {
        /* Aún incompleta: solo se corta si ya se pasó del límite. */
        if (len > HP_MAX_HEADER_BYTES) {
            return fail(req, 431);
        }
        return HP_NEED_MORE;
    }

    size_t header_len = (size_t)(header_end - buf);
    if (header_len > HP_MAX_HEADER_BYTES) {
        return fail(req, 431);
    }
    req->header_len = header_len;

    const char *end = buf + header_len;

    /* --- línea de petición: METHOD SP TARGET SP HTTP/1.x --- */
    const char *eol = find_crlf(buf, end);
    if (eol == NULL) {
        return fail(req, 400);
    }

    const char *sp1 = memchr(buf, ' ', (size_t)(eol - buf));
    if (sp1 == NULL || sp1 == buf) {
        return fail(req, 400);
    }
    req->is_head = ieq(buf, (size_t)(sp1 - buf), "HEAD");
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1));
    if (sp2 == NULL || sp2 == sp1 + 1) {
        return fail(req, 400);
    }

    const char *ver    = sp2 + 1;
    size_t      verlen = (size_t)(eol - ver);
    if (verlen != 8 || memcmp(ver, "HTTP/1.", 7) != 0) {
        return fail(req, 505);
    }
    if (ver[7] == '1') {
        req->minor_version = 1;
        req->keep_alive    = true;
    } else if (ver[7] == '0') {
        req->minor_version = 0;
        req->keep_alive    = false; /* en 1.0 hay que pedirlo explícitamente */
    } else {
        return fail(req, 505);
    }

    /* --- cabeceras --- */
    const char *line = eol + 2;
    while (line < end) {
        const char *le = find_crlf(line, end);
        if (le == NULL) {
            return fail(req, 400);
        }
        if (le == line) {
            break; /* línea vacía: fin de cabeceras */
        }

        const char *name, *value;
        size_t      namelen, valuelen;
        if (!split_header(line, le, &name, &namelen, &value, &valuelen)) {
            return fail(req, 400);
        }

        if (ieq(name, namelen, "host")) {
            if (req->has_host) {
                /* Dos Host distintos son la base del contrabando de
                 * peticiones: se rechaza en lugar de elegir uno. */
                return fail(req, 400);
            }
            if (valuelen == 0 || valuelen >= HP_MAX_HOST_LEN) {
                return fail(req, 400);
            }
            memcpy(req->host, value, valuelen);
            req->host[valuelen] = '\0';
            req->has_host       = true;
        } else if (ieq(name, namelen, "connection")) {
            if (ieq(value, valuelen, "close")) {
                req->keep_alive = false;
            } else if (ieq(value, valuelen, "keep-alive")) {
                req->keep_alive = true;
            }
        } else if (ieq(name, namelen, "x-forwarded-for")) {
            req->has_xff = true;
        } else if (ieq(name, namelen, "content-length")) {
            if (!parse_u64(value, valuelen, &req->content_length)) {
                return fail(req, 400);
            }
            if (req->body_mode != HP_BODY_CHUNKED) {
                req->body_mode = HP_BODY_LENGTH;
            }
        } else if (ieq(name, namelen, "transfer-encoding")) {
            /* chunked manda sobre Content-Length; si vienen los dos, quien
             * los interprete distinto ve peticiones distintas. */
            if (ieq(value, valuelen, "chunked")) {
                req->body_mode = HP_BODY_CHUNKED;
            } else {
                return fail(req, 400);
            }
        }

        line = le + 2;
    }

    /* Sin Host no hay a dónde enrutar: es 400, no 502. */
    if (!req->has_host) {
        return fail(req, 400);
    }

    req->status = HP_DONE;
    return HP_DONE;
}

/* --- respuesta ------------------------------------------------------------ */

void hp_response_init(http_response *res)
{
    memset(res, 0, sizeof *res);
    res->status        = HP_NEED_MORE;
    res->minor_version = 1;
    res->keep_alive    = true;
    res->body_mode     = HP_BODY_UNTIL_CLOSE;
}

hp_status hp_response_execute(http_response *res, const char *buf, size_t len,
                              bool head_request)
{
    if (res->status != HP_NEED_MORE) {
        return res->status;
    }

    const char *header_end = find_header_end(buf, len);
    if (header_end == NULL) {
        if (len > HP_MAX_HEADER_BYTES) {
            res->status     = HP_ERROR;
            res->error_code = 502; /* el upstream manda cabeceras sin fin */
            return HP_ERROR;
        }
        return HP_NEED_MORE;
    }

    res->header_len = (size_t)(header_end - buf);
    const char *end = buf + res->header_len;

    /* HTTP/1.x SP código SP razón */
    const char *eol = find_crlf(buf, end);
    if (eol == NULL || (size_t)(eol - buf) < 12 || memcmp(buf, "HTTP/1.", 7) != 0) {
        res->status     = HP_ERROR;
        res->error_code = 502;
        return HP_ERROR;
    }

    res->minor_version = buf[7] == '0' ? 0 : 1;
    res->keep_alive    = res->minor_version == 1;
    res->status_code   = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
    if (res->status_code < 100 || res->status_code > 599) {
        res->status     = HP_ERROR;
        res->error_code = 502;
        return HP_ERROR;
    }

    bool have_length  = false;
    bool have_chunked = false;

    const char *line = eol + 2;
    while (line < end) {
        const char *le = find_crlf(line, end);
        if (le == NULL) {
            break;
        }
        if (le == line) {
            break;
        }

        const char *name, *value;
        size_t      namelen, valuelen;
        if (split_header(line, le, &name, &namelen, &value, &valuelen)) {
            if (ieq(name, namelen, "content-length")) {
                have_length = parse_u64(value, valuelen, &res->content_length);
            } else if (ieq(name, namelen, "transfer-encoding")) {
                have_chunked = ieq(value, valuelen, "chunked");
            } else if (ieq(name, namelen, "connection")) {
                if (ieq(value, valuelen, "close")) {
                    res->keep_alive = false;
                } else if (ieq(value, valuelen, "keep-alive")) {
                    res->keep_alive = true;
                }
            }
        }
        line = le + 2;
    }

    /* Estas respuestas no llevan cuerpo aunque declaren longitud, y darlo por
     * bueno desincronizaría la conexión. */
    if (head_request || res->status_code == 204 || res->status_code == 304 ||
        (res->status_code >= 100 && res->status_code < 200)) {
        res->body_mode = HP_BODY_NONE;
    } else if (have_chunked) {
        res->body_mode = HP_BODY_CHUNKED;
    } else if (have_length) {
        res->body_mode = HP_BODY_LENGTH;
    } else {
        /* Sin delimitador el final es el cierre, así que no hay reutilización
         * posible por mucho que diga Connection. */
        res->body_mode  = HP_BODY_UNTIL_CLOSE;
        res->keep_alive = false;
    }

    res->status = HP_DONE;
    return HP_DONE;
}

/* --- cuerpo chunked ------------------------------------------------------- */

/* CH_SIZE_EXT existe porque una extensión de chunk ("1a;foo=bar") puede traer
 * dígitos hexadecimales que NO son parte del tamaño: seguir acumulándolos
 * daría un chunk gigante y desincronizaría el cuerpo entero. */
enum { CH_SIZE = 0, CH_SIZE_EXT, CH_DATA, CH_DATA_CRLF, CH_TRAILER, CH_END };

#define CH_MAX_LINE 64

void hp_chunked_init(hp_chunked *ch)
{
    memset(ch, 0, sizeof *ch);
    ch->state = CH_SIZE;
}

size_t hp_chunked_feed(hp_chunked *ch, const char *buf, size_t len)
{
    size_t i = 0;

    while (i < len && !ch->done && !ch->error) {
        switch (ch->state) {
        case CH_SIZE:
        case CH_SIZE_EXT: {
            char c = buf[i++];
            if (c == '\n') {
                ch->line_len = 0;
                /* Tamaño 0 marca el último chunk: lo que sigue es el tráiler. */
                ch->state = (ch->remaining == 0) ? CH_TRAILER : CH_DATA;
                break;
            }
            if (c == '\r') {
                break;
            }
            if (++ch->line_len > CH_MAX_LINE) {
                ch->error = true;
                break;
            }
            if (ch->state == CH_SIZE_EXT) {
                break; /* dentro de la extensión: nada que acumular */
            }
            if (c == ';') {
                ch->state = CH_SIZE_EXT;
                break;
            }

            int d;
            if (c >= '0' && c <= '9') {
                d = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                d = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                d = c - 'A' + 10;
            } else {
                ch->error = true; /* basura donde debería ir el tamaño */
                break;
            }
            if (ch->remaining > (UINT64_MAX - (uint64_t)d) / 16) {
                ch->error = true;
                break;
            }
            ch->remaining = ch->remaining * 16 + (uint64_t)d;
            break;
        }

        case CH_DATA: {
            uint64_t avail = (uint64_t)(len - i);
            uint64_t take  = avail < ch->remaining ? avail : ch->remaining;
            i += (size_t)take;
            ch->remaining -= take;
            if (ch->remaining == 0) {
                ch->state = CH_DATA_CRLF;
            }
            break;
        }

        case CH_DATA_CRLF:
            if (buf[i] == '\n') {
                ch->state    = CH_SIZE;
                ch->line_len = 0;
            }
            i++;
            break;

        case CH_TRAILER:
            /* Líneas de tráiler hasta una vacía. line_len cuenta los bytes de
             * la línea actual: si un '\n' llega con la línea vacía, se acabó. */
            if (buf[i] == '\n') {
                if (ch->line_len == 0) {
                    ch->state = CH_END;
                    ch->done  = true;
                } else {
                    ch->line_len = 0;
                }
            } else if (buf[i] != '\r') {
                ch->line_len++;
            }
            i++;
            break;

        default:
            ch->done = true;
            break;
        }
    }

    return i;
}

/* --- reescritura de la petición ------------------------------------------- */

ssize_t hp_rewrite(const http_request *req, const char *buf, size_t len,
                   const char *client_ip, const char *proto,
                   bool upstream_keep_alive, char *out, size_t outlen)
{
    if (req->status != HP_DONE || len < req->header_len || client_ip == NULL) {
        return -1;
    }

    const char *end  = buf + req->header_len;
    size_t      used = 0;

#define PUT(ptr, n)                          \
    do {                                     \
        if (used + (n) > outlen) {           \
            return -1;                       \
        }                                    \
        memcpy(out + used, (ptr), (n));      \
        used += (n);                         \
    } while (0)

    /* línea de petición tal cual */
    const char *eol = find_crlf(buf, end);
    if (eol == NULL) {
        return -1;
    }
    PUT(buf, (size_t)(eol - buf) + 2);

    /* Valor previo de X-Forwarded-For, para encadenar en vez de pisar. */
    const char *xff_val    = NULL;
    size_t      xff_vallen = 0;

    const char *line = eol + 2;
    while (line < end) {
        const char *le = find_crlf(line, end);
        if (le == NULL) {
            return -1;
        }
        if (le == line) {
            break;
        }

        const char *name, *value;
        size_t      namelen, valuelen;
        if (!split_header(line, le, &name, &namelen, &value, &valuelen)) {
            return -1;
        }

        /* Connection y Proxy-Connection describen el enlace cliente-proxy, no
         * el proxy-upstream: reenviarlas haría que el upstream cerrara cuando
         * el que quiere cerrar es el cliente, o al revés. */
        bool ours = ieq(name, namelen, "x-forwarded-for") ||
                    ieq(name, namelen, "x-forwarded-proto") ||
                    ieq(name, namelen, "x-real-ip") ||
                    ieq(name, namelen, "connection") ||
                    ieq(name, namelen, "proxy-connection") ||
                    ieq(name, namelen, "keep-alive");

        if (ieq(name, namelen, "x-forwarded-for")) {
            xff_val    = value;
            xff_vallen = valuelen;
        }

        if (!ours) {
            PUT(line, (size_t)(le - line) + 2);
        }

        line = le + 2;
    }

    /* X-Forwarded-For: <cadena previa>, <cliente> */
    static const char xff_name[] = "X-Forwarded-For: ";
    PUT(xff_name, sizeof xff_name - 1);
    if (xff_val != NULL && xff_vallen > 0) {
        PUT(xff_val, xff_vallen);
        PUT(", ", 2);
    }
    PUT(client_ip, strlen(client_ip));
    PUT("\r\n", 2);

    static const char proto_name[] = "X-Forwarded-Proto: ";
    PUT(proto_name, sizeof proto_name - 1);
    PUT(proto != NULL ? proto : "http", strlen(proto != NULL ? proto : "http"));
    PUT("\r\n", 2);

    static const char real_name[] = "X-Real-IP: ";
    PUT(real_name, sizeof real_name - 1);
    PUT(client_ip, strlen(client_ip));
    PUT("\r\n", 2);

    if (upstream_keep_alive) {
        static const char keep[] = "Connection: keep-alive\r\n";
        PUT(keep, sizeof keep - 1);
    } else {
        static const char close_h[] = "Connection: close\r\n";
        PUT(close_h, sizeof close_h - 1);
    }

    PUT("\r\n", 2); /* fin de cabeceras */

#undef PUT

    return (ssize_t)used;
}
