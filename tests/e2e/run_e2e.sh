#!/usr/bin/env bash
#
# run_e2e.sh — prueba de funcionamiento del proxy (R9).
#
# Los tres pasos del enunciado:
#   1. generar datos de configuración
#   2. lanzar servidores de servicio
#   3. dar de alta dominios en /etc/hosts
#
# Sin privilegios para tocar /etc/hosts, --no-hosts valida exactamente lo mismo
# con `curl --resolve`, que es el mismo mecanismo de resolución sin efectos
# sobre el sistema.
#
# Uso:
#   ./tests/e2e/run_e2e.sh [--no-hosts] [--build-dir DIR] [--keep]
#
# Sale 0 solo si pasan todas las aserciones.

set -u

# --- parámetros ------------------------------------------------------------

BUILD_DIR="build"
USE_HOSTS=auto
KEEP=false

while [ $# -gt 0 ]; do
  case "$1" in
    --no-hosts)   USE_HOSTS=false ;;
    --build-dir)  BUILD_DIR="$2"; shift ;;
    --keep)       KEEP=true ;;
    -h|--help)    sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "opción desconocida: $1" >&2; exit 2 ;;
  esac
  shift
done

FRONT_PUBLIC=8080
FRONT_INTERNAL=8081
BACKENDS="9001 9002 9003"
BACKEND_B=9011

PROXY_BIN="$BUILD_DIR/src/proxy"
ECHO_BIN="$BUILD_DIR/tests/echo_server"

HOSTS_FILE=/etc/hosts
HOSTS_BACKUP=/etc/hosts.proxy.bak
HOSTS_MARK="# proxy-e2e"

pass=0
fail=0
skip=0

ok()   { echo "  [ok]   $1"; pass=$((pass + 1)); }
bad()  { echo "  [FALLO] $1"; fail=$((fail + 1)); }
note() { echo "  [--]   $1"; skip=$((skip + 1)); }
say()  { echo; echo "== $1"; }

# --- limpieza --------------------------------------------------------------

TMPDIR_E2E="$(mktemp -d)"
PROXY_PID=""
ECHO_PIDS=""
HOSTS_TOUCHED=false

cleanup() {
  local rc=$?

  [ -n "$PROXY_PID" ] && kill -TERM "$PROXY_PID" 2>/dev/null
  for p in $ECHO_PIDS; do kill -TERM "$p" 2>/dev/null; done
  wait 2>/dev/null

  # El /etc/hosts se restaura pase lo que pase, también si el script muere a
  # mitad: dejar entradas sueltas rompería la resolución de la máquina.
  if [ "$HOSTS_TOUCHED" = true ]; then
    if [ -f "$HOSTS_BACKUP" ]; then
      cp "$HOSTS_BACKUP" "$HOSTS_FILE" && rm -f "$HOSTS_BACKUP"
      echo "  (/etc/hosts restaurado)"
    else
      sed -i.bak "/$HOSTS_MARK/d" "$HOSTS_FILE" 2>/dev/null
    fi
  fi

  if [ "$KEEP" = true ]; then
    echo "  (ficheros en $TMPDIR_E2E)"
  else
    rm -rf "$TMPDIR_E2E"
  fi

  exit $rc
}
trap cleanup EXIT INT TERM

# --- comprobaciones previas ------------------------------------------------

for bin in "$PROXY_BIN" "$ECHO_BIN"; do
  if [ ! -x "$bin" ]; then
    echo "No encuentro $bin" >&2
    echo "Compila primero:  meson setup $BUILD_DIR && meson compile -C $BUILD_DIR" >&2
    exit 2
  fi
done

for cmd in curl; do
  command -v "$cmd" >/dev/null || { echo "falta $cmd" >&2; exit 2; }
done

port_busy() {
  # Sin ss ni netstat garantizados: se pregunta conectando.
  (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3>&-; return 0; }
  return 1
}

for p in $FRONT_PUBLIC $FRONT_INTERNAL $BACKENDS $BACKEND_B; do
  if port_busy "$p"; then
    echo "El puerto $p ya está ocupado; cierra lo que lo use y reintenta." >&2
    exit 2
  fi
done

wait_for_port() {
  local port=$1 tries=50
  while [ $tries -gt 0 ]; do
    port_busy "$port" && return 0
    tries=$((tries - 1))
    sleep 0.1
  done
  return 1
}

# --- paso 1: generar la configuración --------------------------------------

say "Paso 1: generando configuración"

CONF="$TMPDIR_E2E/proxy.toml"

# workers = 1 a propósito. Con varios workers cada uno lleva su propio turno de
# round-robin (no comparten estado, que es justo lo que los hace rápidos), así
# que el reparto es uniforme en conjunto pero no exacto. Para afirmar "100 ±1"
# hace falta un solo turno. El caso multiproceso se comprueba al final.
cat > "$CONF" <<TOML
[global]
workers   = 1
max_conns = 1024
log_level = "info"

[[frontend]]
name   = "public"
listen = "127.0.0.1:$FRONT_PUBLIC"

  [[frontend.route]]
  host    = "a.test"
  backend = "pool_a"

  [[frontend.route]]
  host    = "*.b.test"
  backend = "pool_b"

[[frontend]]
name   = "internal"
listen = "127.0.0.1:$FRONT_INTERNAL"

  [[frontend.route]]
  host    = "admin.test"
  backend = "pool_admin"

[[backend]]
name    = "pool_a"
balance = "round_robin"
servers = [
  { addr = "127.0.0.1:9001" },
  { addr = "127.0.0.1:9002" },
  { addr = "127.0.0.1:9003" },
]

[[backend]]
name    = "pool_b"
servers = [ { addr = "127.0.0.1:$BACKEND_B" } ]

[[backend]]
name    = "pool_admin"
servers = [ { addr = "127.0.0.1:$BACKEND_B" } ]
TOML

echo "  $CONF"

# --- paso 2: lanzar los servidores de servicio ------------------------------

say "Paso 2: lanzando servidores de servicio"

for p in $BACKENDS; do
  "$ECHO_BIN" "$p" "server-$p" >>"$TMPDIR_E2E/backends.log" 2>&1 &
  eval "PID_$p=$!"
  ECHO_PIDS="$ECHO_PIDS $!"
done
"$ECHO_BIN" "$BACKEND_B" "server-$BACKEND_B" >>"$TMPDIR_E2E/backends.log" 2>&1 &
ECHO_PIDS="$ECHO_PIDS $!"

for p in $BACKENDS $BACKEND_B; do
  wait_for_port "$p" || { echo "el backend $p no arrancó" >&2; exit 2; }
done
echo "  backends en $BACKENDS y $BACKEND_B"

# --- paso 3: dar de alta los dominios ---------------------------------------

say "Paso 3: registrando dominios"

# Todos los dominios que el script va a pedir, incluidos los que deben fallar:
# un Host que no resuelve da 000 en curl, no el 502 del proxy, y eso no prueba
# nada sobre el enrutado.
DOMAINS="a.test x.b.test admin.test desconocido.test renombrado.test"

if [ "$USE_HOSTS" = auto ]; then
  if [ -w "$HOSTS_FILE" ]; then USE_HOSTS=true; else USE_HOSTS=false; fi
fi

if [ "$USE_HOSTS" = true ]; then
  cp "$HOSTS_FILE" "$HOSTS_BACKUP" || { echo "no pude copiar $HOSTS_FILE" >&2; exit 2; }
  HOSTS_TOUCHED=true
  for d in $DOMAINS; do
    echo "127.0.0.1 $d $HOSTS_MARK" >> "$HOSTS_FILE"
  done
  echo "  añadidos a $HOSTS_FILE (copia en $HOSTS_BACKUP): $DOMAINS"
else
  echo "  sin privilegios sobre $HOSTS_FILE: se usa curl --resolve"
  echo "  (equivalente para el proxy: el Host viaja igual en la petición)"
fi

# curl con o sin /etc/hosts, misma llamada para el resto del script
req() {
  local host=$1 port=$2; shift 2
  if [ "$USE_HOSTS" = true ]; then
    curl -s --max-time 5 "$@" "http://$host:$port/"
  else
    curl -s --max-time 5 --resolve "$host:$port:127.0.0.1" "$@" "http://$host:$port/"
  fi
}

req_code() {
  local host=$1 port=$2; shift 2
  if [ "$USE_HOSTS" = true ]; then
    curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$@" "http://$host:$port/"
  else
    curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
      --resolve "$host:$port:127.0.0.1" "$@" "http://$host:$port/"
  fi
}

# --- arranque del proxy -----------------------------------------------------

start_proxy() {
  "$PROXY_BIN" -c "$CONF" >>"$TMPDIR_E2E/proxy.log" 2>&1 &
  PROXY_PID=$!
  wait_for_port "$FRONT_PUBLIC" || {
    echo "el proxy no llegó a escuchar; últimas líneas del log:" >&2
    tail -20 "$TMPDIR_E2E/proxy.log" >&2
    exit 2
  }
}

stop_proxy() {
  [ -n "$PROXY_PID" ] && kill -TERM "$PROXY_PID" 2>/dev/null
  PROXY_PID=""
  sleep 0.3
}

start_proxy
echo "  proxy escuchando en $FRONT_PUBLIC y $FRONT_INTERNAL"

# --- aserciones -------------------------------------------------------------

say "Aserciones"

# E7: enrutado por header Host
body=$(req a.test $FRONT_PUBLIC)
case "$body" in
  server-900*) ok "E7  a.test resuelve a pool_a ($(echo "$body" | head -1))" ;;
  *)           bad "E7  a.test devolvió: $(echo "$body" | head -1)" ;;
esac

# E8: wildcard a un pool distinto
body=$(req x.b.test $FRONT_PUBLIC)
case "$body" in
  server-$BACKEND_B*) ok "E8  *.b.test resuelve a pool_b, distinto de a.test" ;;
  *)                  bad "E8  x.b.test devolvió: $(echo "$body" | head -1)" ;;
esac

# E8: sin ruta y sin default => 502
code=$(req_code desconocido.test $FRONT_PUBLIC)
[ "$code" = "502" ] && ok "E8  un Host sin ruta da 502" \
                    || bad "E8  Host sin ruta dio $code"

# E6: los frontends están aislados entre sí
code=$(req_code admin.test $FRONT_PUBLIC)
[ "$code" = "502" ] && ok "E6  admin.test NO resuelve en el frontend público" \
                    || bad "E6  admin.test dio $code en el puerto público"
body=$(req admin.test $FRONT_INTERNAL)
case "$body" in
  server-*) ok "E6  admin.test sí resuelve en el frontend interno" ;;
  *)        bad "E6  admin.test en el interno devolvió: $(echo "$body" | head -1)" ;;
esac

# E14: el upstream recibe la IP real del cliente
body=$(req a.test $FRONT_PUBLIC)
case "$body" in
  *"xff=127.0.0.1"*) ok "E14 el upstream recibe X-Forwarded-For con la IP del cliente" ;;
  *)                 bad "E14 no llegó X-Forwarded-For: $(echo "$body" | tr '\n' ' ')" ;;
esac

# E14: la cadena de proxies se conserva
body=$(req a.test $FRONT_PUBLIC -H 'X-Forwarded-For: 10.0.0.1')
case "$body" in
  *"xff=10.0.0.1, 127.0.0.1"*) ok "E14 X-Forwarded-For se encadena, no se sustituye" ;;
  *)                           bad "E14 encadenado incorrecto: $(echo "$body" | tr '\n' ' ')" ;;
esac

# E2: keep-alive. curl reutiliza la conexión cuando le pides varias URLs de
# una vez, y avisa por el canal de traza cuando lo hace.
if [ "$USE_HOSTS" = true ]; then
  ka=$(curl -sv --max-time 5 -o /dev/null \
        "http://a.test:$FRONT_PUBLIC/1" "http://a.test:$FRONT_PUBLIC/2" \
        "http://a.test:$FRONT_PUBLIC/3" 2>&1)
else
  ka=$(curl -sv --max-time 5 -o /dev/null --resolve "a.test:$FRONT_PUBLIC:127.0.0.1" \
        "http://a.test:$FRONT_PUBLIC/1" "http://a.test:$FRONT_PUBLIC/2" \
        "http://a.test:$FRONT_PUBLIC/3" 2>&1)
fi
reused=$(printf '%s' "$ka" | grep -ci 'Re-using existing connection' || true)
[ "$reused" -ge 2 ] && ok "E2  el cliente reutiliza la conexión ($reused de 2 veces)" \
                    || bad "E2  no hubo keep-alive con el cliente ($reused reutilizaciones)"

# E9: round-robin reparte 300 entre 3
say "E9: reparto round-robin (300 peticiones)"
: > "$TMPDIR_E2E/tally"
for _ in $(seq 1 300); do
  req a.test $FRONT_PUBLIC | head -1 >> "$TMPDIR_E2E/tally"
done
sort "$TMPDIR_E2E/tally" | uniq -c | sed 's/^/       /'

rr_ok=true
for p in $BACKENDS; do
  n=$(grep -c "^server-$p$" "$TMPDIR_E2E/tally")
  [ "$n" -ge 99 ] && [ "$n" -le 101 ] || { rr_ok=false; echo "       server-$p: $n (esperado 100 ±1)"; }
done
[ "$rr_ok" = true ] && ok "E9  cada backend recibe 100 ±1 de 300" \
                    || bad "E9  el reparto no es uniforme"

# E10/E11: un backend muerto sale del reparto sin cortar el servicio
say "E10/E11: salud pasiva"
# Se mata por el PID que guardamos al arrancarlo, no buscándolo por nombre:
# pgrep puede no encontrarlo y un kill sin argumento pasa desapercibido.
kill -TERM "$PID_9002" 2>/dev/null
sleep 0.3

# Una sola petición por vuelta, de la que se sacan código y cuerpo. Con dos
# peticiones y registrando solo una, el muestreo cae siempre en la misma fase
# del turno y el reparto parece mucho más sesgado de lo que es.
: > "$TMPDIR_E2E/tally2"
errs=0
for _ in $(seq 1 60); do
  out=$(req a.test $FRONT_PUBLIC -w $'\n%{http_code}')
  [ "$(echo "$out" | tail -1)" = "200" ] || errs=$((errs + 1))
  echo "$out" | head -1 >> "$TMPDIR_E2E/tally2"
done
sort "$TMPDIR_E2E/tally2" | uniq -c | sed 's/^/       /'

[ "$errs" -le 3 ] && ok "E10 con un backend caído se sigue respondiendo 200 ($errs fallos de 60)" \
                  || bad "E10 $errs fallos de 60 con un backend caído"
[ "$(grep -c '^server-9002$' "$TMPDIR_E2E/tally2")" -eq 0 ] \
  && ok "E11 el backend caído queda excluido del reparto" \
  || bad "E11 el backend caído sigue recibiendo tráfico"

# E13: recarga en caliente
say "E13: recarga sin cortar el servicio"
sed -i.bak 's/host    = "a.test"/host    = "renombrado.test"/' "$CONF"
kill -HUP "$PROXY_PID"
sleep 0.5

code=$(req_code renombrado.test $FRONT_PUBLIC)
[ "$code" = "200" ] && ok "E13 la ruta nueva funciona tras SIGHUP" \
                    || bad "E13 renombrado.test dio $code tras recargar"
code=$(req_code a.test $FRONT_PUBLIC)
[ "$code" = "502" ] && ok "E13 la ruta vieja desaparece tras recargar" \
                    || bad "E13 a.test sigue dando $code tras recargar"

# E13: un reload roto no degrada el servicio
cp "$CONF" "$TMPDIR_E2E/proxy.toml.good"
echo 'esto no es TOML {{{' > "$CONF"
kill -HUP "$PROXY_PID"
sleep 0.5
code=$(req_code renombrado.test $FRONT_PUBLIC)
[ "$code" = "200" ] && ok "E13 una recarga inválida conserva la configuración anterior" \
                    || bad "E13 tras una recarga inválida el servicio dio $code"
cp "$TMPDIR_E2E/proxy.toml.good" "$CONF"

# E5: multiproceso
say "E5: un worker por CPU"
stop_proxy
sed -i.bak 's/^workers   = 1$/workers   = 0/' "$CONF"
start_proxy

: > "$TMPDIR_E2E/tally3"
errs=0
for _ in $(seq 1 120); do
  out=$(req renombrado.test $FRONT_PUBLIC -w $'\n%{http_code}')
  [ "$(echo "$out" | tail -1)" = "200" ] || errs=$((errs + 1))
  echo "$out" | head -1 >> "$TMPDIR_E2E/tally3"
done
sort "$TMPDIR_E2E/tally3" | uniq -c | sed 's/^/       /'

distinct=$(sort -u "$TMPDIR_E2E/tally3" | grep -c '^server-')
[ "$errs" -eq 0 ] && ok "E5  con varios workers no hay errores en 120 peticiones" \
                  || bad "E5  $errs peticiones sin respuesta con varios workers"
# Con 9002 caído quedan dos backends vivos. Cada worker lleva su propio turno,
# así que no se exige proporción, solo que todos los vivos reciban trabajo.
[ "$distinct" -ge 2 ] && ok "E5  el trabajo llega a todos los backends vivos ($distinct)" \
                      || bad "E5  solo $distinct backend recibió tráfico"

# E17: pendiente
note "E17 socket de estadísticas JSON: el módulo stats aún no existe"

# --- resumen ----------------------------------------------------------------

say "Resumen"
echo "  aserciones OK:        $pass"
echo "  aserciones fallidas:  $fail"
echo "  pendientes:           $skip"

if [ "$fail" -ne 0 ]; then
  echo
  echo "Últimas líneas del log del proxy:"
  tail -20 "$TMPDIR_E2E/proxy.log" | sed 's/^/    /'
fi

exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)
