proxy_inc = include_directories('.')

thread_dep  = dependency('threads')
tomlc99_dep = dependency('tomlc99', fallback : ['tomlc99', 'tomlc99_dep'])

# Todo menos main.c va a una biblioteca estática, para que los tests enlacen
# exactamente el mismo código que el ejecutable.
core_sources = files(
  'io_event_common.c',
  'net_compat.c',
  'config.c',
  'router.c',
  'http_parser.c',
  'listener.c',
  'connection.c',
  'backend_pool.c',
  'buffer_pool.c',
  'log.c',
  'health.c',
  'stats.c',
) + io_event_impl

libproxycore = static_library('proxycore',
  core_sources,
  include_directories : proxy_inc,
  dependencies : [thread_dep, tomlc99_dep])

proxycore_dep = declare_dependency(
  link_with : libproxycore,
  include_directories : proxy_inc,
  dependencies : [thread_dep, tomlc99_dep])

proxy_exe = executable('proxy',
  files('main.c'),
  dependencies : [proxycore_dep],
  install : true)
