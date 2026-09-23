cmocka_dep = dependency('cmocka', required : get_option('with_tests'))

if not cmocka_dep.found()
  message('cmocka no encontrado: no se compilan los tests')
  subdir_done()
endif

# E19: el README declara 22 tests en 4 suites (http_parser, router,
# backend_pool, config). Cada suite entra con su módulo; por ahora existe el
# bucle de eventos, así que solo hay esta.
test_io_event = executable('test_io_event',
  files('test_io_event.c'),
  include_directories : proxy_inc,
  link_with : libproxycore,
  dependencies : [cmocka_dep])

test('io_event', test_io_event, suite : 'io_event')
