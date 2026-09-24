#!/usr/bin/env bash
#
# bench_proxy.sh — benchmark del proxy con wrk (E2, E20).
#
# Los tres escenarios que publica el README, con el umbral de aprobado en
# >= 50.000 req/s y 0 errores en los tres.
#
# Antes de medir el proxy mide los backends a pelo. Si el proxy se acerca a esa
# cifra, el cuello de botella es la máquina o el backend, no el código: una
# medida así no dice nada del proxy y el script lo avisa en vez de dejar que
# parezca un resultado.
#
# Uso:
#   ./bench/bench_proxy.sh [--build-dir DIR] [--quick] [--duration 30s]
#
#   --quick   5 segundos por escenario, para comprobar que todo encaja
#
# Sale 0 si los tres escenarios superan el umbral.

set -u

BUILD_DIR="build-rel"
DURATION="30s"
# No 8080: lo ocupa medio mundo (Jetty, Tomcat, servidores de desarrollo). Si
# el puerto está cogido, wrk mide ese otro servicio y el informe sale con
# cifras que no son del proxy.
PORT=18080
BACKEND_PORTS="19101 19102 19103"

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift ;;
    --duration)  DURATION="$2";  shift ;;
    --port)      PORT="$2";      shift ;;
    --quick)     DURATION="5s" ;;
    -h|--help)   sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "opción desconocida: $1" >&2; exit 2 ;;
  esac
  shift
done

PROXY_BIN="$BUILD_DIR/src/proxy"
BACKEND_BIN="$BUILD_DIR/bench/bench_backend"

THRESHOLD=50000

# --- entorno ---------------------------------------------------------------

# Sin descriptores no se llega a las conexiones del escenario grande, y el
# fallo aparece como "errores de conexión" que parecen del proxy.
ulimit -n 65535 2>/dev/null || true
NOFILE=$(ulimit -n)

for bin in "$PROXY_BIN" "$BACKEND_BIN"; do
  if [ ! -x "$bin" ]; then
    echo "No encuentro $bin" >&2
    echo "Compila en modo release:" >&2
    echo "  meson setup $BUILD_DIR --buildtype=release && meson compile -C $BUILD_DIR" >&2
    exit 2
  fi
done

command -v wrk >/dev/null || { echo "falta wrk (ver requerimiento.md §4.2)" >&2; exit 2; }

# Medir un build de depuración y publicarlo sería engañarse: las cifras pueden
# ser la mitad.
BUILDTYPE=$(grep -m1 '^buildtype' "$BUILD_DIR/meson-info/intro-buildoptions.json" 2>/dev/null || true)
if [ -f "$BUILD_DIR/meson-info/intro-buildoptions.json" ]; then
  if ! grep -q '"name": "buildtype", "value": "release"' \
        "$BUILD_DIR/meson-info/intro-buildoptions.json" 2>/dev/null; then
    bt=$(sed -n 's/.*"name": "buildtype", "value": "\([a-z]*\)".*/\1/p' \
          "$BUILD_DIR/meson-info/intro-buildoptions.json" | head -1)
    if [ -n "$bt" ] && [ "$bt" != "release" ]; then
      echo "AVISO: $BUILD_DIR es buildtype=$bt, no release."
      echo "       Las cifras no son comparables con las del README."
      echo
    fi
  fi
fi

# --- datos de la máquina, que sin ellos un req/s no significa nada ----------

OS_NAME="$(uname -s) $(uname -r)"
case "$(uname -s)" in
  Linux)
    CPU_MODEL=$(sed -n 's/^model name[ \t]*: //p' /proc/cpuinfo | head -1)
    CORES=$(nproc)
    RAM_GB=$(awk '/MemTotal/ {printf "%.1f", $2/1048576}' /proc/meminfo)
    ;;
  Darwin)
    CPU_MODEL=$(sysctl -n machdep.cpu.brand_string 2>/dev/null)
    CORES=$(sysctl -n hw.ncpu)
    RAM_GB=$(awk -v b="$(sysctl -n hw.memsize)" 'BEGIN{printf "%.1f", b/1073741824}')
    ;;
  *)
    CPU_MODEL="desconocida"
    CORES=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo "?")
    RAM_GB="?"
    ;;
esac
[ -n "${CPU_MODEL:-}" ] || CPU_MODEL="desconocida"

# --- limpieza --------------------------------------------------------------

TMPD="$(mktemp -d)"
PROXY_PID=""
BACKEND_PIDS=""

# Los crudos de una ejecución anterior con errores sobreviven a una ejecución
# limpia, y entonces el informe nuevo se lee con los datos viejos al lado.
rm -f bench/results/wrk-run*.txt bench/results/proxy.log 2>/dev/null

cleanup() {
  [ -n "$PROXY_PID" ] && kill -TERM "$PROXY_PID" 2>/dev/null
  # Al grupo, no al PID: bench_backend y el proxy crean hijos con fork, y matar
  # solo al padre deja workers escuchando que envenenan la siguiente ejecución.
  for p in $BACKEND_PIDS; do
    kill -TERM "-$p" 2>/dev/null || kill -TERM "$p" 2>/dev/null
  done
  wait 2>/dev/null
  rm -rf "$TMPD"
}
trap cleanup EXIT INT TERM

port_busy() {
  (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3>&-; return 0; }
  return 1
}

wait_for_port() {
  local port=$1 tries=50
  while [ $tries -gt 0 ]; do
    port_busy "$port" && return 0
    tries=$((tries - 1))
    sleep 0.1
  done
  return 1
}

# Si algo ya escucha donde vamos a medir, wrk mediría ESO. Comprobarlo antes
# cuesta un segundo; descubrirlo después cuesta un informe entero de cifras
# que no son del proxy.
for p in $PORT $BACKEND_PORTS; do
  if port_busy "$p"; then
    echo "El puerto $p ya está ocupado por otro proceso." >&2
    echo "Libéralo, o elige otro con --port." >&2
    command -v ss >/dev/null && ss -tlnp 2>/dev/null | grep ":$p " >&2
    exit 2
  fi
done

# --- arranque --------------------------------------------------------------

CONF="$TMPD/bench.toml"
{
  echo '[global]'
  echo 'workers   = 0'
  echo 'max_conns = 65536'
  echo 'log_level = "error"'
  echo
  echo '[[frontend]]'
  echo 'name   = "bench"'
  echo "listen = \"127.0.0.1:$PORT\""
  echo '  [[frontend.route]]'
  echo '  host    = "default"'
  echo '  backend = "pool"'
  echo
  echo '[[backend]]'
  echo 'name    = "pool"'
  echo 'balance = "round_robin"'
  echo 'servers = ['
  for p in $BACKEND_PORTS; do
    echo "  { addr = \"127.0.0.1:$p\" },"
  done
  echo ']'
} > "$CONF"

# Un worker por backend para que no sean ellos el límite.
BACKEND_WORKERS=$(( CORES > 2 ? 2 : 1 ))
for p in $BACKEND_PORTS; do
  "$BACKEND_BIN" "$p" "$BACKEND_WORKERS" >/dev/null 2>&1 &
  BACKEND_PIDS="$BACKEND_PIDS $!"
  wait_for_port "$p" || { echo "el backend $p no arrancó" >&2; exit 2; }
done

"$PROXY_BIN" -c "$CONF" >"$TMPD/proxy.log" 2>&1 &
PROXY_PID=$!
wait_for_port "$PORT" || {
  echo "el proxy no llegó a escuchar:" >&2
  cat "$TMPD/proxy.log" >&2
  exit 2
}

# Que el puerto responda no prueba que responda el proxy: con SO_REUSEPORT un
# worker puede haber fallado el bind mientras otro proceso sirve ahí.
sleep 0.3
if grep -qi 'address already in use\|bind ' "$TMPD/proxy.log"; then
  echo "Algún worker no pudo abrir $PORT; lo que haya ahí no es (solo) el proxy:" >&2
  grep -i 'bind ' "$TMPD/proxy.log" | head -3 >&2
  exit 2
fi

# --- ejecución -------------------------------------------------------------

# Imprime "reqs latencia errores" a partir de la salida de wrk.
parse_wrk() {
  local out="$1"
  local reqs lat errs socket_errs non2xx

  reqs=$(sed -n 's/^Requests\/sec:[ \t]*//p' "$out" | head -1)
  lat=$(awk '/^ *Latency/ {print $2; exit}' "$out")

  # wrk indenta estas dos líneas con espacios. Anclarlas a principio de línea
  # hacía que los errores no se contaran nunca: el resumen decía "0 errores"
  # mientras el fichero crudo tenía cientos de miles de respuestas no-2xx.
  socket_errs=$(sed -n 's/.*connect \([0-9]*\), read \([0-9]*\), write \([0-9]*\), timeout \([0-9]*\).*/\1+\2+\3+\4/p' "$out" | head -1)
  [ -n "$socket_errs" ] && errs=$(( socket_errs )) || errs=0
  non2xx=$(sed -n 's/^[[:space:]]*Non-2xx or 3xx responses:[[:space:]]*//p' "$out" | head -1)
  [ -n "$non2xx" ] && errs=$(( errs + non2xx ))

  echo "${reqs:-0} ${lat:-?} $errs"
}

run_wrk() {  # run_wrk <etiqueta> <threads> <conns> <url> <fichero>
  wrk -t"$2" -c"$3" -d"$DURATION" --latency "$4" > "$5" 2>&1
  parse_wrk "$5"
}

NBK=$(echo $BACKEND_PORTS | wc -w | tr -d ' ')

echo "== Control: UN backend a pelo (para saber dónde está el límite)"
read -r ctl_reqs ctl_lat ctl_errs <<EOF
$(run_wrk control 4 200 "http://127.0.0.1:$(echo $BACKEND_PORTS | cut -d' ' -f1)/" "$TMPD/control.txt")
EOF
printf '   un backend: %s req/s, %s errores\n' "$ctl_reqs" "$ctl_errs"

# El proxy reparte entre los N backends, así que su techo no es el de uno solo.
# Comparar contra un backend haría saltar el aviso de saturación sin motivo.
ctl_int=${ctl_reqs%%.*}
CEILING=$(( ${ctl_int:-0} * NBK ))
printf '   techo estimado con %d backends: ~%d req/s\n' "$NBK" "$CEILING"

# El control se corre sin proxy. Si YA produce errores, la máquina no da para
# esta carga y los errores de los escenarios siguientes no son atribuibles al
# proxy: hay que decirlo antes de que alguien lea la tabla al revés.
if [ "${ctl_errs:-0}" -gt 0 ]; then
  echo "   AVISO: el control ya da $ctl_errs errores sin proxy de por medio."
  echo "          Son timeouts por saturación: hay más procesos que núcleos."
fi
echo

echo "== Proxy"
declare -a LABELS=("-t4 -c400" "-t4 -c200" "-t2 -c100")
declare -a THREADS=(4 4 2)
declare -a CONNS=(400 200 100)

results=""
fail=0

for i in 0 1 2; do
  label="${LABELS[$i]} -d$DURATION"
  printf '   %s ... ' "$label"
  read -r reqs lat errs <<EOF
$(run_wrk "$label" "${THREADS[$i]}" "${CONNS[$i]}" "http://127.0.0.1:$PORT/" "$TMPD/run$i.txt")
EOF
  printf '%s req/s, %s, %s errores\n' "$reqs" "$lat" "$errs"

  results="$results|$label|$reqs|$lat|$errs"

  int_reqs=${reqs%%.*}
  [ "${int_reqs:-0}" -ge "$THRESHOLD" ] || fail=1
  [ "${errs:-0}" -eq 0 ] || fail=1
done

# --- informe ---------------------------------------------------------------

REPORT="$TMPD/informe.md"
{
  echo "### Referencia (README del enunciado) — macOS/ARM64"
  echo
  echo "| Configuración | Req/s | Latencia media | Errores |"
  echo "|---|---|---|---|"
  echo "| \`-t4 -c400 -d30s\` | 54.183 | 7,39 ms | 0 |"
  echo "| \`-t4 -c200 -d30s\` | 52.657 | 3,90 ms | 0 |"
  echo "| \`-t2 -c100 -d30s\` | 55.311 | 1,84 ms | 0 |"
  echo
  echo "Cifras **no reproducidas**: son del hardware del enunciado, no de esta máquina."
  echo
  echo "### Medición propia — $OS_NAME · $CPU_MODEL · $CORES núcleos · ${RAM_GB} GB"
  echo
  echo "| Configuración | Req/s | Latencia media | Errores |"
  echo "|---|---|---|---|"
  echo "$results" | tr '|' '\n' | awk 'NR>1 {
      if ((NR-2) % 4 == 0) printf "| `%s` ", $0;
      else if ((NR-2) % 4 == 1) printf "| %s ", $0;
      else if ((NR-2) % 4 == 2) printf "| %s ", $0;
      else printf "| %s |\n", $0;
  }'
  echo
  nbk=$(echo $BACKEND_PORTS | wc -w | tr -d ' ')
  procs=$(( 4 + CORES + nbk * BACKEND_WORKERS ))
  echo "- Umbral: ≥ $THRESHOLD req/s y 0 errores en los tres escenarios."
  echo "- \`wrk\`, proxy y los $nbk backends comparten máquina, como en el README."
  echo "- Carga de procesos: ~$procs hilos activos (wrk 4 + proxy $CORES + backends $(( nbk * BACKEND_WORKERS ))) sobre $CORES CPU."
  echo "- Límite de descriptores: $NOFILE."
  echo "- Control (UN backend sin proxy): $ctl_reqs req/s, $ctl_errs errores; techo estimado con $nbk backends ~$CEILING req/s."
  if [ "${ctl_errs:-0}" -gt 0 ]; then
    echo "- **El control ya produce errores sin proxy**: son timeouts de saturación"
    echo "  del entorno, no fallos del proxy. Con núcleos dedicados desaparecen."
  fi
} > "$REPORT"

echo
cat "$REPORT"

# --- veredicto -------------------------------------------------------------

echo

best=0
for i in 0 1 2; do
  r=$(sed -n 's/^Requests\/sec:[ \t]*//p' "$TMPD/run$i.txt" | head -1)
  r=${r%%.*}
  [ "${r:-0}" -gt "$best" ] && best=${r:-0}
done

if [ "$CEILING" -gt 0 ] && [ "$best" -gt $(( CEILING * 80 / 100 )) ]; then
  echo "AVISO: el proxy ($best req/s) roza el techo estimado de los backends (~$CEILING req/s)."
  echo "       Lo que limita es la máquina, no el proxy: esta cifra mide el entorno."
fi

if [ "$fail" -eq 0 ]; then
  echo "RESULTADO: los tres escenarios superan $THRESHOLD req/s sin errores."
elif [ "${ctl_errs:-0}" -gt 0 ]; then
  echo "RESULTADO: no se cumple el criterio (req/s Y 0 errores), pero el control ya"
  echo "           fallaba sin proxy: el entorno está saturado. Repite en una máquina"
  echo "           con núcleos dedicados antes de dar por buena la parte de errores."
else
  echo "RESULTADO: por debajo del umbral. Publica la cifra con el contexto de arriba;"
  echo "           una medida baja y documentada es un resultado, inventarla no."
fi

mkdir -p bench/results 2>/dev/null
cp "$REPORT" "bench/results/ultimo.md" 2>/dev/null && \
  echo "(informe en bench/results/ultimo.md)"

# Con errores, la salida cruda de wrk es lo único que dice de qué tipo son
# —connect, read, write, timeout o respuestas no-2xx— y cada uno apunta a un
# sitio distinto. Sin ella solo queda adivinar.
if [ "$fail" -ne 0 ]; then
  for i in 0 1 2; do
    cp "$TMPD/run$i.txt" "bench/results/wrk-run$i.txt" 2>/dev/null
  done
  cp "$TMPD/proxy.log" "bench/results/proxy.log" 2>/dev/null
  echo "(salida cruda de wrk en bench/results/wrk-run*.txt)"
fi

exit $fail
