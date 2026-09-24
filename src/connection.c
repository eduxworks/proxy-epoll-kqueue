/* connection.c — relay bidireccional no bloqueante con keep-alive.
 *
 * Tres reglas gobiernan el fichero:
 *
 * 1. Edge-triggered obliga a drenar. Cada lectura y cada escritura van en
 *    bucle hasta EAGAIN. Si un handler se deja datos, el kernel no vuelve a
 *    avisar y la conexión se queda colgada sin que nadie lo note.
 *
 * 2. Contrapresión en vez de buffers que crecen. Cuando el buffer de salida de
 *    un sentido está lleno, se deja de leer del otro extremo.
 *
 * 3. Para reutilizar una conexión hay que saber dónde acaba cada mensaje. El
 *    cuerpo se delimita por Content-Length o por chunked; si no hay forma de
 *    delimitarlo, el final es el cierre y no hay reutilización posible.
 *    Contar mal aquí significa servir media respuesta como si fuera la
 *    siguiente.
 *
 * No se admite encadenar peticiones (pipelining): mientras se espera una
 * respuesta no se lee del cliente, así que la siguiente petición se queda en
 * el socket hasta que toca. Es lo que hacen los clientes reales y evita tener
 * que casar respuestas con peticiones fuera de orden.
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
    bool         req_ready;     /* cabecera ya reescrita en out */
    uint64_t     req_body_left; /* cuerpo de la petición por reenviar */

    http_response resp;
    hp_chunked    chunk;
    uint64_t      resp_body_left;
    bool          resp_headers_done;
    bool          resp_complete;

    bool client_eof;
    bool upstream_eof;
    bool keep_client; /* el cliente permite reutilizar su conexión */

    unsigned long served; /* peticiones atendidas en esta conexión */
} conn;

struct conn_manager {
    io_loop     *loop;
    router_slot *slot;
    buffer_pool *bufs;
    conn        *free_list;
    unsigned long accepted;  /* conexiones aceptadas */
    unsigned long requests;  /* peticiones completadas */
    unsigned long errors;    /* respuestas de error generadas por el proxy */
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
    m->accepted++;
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

unsigned long conn_manager_accepted(const conn_manager *m)
{
    return m != NULL ? m->accepted : 0;
}

unsigned long conn_manager_requests(const conn_manager *m)
{
    return m != NULL ? m->requests : 0;
}

unsigned long conn_manager_errors(const conn_manager *m)
{
    return m != NULL ? m->errors : 0;
}

size_t conn_manager_active(const conn_manager *m)
{
    return m != NULL ? m->active : 0;
}

/* --- cierre --------------------------------------------------------------- */

static void drop_upstream(conn *c)
{
    if (c->upstream_fd >= 0) {
        io_loop_del(c->mgr->loop, c->upstream_fd);
        close(c->upstream_fd);
        c->upstream_fd = -1;
    }
    if (c->be != NULL) {
        backend_conn_closed(c->be);
        c->be = NULL;
    }
}

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
    drop_upstream(c);

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

    if (c->rt != NULL) {
        router_unref(c->rt);
        c->rt = NULL;
    }

    conn_release(c);
}

/* --- respuestas de error -------------------------------------------------- */

/* Escritura directa y de una vez: son pocos bytes y la conexión se cierra a
 * continuación, así que no merece pasar por la maquinaria del relay. */
static void send_error(conn *c, int code)
{
    const char *line;
    switch (code) {
    case 400: line = "HTTP/1.1 400 Bad Request\r\n";                     break;
    case 431: line = "HTTP/1.1 431 Request Header Fields Too Large\r\n"; break;
    case 503: line = "HTTP/1.1 503 Service Unavailable\r\n";             break;
    case 505: line = "HTTP/1.1 505 HTTP Version Not Supported\r\n";      break;
    default:  line = "HTTP/1.1 502 Bad Gateway\r\n";                     break;
    }

    char   msg[256];
    size_t n = (size_t)snprintf(msg, sizeof msg,
                                "%sContent-Length: 0\r\nConnection: close\r\n\r\n",
                                line);

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

    c->mgr->errors++;
    conn_close(c);
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

    /* Del cliente solo se lee mientras quede cuerpo de petición por reenviar.
     * Terminada la petición se deja de leer: así la siguiente se queda en el
     * socket y no se cuela en medio de esta. */
    if (!c->client_eof && c->req_body_left > 0 && c->out_len < SLOT) {
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
    if (c->upstream_fd >= 0) {
        io_loop_mod(c->mgr->loop, c->upstream_fd, um);
    }
}

/* --- fin de respuesta y reutilización -------------------------------------- */

static void start_next_request(conn *c)
{
    /* La conexión de salida solo se conserva si la respuesta venía bien
     * delimitada y el upstream no pidió cerrar; si no, se suelta y la
     * siguiente petición abrirá otra. */
    bool reuse_upstream = c->resp.keep_alive && !c->upstream_eof &&
                          c->resp.body_mode != HP_BODY_UNTIL_CLOSE;
    if (!reuse_upstream) {
        drop_upstream(c);
    }

    hp_init(&c->req);
    hp_response_init(&c->resp);
    c->req_ready         = false;
    c->req_body_left     = 0;
    c->resp_headers_done = false;
    c->resp_complete     = false;
    c->resp_body_left    = 0;
    c->attempts          = 0;
    c->client_eof        = false;
    c->upstream_eof      = false;
    c->in_len = c->in_off = 0;
    c->out_len = c->out_off = 0;
    c->served++;
    c->mgr->requests++;

    c->state = CONN_READING_REQUEST;
    io_loop_mod(c->mgr->loop, c->client_fd, IO_READ);
    if (c->upstream_fd >= 0) {
        /* En reposo se sigue observando: si el upstream cierra su lado hay que
         * enterarse ahora y no al escribirle la siguiente petición. */
        io_loop_mod(c->mgr->loop, c->upstream_fd, IO_READ);
    }
}

static void finish_response(conn *c)
{
    if (!c->keep_client || !c->resp.keep_alive) {
        conn_close(c);
        return;
    }
    start_next_request(c);
}

/* Contabiliza los bytes de cuerpo recién llegados. Devuelve false si el
 * troceado venía mal formado, que solo puede acabar en cierre. */
static bool feed_response_body(conn *c, size_t from)
{
    size_t n = c->in_len - from;

    switch (c->resp.body_mode) {
    case HP_BODY_NONE:
        c->resp_complete = true;
        break;

    case HP_BODY_LENGTH:
        if (n >= c->resp_body_left) {
            c->resp_body_left = 0;
            c->resp_complete  = true;
        } else {
            c->resp_body_left -= n;
        }
        break;

    case HP_BODY_CHUNKED:
        hp_chunked_feed(&c->chunk, (const char *)c->in + from, n);
        if (c->chunk.error) {
            return false;
        }
        if (c->chunk.done) {
            c->resp_complete = true;
        }
        break;

    case HP_BODY_UNTIL_CLOSE:
    default:
        break; /* acaba cuando cierre el upstream */
    }

    return true;
}

static void proxy_step(conn *c)
{
    if (c->upstream_fd >= 0 &&
        !drain_write(c->upstream_fd, c->out, &c->out_off, &c->out_len)) {
        conn_close(c);
        return;
    }
    if (!drain_write(c->client_fd, c->in, &c->in_off, &c->in_len)) {
        conn_close(c);
        return;
    }

    /* Sin delimitador, el cierre del upstream ES el final del mensaje. */
    if (c->upstream_eof && c->resp_headers_done &&
        c->resp.body_mode == HP_BODY_UNTIL_CLOSE) {
        c->resp_complete = true;
    }

    /* El cliente terminó de mandar y ya le hemos pasado todo al upstream. */
    if (c->client_eof && c->out_off == c->out_len && c->upstream_fd >= 0) {
        shutdown(c->upstream_fd, SHUT_WR);
    }

    if (c->resp_complete && c->in_off == c->in_len) {
        finish_response(c);
        return;
    }

    /* El upstream se fue sin completar la respuesta: no hay nada que salvar. */
    if (c->upstream_eof && !c->resp_complete && c->in_off == c->in_len) {
        conn_close(c);
        return;
    }

    update_masks(c);
}

/* --- eventos del cliente --------------------------------------------------- */

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
            conn_close(c); /* se fue sin pedir nada, o cerró estando en reposo */
            return;
        }

        hp_status st = hp_execute(&c->req, (const char *)c->out, c->out_len);
        if (st == HP_ERROR) {
            send_error(c, c->req.error_code);
            return;
        }
        if (st == HP_NEED_MORE) {
            if (eof) {
                conn_close(c); /* cortó a mitad de cabecera */
            } else if (c->out_len >= SLOT) {
                send_error(c, 431);
            }
            return;
        }

        c->client_eof = eof;
        route_and_connect(c);
        return;
    }

    if (c->state == CONN_PROXYING) {
        size_t before = c->out_len;
        if (drain_read(fd, c->out, &c->out_len, SLOT, &eof) < 0) {
            conn_close(c);
            return;
        }

        size_t got = c->out_len - before;
        if (c->req_body_left != UINT64_MAX) {
            c->req_body_left = got >= c->req_body_left ? 0 : c->req_body_left - got;
        }
        if (eof) {
            c->client_eof = true;
        }
        proxy_step(c);
    }
}

/* --- eventos del upstream -------------------------------------------------- */

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
            /* Salud pasiva: se apunta el fallo y se prueba con otro. */
            pool_report(c->pool, c->be, false);
            drop_upstream(c);
            route_and_connect(c);
            return;
        }

        pool_report(c->pool, c->be, true);
        backend_conn_opened(c->be);

        c->state = CONN_PROXYING;
        io_loop_mod(c->mgr->loop, c->upstream_fd, IO_READ | IO_WRITE);
        proxy_step(c);
        return;
    }

    /* Entre peticiones lo único que puede llegar de un upstream en reposo es
     * su cierre. Se suelta y la siguiente petición abrirá otra conexión. */
    if (c->state == CONN_READING_REQUEST) {
        drop_upstream(c);
        return;
    }

    if (c->state != CONN_PROXYING) {
        return;
    }

    if (events & IO_ERR) {
        conn_close(c);
        return;
    }

    bool   eof    = (events & IO_EOF) != 0;
    size_t before = c->in_len;

    if (drain_read(fd, c->in, &c->in_len, SLOT, &eof) < 0) {
        conn_close(c);
        return;
    }

    size_t frame_from;
    if (!c->resp_headers_done) {
        hp_status st = hp_response_execute(&c->resp, (const char *)c->in,
                                           c->in_len, c->req.is_head);
        if (st == HP_ERROR) {
            pool_report(c->pool, c->be, false);
            send_error(c, 502);
            return;
        }
        if (st == HP_NEED_MORE) {
            /* Nada se reenvía hasta tener la cabecera entera: hasta entonces
             * no se sabe cómo delimitar el cuerpo. */
            if (eof || c->in_len >= SLOT) {
                conn_close(c);
            }
            return;
        }

        c->resp_headers_done = true;
        c->resp_body_left    = c->resp.content_length;
        if (c->resp.body_mode == HP_BODY_CHUNKED) {
            hp_chunked_init(&c->chunk);
        }
        frame_from = c->resp.header_len;
    } else {
        frame_from = before;
    }

    if (!feed_response_body(c, frame_from)) {
        conn_close(c); /* troceado mal formado */
        return;
    }

    if (eof) {
        c->upstream_eof = true;
    }
    proxy_step(c);
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

    /* Sin Nagle: el proxy ya escribe en bloques grandes y esperar a llenar
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

/* Reescribe la petición en el buffer de salida y calcula cuánto cuerpo queda. */
static bool prepare_request(conn *c)
{
    unsigned char *rewritten = bufpool_get(c->mgr->bufs);
    if (rewritten == NULL) {
        send_error(c, 503);
        return false;
    }

    ssize_t hn = hp_rewrite(&c->req, (const char *)c->out, c->out_len,
                            c->client_ip, "http", true, (char *)rewritten, SLOT);
    size_t body = c->out_len - c->req.header_len;
    if (hn < 0 || (size_t)hn + body > SLOT) {
        bufpool_put(c->mgr->bufs, rewritten);
        send_error(c, 431);
        return false;
    }
    if (body > 0) {
        memcpy(rewritten + hn, c->out + c->req.header_len, body);
    }

    bufpool_put(c->mgr->bufs, c->out);
    c->out     = rewritten;
    c->out_len = (size_t)hn + body;
    c->out_off = 0;

    switch (c->req.body_mode) {
    case HP_BODY_LENGTH:
        c->req_body_left =
            c->req.content_length > body ? c->req.content_length - body : 0;
        break;
    case HP_BODY_CHUNKED:
        /* Delimitar un cuerpo troceado del cliente exige su propio contador;
         * mientras no exista se reenvía hasta que el cliente cierre, y esa
         * conexión no se reutiliza. */
        c->req_body_left = UINT64_MAX;
        c->keep_client   = false;
        break;
    default:
        c->req_body_left = 0;
        break;
    }

    c->req_ready = true;
    return true;
}

static void route_and_connect(conn *c)
{
    backend_pool *pool = router_lookup(c->rt, c->frontend_index, c->req.host);
    if (pool == NULL) {
        send_error(c, 502); /* ni exacta, ni wildcard, ni default */
        return;
    }

    /* Una conexión de salida reutilizada solo vale si va al mismo pool: si el
     * Host de esta petición enruta a otro sitio, hay que abrir otra. */
    if (c->upstream_fd >= 0 && c->pool != pool) {
        drop_upstream(c);
    }
    c->pool = pool;

    if (!c->req_ready) {
        c->keep_client = c->req.keep_alive;
        if (!prepare_request(c)) {
            return;
        }
    }

    if (c->in == NULL) {
        c->in = bufpool_get(c->mgr->bufs);
        if (c->in == NULL) {
            send_error(c, 503);
            return;
        }
    }

    if (c->upstream_fd >= 0) {
        /* Ya hay conexión viva con este pool: se aprovecha y se ahorra el
         * handshake, que es de donde sale la mayor parte del rendimiento. */
        c->state = CONN_PROXYING;
        io_loop_mod(c->mgr->loop, c->upstream_fd, IO_READ | IO_WRITE);
        proxy_step(c);
        return;
    }

    /* Mientras se conecta no se lee del cliente: no habría dónde poner lo
     * leído sin pisar la petición pendiente de enviar. */
    io_loop_mod(c->mgr->loop, c->client_fd, 0);

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
    c->keep_client    = true;
    hp_init(&c->req);
    hp_response_init(&c->resp);

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
        send_error(c, 503); /* sin buffers, decirlo es mejor que colgar */
        return;
    }

    if (io_loop_add(m->loop, client_fd, IO_READ, on_client_event, c) < 0) {
        conn_close(c);
    }
}
