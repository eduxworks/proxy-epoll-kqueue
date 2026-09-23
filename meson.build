project('proxy', 'c',
  version : '0.1.0',
  meson_version : '>= 0.60.0',
  default_options : [
    'c_std=c11',
    'warning_level=3',   # -Wall -Wextra -Wpedantic
    'werror=true',
    'buildtype=debugoptimized',
  ])

host_os = host_machine.system()

# E3: la plataforma decide qué implementación del bucle de eventos se compila.
# El resto del proyecto no sabe en qué sistema corre.
if host_os == 'linux'
  io_event_impl = files('src/io_event_epoll.c')
  io_backend = 'epoll'
  # Con -std=c11 estricto glibc define __STRICT_ANSI__ y esconde epoll_create1,
  # pipe() y compañía. En macOS y FreeBSD las cabeceras las exponen por defecto.
  add_project_arguments('-D_GNU_SOURCE', language : 'c')
elif host_os == 'darwin' or host_os == 'freebsd'
  io_event_impl = files('src/io_event_kqueue.c')
  io_backend = 'kqueue'
else
  error('Plataforma no soportada: @0@. Este proyecto requiere epoll (Linux) o kqueue (macOS/FreeBSD).'.format(host_os))
endif

message('Sistema @0@ -> backend de E/S: @1@'.format(host_os, io_backend))

subdir('src')
subdir('tests')
