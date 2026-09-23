# CLAUDE.md — Especificación del Proxy Inverso L7 (C11 + epoll/kqueue)

Documento de trabajo del repositorio. Define **qué** hay que construir, **con qué
contratos** y **cómo se valida**.

> **Precedencia.** El `README.md` es el documento contra el que se **evalúa** esta
> tarea: todo lo que aparece en él es **obligatorio y evaluable**, incluidas sus
> cifras concretas (≥ 50.000 req/s, 22 tests en 4 suites, ring buffer 4096×512 B,
> slots de 16 KB, hash djb2). Este `CLAUDE.md` **no añade ni recorta alcance**:
> desarrolla el README en criterios verificables. Ante cualquier discrepancia,
> manda el `README.md` y este documento se corrige.
> El enunciado (`../prompt.md`) es el subconjunto mínimo: R1–R9 de §1.2.

---

## 1. Requisitos evaluables

### 1.1 Derivados del README (todos obligatorios)

| ID | Requisito (fuente: README) | Módulo | Criterio de aceptación |
|----|---------------------------|--------|------------------------|
| E1 | Proxy inverso **L7** en **C11** asíncrono | todos | `-std=c11 -Wall -Wextra -Wpedantic -Werror` limpio |
| E2 | **≥ 50.000 req/s** en los tres escenarios de `wrk` | — | Tabla §7; 0 errores en los tres |
| E3 | Portable Linux (`epoll`) / macOS (`kqueue`) tras **API común** | `io_event` | Mismo header, dos `.c`; Meson elige; sin `#ifdef` fuera de ellos. `kqueue` se desarrolla en **FreeBSD** y se confirma en **macOS ARM64 vía CI**, nunca como código sin compilar ([`requerimiento.md`](requerimiento.md) §7) |
| E4 | **Un event loop por worker**, no bloqueante, **edge-triggered** | `io_event`, `connection` | Se registra con `EPOLLET`/`EV_CLEAR`; todo handler drena hasta `EAGAIN` |
| E5 | Multiproceso **`SO_REUSEPORT`**, un worker por CPU | `main`, `listener` | `workers=0` ⇒ `sysconf(_SC_NPROCESSORS_ONLN)` procesos, cada uno con su loop. Ojo §2.2: la opción **no reparte carga igual en los tres SO** |
| E6 | Múltiples frontends (puertos de escucha) | `listener` | ≥ 2 puertos simultáneos con tablas de rutas independientes |
| E7 | Enrutado por header **`Host:`** | `http_parser`, `router` | Dos `Host` distintos sobre el mismo puerto ⇒ backends distintos |
| E8 | Resolución **exacto → `*.dom` → `default` → 502** | `router` | §4.2; hash **djb2**; 502 cuando no hay ruta |
| E9 | Balanceo **`round_robin`, `weighted`, `least_conn`** | `backend_pool` | Los tres implementados y seleccionables por config |
| E10 | **Exclusión de backends caídos** del balanceo | `backend_pool` | Un backend DOWN no recibe tráfico; todos DOWN ⇒ 502 |
| E11 | Health checks **activos** (sondas TCP/HTTP, **hilo aparte**) y **pasivos** (fallo de conexión) | `health`, `backend_pool` | Sonda marca DOWN/UP con `fall`/`rise`; fallo de `connect` marca DOWN sin esperar sonda |
| E12 | Config **TOML** (frontends, rutas por dominio, backends con pesos) | `config` | Parser **tomlc99**; validación estricta §4.3 |
| E13 | **Recarga en caliente por SIGHUP sin cortar conexiones**, self-pipe + **swap atómico del router con refcount RCU** | `config`, `router` | §5; con `wrk` en marcha, `kill -HUP` ⇒ 0 errores |
| E14 | **Inyección `X-Forwarded-For` / `X-Forwarded-Proto` / `X-Real-IP`** | `http_parser` | El upstream las recibe con la IP real del cliente |
| E15 | **Logging sin bloquear el loop**: ring buffer **4096 × 512 B** + hilo consumidor | `log` | El loop nunca escribe a disco; desbordamiento contabilizado, no bloqueante |
| E16 | **Arena `mmap`** de slots de **16 KB** con freelist | `buffer_pool` | Sin `malloc` en el camino caliente; slots devueltos en toda ruta de error |
| E17 | **Stats JSON** sobre socket UNIX (uptime, contadores, backends) | `stats` | Snapshot válido según `jq`, servido sin bloquear el loop |
| E18 | Build con **Meson** | `meson.build` | `meson setup build && meson compile -C build` |
| E19 | **22 tests cmocka en 4 suites** | `tests/` | 22 casos en 4 binarios; `meson test -C build` reporta 4/4 targets en verde (reparto en §6.1) |
| E20 | Benchmark con **wrk** | `bench/bench_proxy.sh` | Los tres escenarios de §7 |

### 1.2 Requisitos del enunciado (`prompt.md`) — subconjunto cubierto

| ID | Enunciado | Cubierto por |
|----|-----------|--------------|
| R1 | epoll en Linux, kqueue en Mac | E3, E4 |
| R2 | Redirección por nombre de dominio | E7, E8 |
| R3 | Proxy Level 7 | E1, E7, E14 |
| R4 | Varias entradas en puertos distintos | E6 |
| R5 | Cada entrada con N salidas según dominio | E8 |
| R6 | Varios servidores en round-robin por salida | E9 |
| R7 | Proyecto gestionado con Meson | E18 |
| R8 | Reload dinámico de configuración | E13 |
| R9 | Test de funcionamiento (config + servidores + `/etc/hosts`) | §6.2 |

### 1.3 No contemplado por el README

Nada de esto aparece en el README, así que **no se implementa** salvo petición
explícita; si se implementara, no puede poner en riesgo E1–E20:

- **Terminación TLS.** El README dibuja `:80/:443` como puertos de escucha y no
  menciona certificados, OpenSSL ni handshake: `:443` se trata como **puerto TCP
  plano**. Si se pidiera TLS, sería un cambio de alcance, no un detalle.
- **Windows / IOCP como sustituto de macOS / `kqueue`.** El README fija `epoll` y
  `kqueue`; IOCP es un modelo de *compleción*, no de *disponibilidad*, e invalida
  la API de `io_event.h` y el drenado edge-triggered de E4 (análisis en
  [`requerimiento.md`](requerimiento.md) §7.6). Solo cabe como **añadido** una vez
  E1–E20 estén en verde, nunca en lugar de `kqueue`.
- HTTP/2, HTTP/3, upgrade a WebSocket, reescritura de `Transfer-Encoding`.
- Caché de respuestas, compresión, reescritura de rutas.
- Autenticación, rate limiting, WAF, sticky sessions.

---

## 2. Arquitectura

Desarrollo del diagrama del README:

```
Client ──► [Frontend socket :80/:443] ──► parse HTTP Host header ──► route lookup
        ──► [Backend pool] ──► round-robin ──► upstream connect
        ──► pipe bidireccional ──► close / keep-alive

                    ┌──────── worker N (1 por CPU, SO_REUSEPORT) ────────┐
                    │  accept ─► parse Host ─► router ─► pool ─► upstream│
                    │     ▲                                              │
                    │     └── event loop (epoll/kqueue, edge-triggered) ─┘
                    └────────────────────────────────────────────────────┘
                              ▲ SIGHUP ─► self-pipe ─► swap atómico del router
```

**Invariantes** (los cuatro puntos de «💡 Solución» del README, en forma de regla):

1. **Edge-triggered obliga a drenar.** Con `EPOLLET`/`EV_CLEAR` el kernel avisa
   una sola vez por transición: todo handler lee/escribe en bucle hasta
   `EAGAIN`/`EWOULDBLOCK`. Omitirlo deja conexiones colgadas que no reaparecen
   en el loop — el README lo llama «el corazón didáctico del proyecto».
2. **Portabilidad por abstracción.** `io_event.h` define la API;
   `io_event_epoll.c` y `io_event_kqueue.c` la implementan; Meson detecta la
   plataforma. Ningún otro fichero sabe en qué SO corre.
3. **Reload sin downtime.** La config publicada es inmutable: recargar es
   construir un objeto nuevo y sustituir el puntero (§5), nunca mutar el que
   está en uso.
4. **Nada bloqueante en el loop.** Ni `read`/`write` bloqueantes, ni
   `getaddrinfo`, ni escritura a disco (va al ring buffer, E15), ni `malloc` en
   el camino caliente (va al `buffer_pool`, E16). Las sondas de salud viven en
   su propio hilo (E11).

### 2.1 Módulos — la tabla del README con su API

| Módulo | Responsabilidad (README) | API pública esencial |
|--------|--------------------------|----------------------|
| `io_event` | Abstracción epoll/kqueue | `io_loop_create/destroy`, `io_loop_add/mod/del(fd,mask,ctx)`, `io_loop_run/stop` |
| `listener` | Accept loop por puerto frontal, `SO_REUSEPORT` | `listener_open(frontend*)`, `listener_on_readable()` |
| `connection` | Máquina de estados cliente↔upstream, arena de conexiones | `conn_acquire/release`, `conn_on_event` |
| `http_parser` | Extrae `Host:`; inyecta `X-Forwarded-For/-Proto`, `X-Real-IP` | `hp_init`, `hp_execute(buf,len) → NEED_MORE\|DONE\|ERROR` |
| `router` | djb2 para dominios exactos, wildcards `*.dom`, default, refcount RCU | `router_build(config*)`, `router_lookup(host) → pool*`, `router_ref/unref` |
| `backend_pool` | `round_robin`/`weighted`/`least_conn` + health pasivo | `pool_pick() → backend*`, `pool_report(backend*, ok)` |
| `health` | Sondas TCP/HTTP activas en hilo aparte | `health_start/stop` |
| `config` | Parser TOML (tomlc99), validación, reload | `config_load(path) → config*\|NULL+err`, `config_free` |
| `log` | Ring buffer 4096×512 B con hilo consumidor | `log_init`, `log_write(level, fmt, ...)`, `log_flush` |
| `buffer_pool` | Arena `mmap` de slots de 16 KB con freelist | `bufpool_get/put` |
| `stats` | Socket UNIX con snapshot JSON | `stats_snapshot(fd)` |

### 2.2 Diferencias de plataforma que el diseño debe absorber

`io_event.h` no es la única frontera portable: `listener.[ch]` también lo es.
Estas tres diferencias condicionan la API y hay que resolverlas **al diseñar**,
no al portar:

| Aspecto | Linux | FreeBSD | macOS |
|---|---|---|---|
| Reparto de accept entre workers (E5) | `SO_REUSEPORT` | **`SO_REUSEPORT_LB`** (≥ 12.0) | `SO_REUSEPORT` **sin** reparto de carga |
| Accept no bloqueante | `accept4(SOCK_NONBLOCK)` | `accept4` | no existe ⇒ `accept` + `fcntl(O_NONBLOCK)` |
| Cierre del par detectado | `EPOLLRDHUP` | `EV_EOF` del filtro | `EV_EOF` del filtro |

Consecuencia: **`SO_REUSEPORT` no significa lo mismo en los tres sistemas.** En
macOS varios procesos pueden hacer `bind` al mismo puerto, pero el kernel no
reparte las conexiones. Si E5 depende de ese reparto, macOS necesita un plan B
(un aceptador único que distribuya descriptores, o varios `kqueue` sobre el mismo
socket de escucha). La decisión se documenta en el README, porque afecta a una
afirmación suya.

El compilador base de FreeBSD y macOS es **clang**, no gcc: el código debe
compilar limpio con ambos (E1).

---

## 3. Máquina de estados de una conexión

```
ACCEPTED ─► READING_REQUEST ─► RESOLVING ─► CONNECTING ─► PROXYING ─┬─► CLOSING
                  │                │            │                   │
                  └── parse error ─┴─ no route ─┴─ connect fail ─────┴─► ERROR (4xx/502)
```

- `READING_REQUEST`: acumula hasta `\r\n\r\n`. Límites duros: **8 KB** de
  cabeceras y **30 s** desde el `accept` ⇒ `431`/`408` y cierre.
- `RESOLVING`: `router_lookup(Host)` y `router_ref()` — la conexión **retiene**
  la referencia hasta su cierre (base del RCU de E13).
- `CONNECTING`: `connect` no bloqueante; fallo ⇒ `pool_report(backend, false)`
  (health **pasivo**, E11) y reintento con el siguiente backend sano; si no
  queda ninguno ⇒ **502**.
- `PROXYING`: relay bidireccional, un slot de 16 KB por sentido. Si el buffer de
  salida se llena se **deja de leer** del otro extremo (backpressure) y se
  reactiva con `IO_WRITE`.
- `CLOSING`: `shutdown(SHUT_WR)` al lado que terminó, drenado del otro, `close`
  con ambos a cero. Nunca se cierra sin `io_loop_del`, `bufpool_put` y
  `router_unref`.

**Keep-alive**: HTTP/1.1 sin `Connection: close` vuelve a `READING_REQUEST`. La
conexión upstream se reutiliza **solo si el nuevo `Host` enruta al mismo
backend**; si no, se cierra y se abre otra.

---

## 4. Configuración TOML

### 4.1 Esquema completo

```toml
[global]
workers      = 0          # 0 = una por CPU (SO_REUSEPORT), E5
max_conns    = 65536
stats_socket = "/tmp/proxy-stats.sock"
log_file     = "/var/log/proxy.log"
log_level    = "info"     # debug|info|warn|error

[[frontend]]
name   = "public"
listen = "0.0.0.0:80"

  [[frontend.route]]
  host    = "a.test"       # dominio exacto
  backend = "pool_a"

  [[frontend.route]]
  host    = "*.b.test"     # wildcard
  backend = "pool_b"

  [[frontend.route]]
  host    = "default"      # comodín final; si falta ⇒ 502
  backend = "pool_a"

[[frontend]]
name   = "internal"
listen = "127.0.0.1:8081"

  [[frontend.route]]
  host    = "admin.test"
  backend = "pool_admin"

[[backend]]
name    = "pool_a"
balance = "round_robin"    # round_robin | weighted | least_conn
servers = [
  { addr = "127.0.0.1:9001", weight = 1 },
  { addr = "127.0.0.1:9002", weight = 1 },
  { addr = "127.0.0.1:9003", weight = 1 },
]

  [backend.health]
  type     = "tcp"         # tcp | http
  interval = 2000          # ms
  timeout  = 500           # ms
  rise     = 2             # sondas OK ⇒ UP
  fall     = 3             # sondas KO ⇒ DOWN
  path     = "/healthz"    # solo si type = "http"
```

`proxy.toml` de ejemplo usa `:80` como en el README. Las **fixtures de test**
usan puertos altos (`8080`/`8081`) para correr sin privilegios.

### 4.2 Orden de resolución (E8)

1. `Host` exacto — normalizado: sin `:puerto`, en minúsculas.
2. Wildcard `*.dom` **más específico** (mayor número de etiquetas).
3. Ruta `default` del frontend.
4. Ninguna ⇒ **502 Bad Gateway**, cuerpo mínimo, `Connection: close`.

`Host` ausente, vacío o con bytes fuera de `[a-z0-9.\-]` ⇒ **400**.

### 4.3 Validación (se rechaza la config completa si falla alguna)

- Todo `frontend.route.backend` referencia un `[[backend]]` existente.
- Sin `listen` duplicado, sin `host` duplicado dentro de un frontend, sin
  `backend.name` duplicado.
- Cada backend con ≥ 1 servidor; `weight ≥ 1`; `addr` parsea como `IP:puerto`.
- Wildcards de la forma `*.dominio` (un solo `*`, al principio).
- `interval`, `timeout`, `rise`, `fall` positivos; `timeout < interval`.
- `balance` ∈ {`round_robin`, `weighted`, `least_conn`}.
- Errores con **ruta TOML**: `frontend[1].route[0]: backend "pool_x" no existe`.

---

## 5. Recarga en caliente (E13)

Secuencia exigida — **ninguna variante que cierre listeners o conexiones**:

1. El handler de `SIGHUP` hace **solo** `write()` de 1 byte al *self-pipe*
   (async-signal-safe): ni log, ni `malloc`.
2. El loop despierta por el extremo de lectura del pipe.
3. `config_load()` parsea y **valida** en memoria nueva.
4. **Si falla**: se registra el error y **se conserva la config anterior**. Un
   reload fallido nunca degrada el servicio.
5. **Si valida**: `router_build()` y publicación con **intercambio atómico** del
   puntero (`atomic_exchange`, `memory_order_acq_rel`).
6. El router viejo se libera cuando su **refcount llega a 0**: las conexiones en
   vuelo terminan con la configuración con la que empezaron (RCU).
7. Los `listen` que no cambian **reutilizan el fd**; solo se abren/cierran los
   que aparecen o desaparecen.

**Aceptación**: con `wrk` en marcha, `kill -HUP` no produce ni un error ni una
conexión reseteada en las métricas de `wrk`.

---

## 6. Tests

### 6.1 Unitarios — 22 tests, 4 suites (E19)

El README declara **22 casos cmocka repartidos en 4 suites**. Ojo al contar:
`meson test` reporta **targets**, uno por suite, así que su resumen dirá `4/4`;
los 22 casos se cuentan dentro de cada binario (`--print-errorlogs` los lista).
Reparto fijo:

| Suite | N | Casos |
|-------|---|-------|
| `test_http_parser` | 7 | request completa; partida en 2 trozos; partida byte a byte; sin `Host` ⇒ 400; `Host` con puerto; cabeceras > 8 KB ⇒ 431; `X-Forwarded-For` preexistente ⇒ se **añade**, no se sustituye |
| `test_router` | 6 | exacto gana a wildcard; wildcard más específico gana; `default`; sin ruta ⇒ NULL; normalización (mayúsculas + puerto); colisión de hash djb2 |
| `test_backend_pool` | 5 | round-robin 300/3 = 100 ±1; un backend DOWN ⇒ 150/150; todos DOWN ⇒ NULL; `weighted` respeta 3:1; `least_conn` elige el de menos conexiones |
| `test_config` | 4 | fixture válido completo; backend inexistente; `listen` duplicado; wildcard mal formado (cada caso comprueba además que el mensaje cita la ruta TOML) |

**Estado actual**: `test_config` (4) y `test_router` (6) ya están completas según
la tabla. Se les suma `test_io_event` (5 casos), que no entra en la cuenta de 22
porque el README lista `io_event` como módulo pero no como suite; verifica la API
común en las tres plataformas y es la prueba viva de E3.

**Sin cubrir todavía**: `router_slot` (el swap atómico con refcount). Su test
llega con el reload de E13, que es cuando se puede comprobar de punta a punta
que las conexiones en vuelo terminan con la config vieja.

Si un cambio necesita un test nuevo, **se sustituye o se amplía manteniendo el
recuento declarado en el README actualizado en el mismo commit**. Todo bug
corregido añade primero el test que lo reproduce.

### 6.2 End-to-end — `tests/e2e/run_e2e.sh` (R9)

Implementa los tres pasos del enunciado:

1. **Generar configuración**: escribe un `proxy.toml` temporal con 2 frontends
   (`:8080`, `:8081`), 3 dominios y un pool de 3 servidores.
2. **Lanzar servidores de servicio**: N instancias de `echo_server` en
   `127.0.0.1:9001..9003`; cada una responde `200` con un cuerpo que contiene su
   identificador (`server-9001`, …) y refleja las cabeceras recibidas. Eso es lo
   que permite verificar el reparto y las `X-Forwarded-*` desde fuera.
3. **Dar de alta dominios en `/etc/hosts`**: `127.0.0.1 a.test`,
   `127.0.0.1 x.b.test`, `127.0.0.1 admin.test`. Con backup en
   `/etc/hosts.proxy.bak` y `trap EXIT` que **siempre** restaura. Sin privilegios,
   `--no-hosts` valida lo mismo con `curl --resolve`.

**Aserciones** (cada una cita el requisito que cubre):

- `curl http://a.test:8080/` ⇒ 200, cuerpo de un servidor del pool — E7.
- 300 peticiones a `a.test` ⇒ los 3 identificadores, 100 ±1 cada uno — E9.
- `curl http://x.b.test:8080/` ⇒ pool distinto al de `a.test` — E8.
- `admin.test:8081` responde; `admin.test:8080` **no** llega a `pool_admin` — E6.
- `Host` sin ruta y sin `default` ⇒ **502** — E8.
- Un backend muerto ⇒ siguen los 200 repartidos entre los dos vivos — E10, E11.
- Nuevo TOML + `kill -HUP` ⇒ nuevo enrutado aplicado, 0 errores de `curl` — E13.
- El upstream ve `X-Forwarded-For` con la IP del cliente — E14.
- `socat - UNIX-CONNECT:$stats_socket` ⇒ JSON válido con uptime, contadores y
  estado de backends — E17.

Sale con 0 solo si **todas** pasan; imprime un resumen por aserción; es
idempotente (limpia procesos, temporales y `/etc/hosts` en `trap EXIT`).

---

## 7. Benchmark — `bench/bench_proxy.sh` (E2, E20)

Tres escenarios de `wrk`. Referencia del README (macOS/ARM64, build release):

| Configuración | Req/s | Latencia media | Errores |
|---|---|---|---|
| `-t4 -c400 -d30s` | 54.183 | 7,39 ms | 0 |
| `-t4 -c200 -d30s` | 52.657 | 3,90 ms | 0 |
| `-t2 -c100 -d30s` | 55.311 | 1,84 ms | 0 |

**Umbral de aprobado: ≥ 50.000 req/s y 0 errores en los tres.** Esas cifras son
el suelo a no empeorar. El script sube `ulimit -n 65535`, verifica que corre
contra el build release y avisa si el backend de prueba es el cuello de botella.

### 7.1 Cómo se reportan las cifras — referencia vs. propias

Las cifras de arriba son **de referencia**: las publica el README, medidas en
macOS/ARM64 en hardware que no tenemos. Este proyecto se mide en Linux/VPS, así
que **nunca se presentan como propias**. El informe de resultados lleva siempre
las dos tablas, separadas y etiquetadas:

```markdown
### Referencia (README del enunciado) — macOS/ARM64
| Configuración | Req/s | Latencia media | Errores |

### Medición propia — <SO y versión> · <CPU> · <N núcleos> · <RAM>
| Configuración | Req/s | Latencia media | Errores |
```

Cada medición propia declara **SO y versión, modelo de CPU, número de núcleos
(dedicados o compartidos) y RAM**, más el `buildtype` y si `wrk`, el proxy y los
backends compartían máquina. Sin esos datos un número de req/s no significa nada.

Si el hardware disponible no llega al umbral, **se publica la cifra real igualmente**
junto al contexto que la explica: la medición es del entorno, no del código. Una
medida baja y bien documentada es un resultado; una cifra inflada o heredada de
otro hardware es un error de honestidad. El equipo local (§`requerimiento.md` §5.4:
Ryzen 5 3400G, 4 núcleos compartidos con Windows) se etiqueta explícitamente como
**orientativo**, nunca como medición de entrega.

---

## 8. Estructura del repositorio

```
.
├── meson.build              # proyecto raíz, detección de plataforma
├── meson_options.txt        # -Dwith_tests (los sanitizadores usan -Db_sanitize)
├── CLAUDE.md                # este documento
├── README.md                # documento de evaluación
├── proxy.toml               # config de ejemplo
├── .github/workflows/ci.yml # Linux + macOS ARM64 + ASan/UBSan en cada push
├── src/
│   ├── meson.build
│   ├── main.c               # CLI, fork de workers, señales, self-pipe
│   ├── io_event.h           # API común (E3)
│   ├── io_event_internal.h  # tabla de observadores, sin plataforma
│   ├── io_event_common.c    # parte neutral: tabla + O_NONBLOCK
│   ├── io_event_epoll.c     # solo Linux
│   ├── io_event_kqueue.c    # solo macOS/BSD
│   ├── listener.[ch]        ├── connection.[ch]
│   ├── http_parser.[ch]     ├── router.[ch]
│   ├── backend_pool.[ch]    ├── health.[ch]
│   ├── config.[ch]          ├── log.[ch]
│   ├── buffer_pool.[ch]     └── stats.[ch]
├── subprojects/tomlc99.wrap
├── tests/
│   ├── meson.build
│   ├── test_http_parser.c  test_router.c  test_backend_pool.c  test_config.c
│   └── e2e/{run_e2e.sh, echo_server.c, fixtures/*.toml}
└── bench/bench_proxy.sh
```

El binario queda en **`build/src/proxy`**, como indica el README.

### 8.1 Meson (E18)

- `project('proxy','c', default_options:['c_std=c11','warning_level=3','werror=true'])`.
- Backend de I/O por `host_machine.system()`: `linux → io_event_epoll.c`;
  `darwin`/`freebsd` → `io_event_kqueue.c`; otro ⇒ `error('plataforma no soportada')`.
- `tomlc99` y `cmocka` vía `.wrap` + `dependency(..., fallback:)`, para que el
  build funcione sin red si ya están en el sistema.
- `-Db_sanitize=address,undefined` (opción propia de Meson) para depuración. Release: `-O2 -flto`; nunca
  `-Ofast` ni `-march=native`.

---

## 9. Convenciones de código

- **C11 estricto**; extensiones GNU solo dentro de `io_event_*.c`.
- Sin warnings con `-Wall -Wextra -Wpedantic -Werror`.
- `snake_case`; funciones públicas prefijadas por módulo (`router_lookup`,
  `pool_pick`); `static` todo lo que no esté en el `.h`.
- Un `.h` por módulo, guard `PROXY_<MOD>_H`; cada `.c` incluye lo que usa.
- Errores: `int` (`0` OK / `-1` error) o puntero `NULL`; `errno` preservado hasta
  el log. **Nunca `exit()` fuera de `main.c`.**
- Toda ruta de error libera lo adquirido: `bufpool_put`, `io_loop_del` + `close`,
  `router_unref`.
- Sin globales mutables salvo el puntero atómico al router y el self-pipe.
- En handlers de señal, solo funciones async-signal-safe (`write`, `_exit`).
- El build de depuración pasa limpio bajo ASan/UBSan tras ejecutar el e2e.

---

## 10. Comandos (los del README, más los de desarrollo)

```bash
# Build (README)
meson setup build && meson compile -C build

# Tests — 22 tests cmocka, 4 suites (README)
meson test -C build

# Ejecutar con una config TOML (README)
./build/src/proxy -c proxy.toml

# Recargar configuración sin reiniciar (README)
kill -HUP <pid>

# Benchmark (README)
./bench/bench_proxy.sh

# --- desarrollo ---
meson setup build-asan -Db_sanitize=address,undefined && meson compile -C build-asan
meson setup build-rel --buildtype=release && meson compile -C build-rel   # para el benchmark
meson test -C build --print-errorlogs
sudo ./tests/e2e/run_e2e.sh          # con /etc/hosts
./tests/e2e/run_e2e.sh --no-hosts    # con curl --resolve
socat - UNIX-CONNECT:/tmp/proxy-stats.sock
```

---

## 11. Definición de «hecho»

Un cambio está terminado cuando:

1. `meson compile` sin warnings en **Linux, FreeBSD y macOS** — los tres con el CI
   en verde, no de palabra (E3). Ojo: gcc en Linux, **clang** en los otros dos.
2. `meson test` reporta **22/22** en verde en las tres plataformas (E19).
3. `run_e2e.sh` en verde, con y sin `/etc/hosts` (R9).
4. Build ASan/UBSan sin hallazgos tras ejecutar el e2e.
5. El benchmark mantiene **≥ 50.000 req/s y 0 errores** en los tres escenarios (E2).
6. Si el cambio toca la configuración, `proxy.toml` y §4 quedan actualizados en el
   mismo commit; si toca algo que el README afirma (cifras, módulos, recuento de
   tests), **el `README.md` se actualiza en ese mismo commit** — es el documento
   de evaluación y no puede quedar desmentido por el código.
