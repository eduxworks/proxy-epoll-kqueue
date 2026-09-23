proxy_inc = include_directories('.')

thread_dep = dependency('threads')

# Todo menos main.c va a una biblioteca estática, para que los tests puedan
# enlazar contra el mismo código que el ejecutable.
core_sources = files(
  'io_event_common.c',
) + io_event_impl

libproxycore = static_library('proxycore',
  core_sources,
  include_directories : proxy_inc,
  dependencies : [thread_dep])

proxy_exe = executable('proxy',
  files('main.c'),
  include_directories : proxy_inc,
  link_with : libproxycore,
  dependencies : [thread_dep],
  install : true)
