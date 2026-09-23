/* echo_server.c — backend de pruebas para el e2e (R9).
 *
 * Responde 200 con un cuerpo que dice quién es y qué cabeceras le llegaron.
 * Esa identidad es lo que permite comprobar desde fuera el reparto round-robin
 * y la inyección de X-Forwarded-For, sin instrumentar el proxy.
 *
 * Bloqueante y con un hijo por conexión a propósito: aquí no se mide
 * rendimiento, se comprueba comportamiento, y un servidor simple es un
 * servidor que no introduce dudas sobre de quién es el fallo. El fork hace
 * falta porque con keep-alive las conexiones duran, y un servidor secuencial
 * dejaría a los demás workers del proxy esperando en la cola de accept.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void on_term(int signo)
{
    (void)signo;
    running = 0;
}

static const char *find_header(const char *req, const char *name, size_t *len)
{
    size_t nlen = strlen(name);
    for (const char *p = req; (p = strchr(p, '\n')) != NULL; p++) {
        const char *line = p + 1;
        if (strncasecmp(line, name, nlen) == 0 && line[nlen] == ':') {
            const char *v = line + nlen + 1;
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            const char *e = strpbrk(v, "\r\n");
            *len          = e != NULL ? (size_t)(e - v) : strlen(v);
            return v;
        }
    }
    *len = 0;
    return NULL;
}

/* Devuelve false cuando el cliente cerró: así el llamador sabe que la conexión
 * terminó y no hay más peticiones que atender por ella. */
static bool serve_one(int fd, const char *id)
{
    char   req[8192];
    size_t len = 0;

    while (len < sizeof req - 1) {
        ssize_t n = read(fd, req + len, sizeof req - 1 - len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        len += (size_t)n;
        req[len] = '\0';
        if (strstr(req, "\r\n\r\n") != NULL) {
            break;
        }
    }
    req[len] = '\0';

    size_t      xff_len = 0, host_len = 0;
    const char *xff  = find_header(req, "X-Forwarded-For", &xff_len);
    const char *host = find_header(req, "Host", &host_len);

    char body[1024];
    int  blen = snprintf(body, sizeof body,
                         "%s\nhost=%.*s\nxff=%.*s\n", id, (int)host_len,
                         host != NULL ? host : "", (int)xff_len,
                         xff != NULL ? xff : "");

    /* Content-Length y keep-alive: sin un delimitador el proxy no puede
     * reutilizar la conexión, y entonces el e2e no probaría nada de eso. */
    char resp[2048];
    int  rlen = snprintf(resp, sizeof resp,
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/plain\r\n"
                         "Content-Length: %d\r\n"
                         "X-Backend: %s\r\n"
                         "Connection: keep-alive\r\n"
                         "\r\n"
                         "%s",
                         blen, id, body);

    ssize_t off = 0;
    while (off < rlen) {
        ssize_t w = write(fd, resp + off, (size_t)(rlen - off));
        if (w <= 0) {
            if (w < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        off += w;
    }

    return true;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <puerto> [id]\n", argv[0]);
        return EXIT_FAILURE;
    }

    int  port = atoi(argv[1]);
    char id[64];
    snprintf(id, sizeof id, "%s", argc > 2 ? argv[2] : argv[1]);

    /* sigaction con sa_flags = 0, no signal(). En Linux signal() instala el
     * handler con SA_RESTART, así que accept() se reanudaría sola tras la
     * señal: el proceso seguiría sirviendo y "matar el backend" en el e2e no
     * mataría nada. Sin SA_RESTART, accept() devuelve EINTR y el bucle sale. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return EXIT_FAILURE;
    }
    if (listen(fd, 128) < 0) {
        perror("listen");
        close(fd);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "echo_server %s escuchando en 127.0.0.1:%d\n", id, port);

    /* Un hijo por conexión. Con keep-alive una conexión dura mientras el
     * cliente quiera, así que un servidor secuencial dejaría a los demás
     * workers del proxy esperando en la cola de accept sin ser atendidos. */
    signal(SIGCHLD, SIG_IGN); /* sin zombis que recoger */

    /* Grupo de procesos propio, para poder matar a los hijos al salir sin
     * tocar al resto del script. Sin esto, "matar el backend" dejaría vivos a
     * los hijos que sostienen conexiones keep-alive y el proxy seguiría siendo
     * atendido por un backend que se supone caído. */
    setpgid(0, 0);

    while (running) {
        int c = accept(fd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        pid_t pid = fork();
        if (pid == 0) {
            close(fd);
            while (serve_one(c, id)) {
                /* siguiente petición en la misma conexión */
            }
            close(c);
            _exit(0);
        }

        close(c);
        if (pid < 0) {
            perror("fork");
        }
    }

    close(fd);

    /* kill(0, ...) va al grupo propio: este proceso y sus hijos, nadie más. */
    signal(SIGTERM, SIG_IGN);
    kill(0, SIGTERM);

    return EXIT_SUCCESS;
}
