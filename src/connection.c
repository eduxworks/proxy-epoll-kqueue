/* connection.c — relay bidireccional no bloqueante.
 *
 * Dos reglas gobiernan todo el fichero:
 *
 * 1. Edge-triggered obliga a drenar. Cada lectura y cada escritura van en
 *    bucle hasta EAGAIN. Si un handler se deja datos, el kernel no vuelve a
 *    avisar y la conexión se queda colgada sin que nadie lo note.
 *
 * 2. Contrapresión en vez de buffers que crecen. Cuando el buffer de salida de
 *    un sentido está lleno, se deja de leer del otro extremo. Así el proxy no
 *    acumula memoria por cliente cuando el upstream va más lento que el
 *    cliente, o al revés.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backend_pool.h"
#include "connection.h"
#include "http_parser.h"

#define MAX_CONNECT_ATTEMPTS 3
#define SLOT                 BUFPOOL_SLOT_SIZE

typedef enum {
    CONN_READING_REQUEST,
    CONN_CONNECTING,
    CONN_PROXYING,
    CONN_CLOSING
} conn_state;

typedef struct conn {
    conn_manager *mgr;
    struct conn  *next_free; /* arena: lista libre de estructuras */

    int        client_fd;
    int        upstream_fd;
    conn_state state;

    router       *rt; /* referencia propia, soltada al cerrar */
    backend_pool *pool;
    backend      *be;
    int           attempts;

    size_t frontend_index;
    char   client_ip[INET6_ADDRSTRLEN];

    /* cliente -> upstream */
    unsigned char *out;
    size_t         out_len;
    size_t         out_off;

    /* upstream -> cliente */
    unsigned char *in;
    size_t         in_len;
    size_t         in_off;

    http_request req;
    bool         client_eof;
    bool         upstream_eof;
} conn;

struct conn_manager {
    io_loop     *loop;
    router_slot *slot;
    buffer_pool *bufs;
    conn        *free_list; /* estructuras reutilizables */
    size_t       active;
};

static void conn_close(conn *c);
static void update_masks(conn *c);
static void route_and_connect(conn *c);
static void on_upstream_event(int fd, unsigned events, void *ctx);

/* --- arena de conexiones -------------------------------------------------- */

static conn *conn_acquire(conn_manager *m)
{
    conn *c = m->free_list;
    if (c != NULL) {
        m->free_list = c->next_free;
    } else {
        c = malloc(sizeof *c);
        if (c == NULL) {
            return NULL;
        }
    }

    memset(c, 0, sizeof *c);
    c->mgr         = m;
    c->client_fd   = -1;
    c->upstream_fd = -1;
    m->active++;
    return c;
}

static void conn_release(conn *c)
{
    conn_manager *m = c->mgr;
    m->active--;
    c->next_free = m->free_list;
    m->free_list = c;
}

conn_manager *conn_manager_create(io_loop *loop, router_slot *slot,
                                  buffer_pool *bufs)
{
    if (loop == NULL || slot == NULL || bufs == NULL) {
        return NULL;
    }
    conn_manager *m = calloc(1, sizeof *m);
    if (m == NULL) {
        return NULL;
    }
    m->loop = loop;
    m->slot = slot;
    m->bufs = bufs;
    return m;
}

void conn_manager_destroy(conn_manager *m)
{
    if (m == NULL) {
        return;
    }
    conn *c = m->free_list;
    while (c != NULL) {
        conn *next = c->next_free;
        free(c);
        c = next;
    }
    free(m);
}

size_t conn_manager_active(const conn_manager *m)
{
    return m != NULL ? m->active : 0;
}

/* --- respuestas de error -------------------------------------------------- */

/* Escritura directa y de una vez: son pocos bytes y la conexión se cierra a
 * continuación, así que no merece pasar por la maquinaria del relay. */
static void send_error(conn *c, int code)
{
    const char *body;
    switch (code) {
    case 400: body = "HTTP/1.1 400 Bad Request\r\n";           break;
    case 431: body = "HTTP/1.1 431 Request Header Fields Too Large\r\n"; break;
    case 503: body = "HTTP/1.1 503 Service Unavailable\r\n";   break;
    case 505: body = "HTTP/1.1 505 HTTP Version Not Supported\r\n"; break;
    default:  body = "HTTP/1.1 502 Bad Gateway\r\n";           break;
    }

    char   msg[256];
    size_t n = (size_t)snprintf(msg, sizeof msg,
                                "%sContent-Length: 0\r\nConnection: close\r\n\r\n",
                                body);

    size_t off = 0;
    while (off < n) {
        ssize_t w = write(c->client_fd, msg + off, n - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) {
            continue;
        }
        break; /* si el cliente ya no escucha, tampoco pasa nada */
    }

    conn_close(c);
}

/* --- cierre --------------------------------------------------------------- */

static void conn_close(conn *c)
{
    if (c->state == CONN_CLOSING) {
        return;
    }
    c->state = CONN_CLOSING;

    conn_manager *m = c->mgr;

    if (c->client_fd >= 0) {
        io_loop_del(m->loop, c->client_fd);
        close(c->client_fd);
        c->client_fd = -1;
    }
    if (c->upstream_fd >= 0) {
        io_loop_del(m->loop, c->upstream_fd);
        close(c->upstream_fd);
        c->upstream_fd = -1;
    }

    /* Todo lo prestado vuelve, también por el camino de error: un slot que no
     * se devuelve es una fuga que solo se nota bajo carga. */
    if (c->out != NULL) {
        bufpool_put(m->bufs, c->out);
        c->out = NULL;
    }
    if (c->in != NULL) {
        bufpool_put(m->bufs, c->in);
        c->in = NULL;
    }

    if (c->be != NULL) {
        backend_conn_closed(c->be);
        c->be = NULL;
    }
    if (c->rt != NULL) {
        router_unref(c->rt);
        c->rt = NULL;
    }

    conn_release(c);
}

/* --- E/S ------------------------------------------------------------------ */

/* Lee hasta EAGAIN o hasta llenar. Devuelve los bytes leídos, o -1 si el
 * socket falló. El EOF se informa aparte: si viniera como valor de retorno se
 * perdería cuando llega en la misma lectura que los últimos datos, y con
 * edge-triggered esa notificación no se repite. */
static int drain_read(int fd, unsigned char *buf, size_t *len, size_t cap,
                      bool *eof)
{
    int total = 0;
    while (*len < cap) {
        ssize_t n = read(fd, buf + *len, cap - *len);
        if (n > 0) {
            *len += (size_t)n;
            total += (int)n;
            continue;
        }
        if (n == 0) {
            *eof = true;
            return total;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return total;
        }
        return -1;
    }
    return total; /* buffer lleno: contrapresión */
}

/* Devuelve false si el socket murió. */
static bool drain_write(int fd, unsigned char *buf, size_t *off, size_t *len)
{
    while (*off < *len) {
        ssize_t n = write(fd, buf + *off, *len - *off);
        if (n > 0) {
            *off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true; /* se reintenta cuando avise IO_WRITE */
        }
        return false;
    }

    *off = 0; /* vaciado: se compacta para volver a llenar desde cero */
    *len = 0;
    return true;
}

static void update_masks(conn *c)
{
    if (c->state != CONN_PROXYING) {
        return;
    }

    unsigned cm = 0;
    unsigned um = 0;

    /* Solo se lee de un extremo si hay sitio donde poner lo leído. */
    if (!c->client_eof && c->out_len < SLOT) {
        cm |= IO_READ;
    }
    if (c->in_off < c->in_len) {
        cm |= IO_WRITE;
    }
    if (!c->upstream_eof && c->in_len < SLOT) {
        um |= IO_READ;
    }
    if (c->out_off < c->out_len) {
        um |= IO_WRITE;
    }

    io_loop_mod(c->mgr->loop, c->client_fd, cm);
    io_loop_mod(c->mgr->loop, c->upstream_fd, um);
}

/* --- fase de proxy -------------------------------------------------------- */

static void proxy_step(conn *c)
{
    if (!drain_write(c->upstream_fd, c->out, &c->out_off, &c->out_len) ||
        !drain_write(c->client_fd, c->in, &c->in_off, &c->in_len)) {
        conn_close(c);
        return;
    }

    /* El cliente terminó y ya le hemos pasado todo: se le avisa al upstream
     * cerrando solo ese sentido, para que pueda responder tranquilo. */
    if (c->client_eof && c->out_off == c->out_len) {
        shutdown(c->upstream_fd, SHUT_WR);
    }

    /* El upstream terminó y el cliente ya tiene su respuesta entera. */
    if (c->upstream_eof && c->in_off == c->in_len) {
        conn_close(c);
        return;
    }

    update_masks(c);
}

static void on_client_event(int fd, unsigned events, void *ctx)
{
    conn *c   = ctx;
    bool  eof = (events & IO_EOF) != 0;

    if (events & IO_ERR) {
        conn_close(c);
        return;
    }

    if (c->state == CONN_READING_REQUEST) {
        if (drain_read(fd, c->out, &c->out_len, SLOT, &eof) < 0) {
            conn_close(c);
            return;
        }
        if (eof && c->out_len == 0) {
            conn_close(c); /* se fue sin pedir nada */
            return;
        }

        hp_status st = hp_execute(&c->req, (const char *)c->out, c->out_len);
        if (st == HP_ERROR) {
            send_error(c, c->req.error_code);
            return;
        }
        if (st == HP_NEED_MORE) {
            /* Cerró a mitad de cabecera, o no cabe y aún no ha terminado. */
            if (eof) {
                conn_close(c);
            } else if (c->out_len >= SLOT) {
                send_error(c, 431);
            }
            return;
        }

        route_and_connect(c);
        return;
    }

    if (c->state == CONN_PROXYING) {
        if (drain_read(fd, c->out, &c->out_len, SLOT, &eof) < 0) {
            conn_close(c);
            return;
        }
        if (eof) {
            c->client_eof = true;
        }
        proxy_step(c);
    }
}

static void on_upstream_event(int fd, unsigned events, void *ctx)
{
    conn *c = ctx;

    if (c->state == CONN_CONNECTING) {
        int       soerr = 0;
        socklen_t slen  = sizeof soerr;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0) {
            soerr = errno;
        }

        if (soerr != 0 || (events & IO_ERR)) {
            /* Salud pasiva: el fallo se apunta y se prueba con otro. */
            pool_report(c->pool, c->be, false);
            c->be = NULL;
            io_loop_del(c->mgr->loop, fd);
            close(fd);
            c->upstream_fd = -1;

            route_and_connect(c);
            return;
        }

        pool_report(c->pool, c->be, true);
        backend_conn_opened(c->be);

        c->in = bufpool_get(c->mgr->bufs);
        if (c->in == NULL) {
            send_error(c, 503);
            return;
        }

        c->state = CONN_PROXYING;
        io_loop_mod(c->mgr->loop, c->client_fd, IO_READ);
        io_loop_mod(c->mgr->loop, c->upstream_fd, IO_READ | IO_WRITE);
        proxy_step(c);
        return;
    }

    if (c->state == CONN_PROXYING) {
        if (events & IO_ERR) {
            conn_close(c);
            return;
        }

        bool eof = (events & IO_EOF) != 0;
        if (drain_read(fd, c->in, &c->in_len, SLOT, &eof) < 0) {
            conn_close(c);
            return;
        }
        if (eof) {
            c->upstream_eof = true;
        }
        proxy_step(c);
    }
}

/* --- enrutado y conexión de salida ---------------------------------------- */

static bool start_connect(conn *c)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    if (io_set_nonblocking(fd) < 0) {
        close(fd);
        return false;
    }

    /* Sin Nagle: el proxy ya escribe en bloques grandes, y esperar a llenar
     * segmentos solo añade latencia. */
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);

    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port   = htons(backend_port(c->be));
    if (inet_pton(AF_INET, backend_host(c->be), &to.sin_addr) != 1) {
        close(fd);
        return false;
    }

    if (connect(fd, (struct sockaddr *)&to, sizeof to) < 0 &&
        errno != EINPROGRESS) {
        close(fd);
        return false;
    }

    c->upstream_fd = fd;
    c->state       = CONN_CONNECTING;

    /* El resultado del connect llega como "se puede escribir". */
    return io_loop_add(c->mgr->loop, fd, IO_WRITE, on_upstream_event, c) == 0;
}

static void route_and_connect(conn *c)
{
    if (c->pool == NULL) {
        c->pool = router_lookup(c->rt, c->frontend_index, c->req.host);
        if (c->pool == NULL) {
            send_error(c, 502); /* sin ruta: ni exacta, ni wildcard, ni default */
            return;
        }

        /* La cabecera reescrita sustituye a la original en el mismo buffer de
         * salida: lo que quede de cuerpo ya leído se conserva detrás. */
        unsigned char *rewritten = bufpool_get(c->mgr->bufs);
        if (rewritten == NULL) {
            send_error(c, 503);
            return;
        }

        ssize_t hn = hp_rewrite(&c->req, (const char *)c->out, c->out_len,
                                c->client_ip, "http", (char *)rewritten, SLOT);
        size_t body = c->out_len - c->req.header_len;
        if (hn < 0 || (size_t)hn + body > SLOT) {
            bufpool_put(c->mgr->bufs, rewritten);
            send_error(c, 431);
            return;
        }
        if (body > 0) {
            memcpy(rewritten + hn, c->out + c->req.header_len, body);
        }

        bufpool_put(c->mgr->bufs, c->out);
        c->out     = rewritten;
        c->out_len = (size_t)hn + body;
        c->out_off = 0;

        /* Mientras se conecta no se lee más del cliente: no habría dónde
         * ponerlo sin pisar la petición pendiente de enviar. */
        io_loop_mod(c->mgr->loop, c->client_fd, 0);
    }

    while (c->attempts < MAX_CONNECT_ATTEMPTS) {
        c->attempts++;
        c->be = pool_pick(c->pool);
        if (c->be == NULL) {
            break; /* no queda nadie en pie */
        }
        if (start_connect(c)) {
            return;
        }
        pool_report(c->pool, c->be, false);
        c->be = NULL;
    }

    send_error(c, 502);
}

/* --- entrada -------------------------------------------------------------- */

void conn_accepted(conn_manager *m, int client_fd,
                   const struct sockaddr_storage *peer, size_t frontend_index)
{
    conn *c = conn_acquire(m);
    if (c == NULL) {
        close(client_fd);
        return;
    }

    c->client_fd      = client_fd;
    c->frontend_index = frontend_index;
    c->state          = CONN_READING_REQUEST;
    hp_init(&c->req);

    if (peer != NULL && peer->ss_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)peer;
        inet_ntop(AF_INET, &sin->sin_addr, c->client_ip, sizeof c->client_ip);
    } else {
        snprintf(c->client_ip, sizeof c->client_ip, "unknown");
    }

    int on = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);

    /* La referencia al router se toma ahora y se suelta al cerrar: es lo que
     * hace que un reload no cambie la ruta bajo los pies de esta conexión. */
    c->rt = router_slot_acquire(m->slot);

    c->out = bufpool_get(m->bufs);
    if (c->out == NULL) {
        /* Sin buffers no se puede servir; decirlo es mejor que colgar. */
        send_error(c, 503);
        return;
    }

    if (io_loop_add(m->loop, client_fd, IO_READ, on_client_event, c) < 0) {
        conn_close(c);
    }
}
