# tomlc99 no trae build de Meson: se lo ponemos nosotros (E12).
# Código de terceros, así que no hereda -Werror ni warning_level=3 del proyecto.
project('tomlc99', 'c',
  version : '0.1',
  default_options : [
    'c_std=c99',
    'warning_level=1',
    'werror=false',
  ])

tomlc99_lib = static_library('tomlc99',
  files('toml.c'),
  install : false)

tomlc99_dep = declare_dependency(
  link_with : tomlc99_lib,
  include_directories : include_directories('.'))
