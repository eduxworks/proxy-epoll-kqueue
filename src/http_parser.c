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

ssize_t hp_rewrite(const http_request *req, const char *buf, size_t len,
                   const char *client_ip, const char *proto, char *out,
                   size_t outlen)
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

        bool ours = ieq(name, namelen, "x-forwarded-for") ||
                    ieq(name, namelen, "x-forwarded-proto") ||
                    ieq(name, namelen, "x-real-ip");

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

    PUT("\r\n", 2); /* fin de cabeceras */

#undef PUT

    return (ssize_t)used;
}
