# Backend de carga para el benchmark. No es un test: lo arranca
# bench_proxy.sh, que orquesta backends, proxy y wrk.
bench_backend = executable('bench_backend',
  files('bench_backend.c'),
  dependencies : [proxycore_dep],
  build_by_default : true)
