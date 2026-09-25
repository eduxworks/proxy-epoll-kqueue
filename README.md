# 🚄 Proxy Inverso de Alto Rendimiento — C11 + epoll/kqueue

## 🎯 Objetivo del proyecto

Implementar en **C11** un proxy inverso L7 asíncrono capaz de superar **50.000 peticiones/segundo**, portable entre Linux (`epoll`) y macOS (`kqueue`). Es el proyecto de sistemas del curso: aquí no hay framework — se programa directamente contra el sistema operativo.

Con este proyecto el alumno aprende:

- **I/O no bloqueante y event loops**: cómo un solo hilo atiende miles de conexiones (el modelo de nginx y Node.js por dentro).
- La diferencia entre `epoll` (Linux) y `kqueue` (macOS/BSD) y cómo abstraerlas tras una API común.
- Multiproceso con **`SO_REUSEPORT`** (un worker por CPU), balanceo round-robin/weighted/least-conn y health checks.
- Build con **Meson**, tests unitarios con **cmocka** y benchmarking con **wrk**.

## 🏗️ Arquitectura

```
Client ──► [Frontend socket :80/:443] ──► parse HTTP Host header ──► route lookup
        ──► [Backend pool] ──► round-robin ──► upstream connect
        ──► pipe bidireccional ──► close / keep-alive
```

- **Un event-loop por worker**, no bloqueante; edge-triggered (`EPOLLET` / `EV_CLEAR`).
- Capa de abstracción `io_event.[ch]` con la misma API sobre epoll y kqueue.
- Configuración en **TOML** (frontends, rutas por dominio, backends con pesos) con **recarga en caliente vía SIGHUP**.

### Módulos (C11)

| Módulo | Responsabilidad |
|--------|-----------------|
| `io_event` | Abstracción epoll/kqueue (`io_loop_create/add/mod/del/run/stop`) |
| `listener` | Accept loop por puerto frontal, `SO_REUSEPORT` |
| `connection` | Máquina de estados cliente↔upstream, arena de conexiones |
| `http_parser` | Extrae `Host:`; inyecta `X-Forwarded-For/Proto/Real-IP` |
| `router` | Hash djb2 para dominios exactos, wildcards `*.dom`, ruta default, RCU refcount |
| `backend_pool` | round_robin / weighted / least_conn + health pasivo |
| `health` | Sondas TCP/HTTP activas en hilo aparte |
| `config` | Parser TOML (tomlc99), validación, reload |
| `log` | Ring buffer 4096×512B con hilo consumidor |
| `buffer_pool` | Arena `mmap` de slots de 16 KB con freelist |
| `stats` | UNIX socket con snapshot JSON (uptime, contadores, backends) |

## ⚙️ Funcionalidades

- Múltiples frontends (puertos de escucha) y enrutamiento L7 por header `Host:`.
- Resolución de rutas: dominio exacto → wildcard `*.example.com` → `default` → `502`.
- Balanceo `round_robin`, `weighted` y `least_conn` con exclusión de backends caídos.
- Health checks activos (sondas) y pasivos (fallos de conexión).
- **Recarga de configuración sin cortar conexiones** (SIGHUP + swap atómico del router).
- Endpoint de estadísticas JSON.

## 💡 Solución

1. **Edge-triggered obliga a drenar**: con `EPOLLET`/`EV_CLEAR` el kernel avisa una sola vez por cambio; cada handler lee/escribe hasta `EAGAIN`. Es más eficiente pero menos indulgente que level-triggered — el corazón didáctico del proyecto.
2. **Portabilidad por abstracción**: `io_event.h` define la API; `io_event_epoll.c` y `io_event_kqueue.c` la implementan. Meson detecta la plataforma y compila la correcta.
3. **Reload sin downtime**: al recibir SIGHUP (self-pipe trick), se parsea la nueva config en memoria, se valida y se hace un **swap atómico del router** con refcount (RCU) — las conexiones en vuelo terminan con la config vieja.
4. **Logging sin bloquear el event loop**: los mensajes van a un ring buffer y un hilo aparte los escribe a disco.

## 📊 Benchmark

### Referencia del enunciado (macOS/ARM64, wrk, build release)

| Configuración | Req/s | Latencia media | Errores |
|---|---|---|---|
| `-t4 -c400 -d30s` | **54.183** | 7,39 ms | 0 |
| `-t4 -c200 -d30s` | **52.657** | 3,90 ms | 0 |
| `-t2 -c100 -d30s` | **55.311** | 1,84 ms | 0 |

Cifras **no reproducidas**: proceden del hardware del enunciado, no del nuestro.

### Medición propia

Linux 7.0.0 · Intel Core i7-11800H · 8 vCPU · 7,7 GB · build release · `wrk` 4.1.0

| Configuración | Req/s | Latencia media | Errores |
|---|---|---|---|
| `-t4 -c400 -d30s` | **345.120** | 1,10 ms | 0 |
| `-t4 -c200 -d30s` | **312.288** | 609 µs | 0 |
| `-t2 -c100 -d30s` | **246.254** | 366 µs | 0 |

Meta de ≥ 50.000 req/s **superada** en los tres escenarios, sin errores.

Condiciones de la medida, porque un req/s sin contexto no significa nada:

- Son las cifras de la **ejecución más conservadora** de dos, con **todos los
  módulos activos** (registro asíncrono y sondas de salud incluidos). La otra
  ejecución dio 382.786 / 346.581 / 253.741: alrededor de un 10 % de varianza
  entre repeticiones, que es lo normal en una máquina virtual. Se publica la
  baja; quedarse con la mejor sin decirlo no sería una medición, sería una
  selección.
- Los dos hilos que añaden `log` y `health` **no tienen coste medible**: las
  cifras con ellos activos quedan dentro de la varianza de las medidas sin
  ellos.

- `wrk`, proxy y los 3 backends comparten máquina, como en el escenario de
  referencia. Todo el tráfico es *loopback*.
- Los backends son `bench/bench_backend`, event-driven, con `Content-Length` y
  keep-alive; un backend a pelo sostiene ~361.000 req/s, así que el techo de
  los tres queda por encima de lo medido.
- Reproducible con `./bench/bench_proxy.sh`, que además comprueba que los
  puertos estén libres y que el build sea `release` antes de medir nada.

## 🚀 Cómo ejecutar

```bash
# Build
meson setup build && meson compile -C build

# Tests: los 22 casos cmocka de las 4 suites del enunciado
#   (config 4 · router 6 · http_parser 7 · backend_pool 5)
#   más 32 en 7 suites propias: io_event, listener, buffer_pool,
#   http_framing, log, health y stats.
# meson test reporta 11 targets (un binario por suite), 54 casos en total.
meson test -C build

# Ejecutar con una config TOML
./build/src/proxy -c proxy.toml

# Recargar configuración sin reiniciar
kill -HUP <pid>

# Benchmark
./bench/bench_proxy.sh
```
