/* net_compat.c — único fichero, junto a io_event_*.c, con #ifdef de plataforma.
 *
 * Dos diferencias reales y con consecuencias de diseño:
 *
 * 1. SO_REUSEPORT. En Linux reparte conexiones entre los procesos que
 *    comparten puerto. FreeBSD separó ambos comportamientos: SO_REUSEPORT es
 *    solo compartir, y el reparto es SO_REUSEPORT_LB (12.0+). macOS mantiene
 *    SO_REUSEPORT sin reparto, así que allí varios workers escuchando no se
 *    turnan y el modelo de E5 necesita otra solución.
 *
 * 2. accept4() no existe en macOS: hay que poner O_NONBLOCK y FD_CLOEXEC a
 *    mano, con la ventana de carrera que eso implica entre fork y exec.
 */

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "net_compat.h"

#if defined(__linux__)
#define PROXY_REUSE_OPT      SO_REUSEPORT
#define PROXY_REUSE_NAME     "SO_REUSEPORT"
#define PROXY_REUSE_BALANCES true
#define PROXY_HAVE_ACCEPT4   1
#elif defined(__FreeBSD__) && defined(SO_REUSEPORT_LB)
#define PROXY_REUSE_OPT      SO_REUSEPORT_LB
#define PROXY_REUSE_NAME     "SO_REUSEPORT_LB"
#define PROXY_REUSE_BALANCES true
#define PROXY_HAVE_ACCEPT4   1
#else /* macOS y BSD sin _LB */
#define PROXY_REUSE_OPT      SO_REUSEPORT
#define PROXY_REUSE_NAME     "SO_REUSEPORT"
#define PROXY_REUSE_BALANCES false
#define PROXY_HAVE_ACCEPT4   0
#endif

int net_set_reuseport(int fd)
{
    int on = 1;
    return setsockopt(fd, SOL_SOCKET, PROXY_REUSE_OPT, &on, sizeof on);
}

const char *net_reuseport_name(void)
{
    return PROXY_REUSE_NAME;
}

bool net_reuseport_balances(void)
{
    return PROXY_REUSE_BALANCES;
}

int net_accept(int lfd, struct sockaddr_storage *peer, socklen_t *plen)
{
#if PROXY_HAVE_ACCEPT4
    return accept4(lfd, (struct sockaddr *)peer, plen,
                   SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    int fd = accept(lfd, (struct sockaddr *)peer, plen);
    if (fd < 0) {
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
#endif
}
