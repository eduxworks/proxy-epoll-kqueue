/* io_event.h — API común del bucle de eventos (E3).
 *
 * Esta cabecera no sabe en qué sistema operativo corre. La implementan
 * io_event_epoll.c (Linux) e io_event_kqueue.c (macOS/FreeBSD); Meson compila
 * una u otra según host_machine.system(). Ningún #ifdef de plataforma debe
 * aparecer fuera de esos dos ficheros.
 *
 * El bucle es SIEMPRE edge-triggered (EPOLLET / EV_CLEAR): el kernel avisa una
 * sola vez por transición, así que todo callback debe leer o escribir en bucle
 * hasta recibir EAGAIN/EWOULDBLOCK. Si un handler deja datos sin drenar, el
 * descriptor no vuelve a despertar y la conexión queda colgada.
 */
#ifndef PROXY_IO_EVENT_H
#define PROXY_IO_EVENT_H

/* Máscara de eventos. IO_READ e IO_WRITE son de entrada y salida;
 * IO_EOF e IO_ERR solo los reporta el bucle. */
enum {
    IO_READ  = 1u << 0, /* hay datos que leer, o el accept está listo */
    IO_WRITE = 1u << 1, /* se puede escribir sin bloquear */
    IO_EOF   = 1u << 2, /* el par cerró su extremo de escritura */
    IO_ERR   = 1u << 3  /* error en el descriptor */
};

typedef struct io_loop io_loop;

/* Se invoca desde io_loop_run con los eventos ocurridos. No debe bloquear. */
typedef void (*io_cb)(int fd, unsigned events, void *ctx);

io_loop *io_loop_create(void);
void     io_loop_destroy(io_loop *loop);

/* Registra fd con la máscara dada. Devuelve 0 o -1 (errno). */
int io_loop_add(io_loop *loop, int fd, unsigned mask, io_cb cb, void *ctx);

/* Cambia la máscara de un fd ya registrado. Devuelve 0 o -1 (errno). */
int io_loop_mod(io_loop *loop, int fd, unsigned mask);

/* Deja de observar fd. No lo cierra: cerrar es responsabilidad de quien lo abrió. */
int io_loop_del(io_loop *loop, int fd);

/* Corre hasta que io_loop_stop() se llame desde un callback. Devuelve 0 o -1. */
int  io_loop_run(io_loop *loop);
void io_loop_stop(io_loop *loop);

/* "epoll" o "kqueue": qué implementación se compiló. Útil en logs y en CI. */
const char *io_backend_name(void);

/* Utilidad portable (sin #ifdef): activa O_NONBLOCK sobre un fd ya abierto. */
int io_set_nonblocking(int fd);

#endif /* PROXY_IO_EVENT_H */
