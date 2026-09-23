cmocka_dep = dependency('cmocka', required : get_option('with_tests'))

if not cmocka_dep.found()
  message('cmocka no encontrado: no se compilan los tests')
  subdir_done()
endif

# cmocka 2.x usa "##__VA_ARGS__" en sus macros, una extensión GNU que clang
# rechaza con -Wpedantic -Werror (se ve en macOS, no en Ubuntu, que aún trae
# cmocka 1.1.7). Se incluye como cabecera de sistema para que sus avisos no
# tumben el build: nuestro código sigue compilando con las mismas exigencias.
cmocka_dep = cmocka_dep.as_system('system')

# E19: el README declara 22 casos cmocka en 4 suites (http_parser, router,
# backend_pool, config). Cada suite entra con su módulo. test_io_event no está
# en esa cuenta: cubre la abstracción de E/S, que el README lista como módulo
# pero no como suite.
suites = [
  ['io_event', 'test_io_event.c'],
  ['config',   'test_config.c'],
  ['router',   'test_router.c'],
]

foreach s : suites
  exe = executable('test_' + s[0],
    files(s[1]),
    dependencies : [proxycore_dep, cmocka_dep])
  test(s[0], exe, suite : s[0])
endforeach
