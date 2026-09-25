# Proxy inverso L7 en C11 — resumen del trabajo

Documento de exposición. Qué se construyó, por qué se decidió así, qué quedó
fuera a propósito y cómo se verificó.

- **Código**: ~8.000 líneas (4.760 de `src/`), 22 commits.
- **Repositorios**: entrega en el GitLab de la academia; espejo público en
  `github.com/eduxworks/proxy-epoll-kqueue` para el CI de macOS.
- **Estado**: los 20 requisitos del README (E1–E20) y los 9 del enunciado
  (R1–R9), completos y verificados.

---

## 1. Qué hace

Un proxy inverso HTTP asíncrono que escucha en varios puertos, decide a qué
grupo de servidores mandar cada petición según el header `Host:`, reparte entre
ellos, y recarga su configuración sin cortar el tráfico.

```
Cliente ──► :80 / :8081 ──► parsea Host ──► router ──► pool ──► upstream
                │                                                   │
                └──────────── relay bidireccional ◄─────────────────┘
                     ▲ SIGHUP ─► swap atómico del router
```

Un proceso por CPU, cada uno con su bucle de eventos, sin estado compartido
entre ellos: nada de cerrojos en el camino caliente.

---

### Diagramas

`epoll_queue.drawio` (en el repositorio) tiene cuatro páginas, pensadas para
exponer en ese orden:

| Página | Qué muestra |
|---|---|
| **0 · Boceto inicial** | El punto de partida: un «PROXY NGINX» con dos `localhost`. Se conserva porque el contraste con la página 1 es parte de la historia. |
| **1 · Arquitectura** | El recorrido completo: clientes → listeners con `SO_REUSEPORT` → worker (bucle + dos hilos) → router RCU → pools → backends. Señala en color los **tres únicos ficheros con `#ifdef`** y advierte de que `SO_REUSEPORT` no significa lo mismo en los tres sistemas. |
| **2 · Estados de conexión** | Las cuatro transiciones, los caminos de error con su código HTTP, y el bucle de keep-alive que vuelve a `READING_REQUEST` sin cerrar nada. Incluye los dos invariantes y la tabla de delimitación de cuerpos, que es lo que hace ese bucle posible. |
| **3 · Recarga en caliente** | Los cinco pasos de `SIGHUP` al intercambio atómico, el camino de fallo que conserva la configuración anterior, y el router viejo sostenido por las conexiones en vuelo hasta que la última lo libera. |

Los colores significan lo mismo en las tres páginas nuevas: naranja para lo
específico de plataforma, azul para el camino de datos, verde para enrutado y
destinos, morado para los hilos ajenos al bucle, amarillo para configuración.

---

## 2. Los módulos

| Módulo | Líneas | Qué resuelve |
|---|---|---|
| `connection` | 820 | Máquina de estados cliente↔upstream, relay, keep-alive |
| `config` | 600 | TOML con tomlc99, validación estricta |
| `http_parser` | 550 | Cabeceras, `X-Forwarded-*`, delimitación de mensajes |
| `main` | 478 | Workers, señales, recarga |
| `router` | 363 | `Host` → pool, con publicación RCU |
| `health` | 312 | Sondas activas en hilo aparte |
| `backend_pool` | 290 | round_robin / weighted / least_conn, salud pasiva |
| `stats` | 285 | Snapshot JSON por socket UNIX |
| `log` | 245 | Anillo 4096×512 B + hilo consumidor |
| `io_event_epoll` / `_kqueue` | 202 / 199 | La misma API sobre dos kernels |
| `listener` | 172 | Accept con `SO_REUSEPORT` |
| `buffer_pool` | 106 | Arena `mmap` de slots de 16 KB |

---

## 3. Las cinco decisiones que definen el diseño

### 3.1 Edge-triggered obliga a drenar

Con `EPOLLET`/`EV_CLEAR` el kernel avisa **una sola vez** por transición. Todo
handler lee y escribe en bucle hasta `EAGAIN`. Olvidarlo deja conexiones
colgadas que no vuelven a despertar, y el fallo no aparece hasta que hay carga.

Un caso concreto: el EOF se informa **aparte** del número de bytes leídos. Si
viajara en el valor de retorno se perdería cuando llega en la misma lectura que
los últimos datos — y esa notificación no se repite.

### 3.2 Portabilidad por abstracción, en tres ficheros y no más

`io_event.h` define la API; `io_event_epoll.c` e `io_event_kqueue.c` la
implementan. Al llegar al `listener` apareció una segunda familia de
diferencias, la de los sockets, que no son del bucle de eventos: viven en
`net_compat.c`. Esos **tres ficheros** son los únicos con `#ifdef` de
plataforma.

La diferencia más interesante: `SO_REUSEPORT` **no significa lo mismo** en los
tres sistemas.

| | Linux | FreeBSD | macOS |
|---|---|---|---|
| Reparto entre workers | `SO_REUSEPORT` | `SO_REUSEPORT_LB` | **no reparte** |
| Accept no bloqueante | `accept4` | `accept4` | no existe |

Por eso el arranque **consulta en ejecución** si el kernel reparte
(`net_reuseport_balances()`) y en macOS levanta un solo worker diciendo por qué,
en vez de fingir paralelismo.

### 3.3 Recargar es publicar, no mutar

`SIGHUP` escribe un byte en un *self-pipe* —lo único async-signal-safe— y el
bucle despierta. Se parsea y valida en memoria nueva; si algo falla **se
conserva la configuración anterior**, porque un reload roto no puede degradar el
servicio.

Si valida, se publica con un intercambio atómico. Cada conexión toma una
referencia al router al nacer y la suelta al morir, así que **termina con la
configuración con la que empezó**. Los pools viven dentro del router: así el
mismo refcount que protege la tabla de rutas protege el estado de salud al que
apunta.

### 3.4 Sin saber dónde acaba un mensaje no hay keep-alive

Reutilizar una conexión exige delimitar cada respuesta; contar mal sirve media
respuesta como si fuera la siguiente.

| Delimitador | Fin del cuerpo | ¿Reutilizable? |
|---|---|---|
| `Content-Length: N` | tras N bytes | sí |
| `chunked` | chunk de tamaño 0 + tráiler | sí |
| 1xx, 204, 304, respuesta a `HEAD` | sin cuerpo, aunque declare longitud | sí |
| ninguno | el cierre del socket | **no** |

Dos reglas de seguridad salen de aquí: `chunked` manda sobre `Content-Length` si
vienen los dos, y un `Content-Length` con basura es `400` en vez de
interpretarse «a lo que parezca». De las discrepancias entre quien delimita
distinto salen las peticiones coladas dentro de otras.

`Connection` es cabecera **de salto a salto**: describe el enlace por el que
llegó, no el siguiente tramo. Se descarta y el proxy emite la suya; si no, un
cliente pidiendo cerrar *su* conexión haría cerrar la del proxy con el backend.

### 3.5 Nada bloqueante en el bucle

Ni escritura a disco (va al anillo del registro), ni `malloc` en el camino
caliente (va a la arena de `mmap`), ni esperas de sondeo (viven en su hilo).

La salud pasiva por sí sola tiene un agujero que conviene entender: **no puede
readmitir a nadie**. Si los miembros de un pool caen todos, deja de haber a
quién enrutar, y sin intentos no hay aciertos que devuelvan a nadie al reparto —
el pool quedaría muerto para siempre. Las sondas activas cierran ese ciclo.

---

## 4. Verificación

### 4.1 Pruebas

**11 binarios, 54 casos.** Los 22 que declara el README, en sus 4 suites
(`config` 4, `router` 6, `http_parser` 7, `backend_pool` 5), más 32 en 7 suites
propias para módulos que el README lista pero no enumera como suites.

**End-to-end (`run_e2e.sh`)**: 20 aserciones, cada una citando el requisito que
cubre. Implementa los tres pasos del enunciado —generar configuración, lanzar
servidores, dar de alta dominios en `/etc/hosts`— con copia de seguridad y
`trap EXIT` que restaura siempre. Sin privilegios, `--no-hosts` valida lo mismo
con `curl --resolve`.

### 4.2 Integración continua

Cada push compila y prueba en **Linux (gcc)**, **macOS ARM64 (clang)** y bajo
**ASan/UBSan**, con el e2e completo en las dos plataformas. `ThreadSanitizer`
limpio en los dos módulos con hilos.

Esto es lo que convierte E3 de afirmación en evidencia: no es que el código
«esté preparado» para macOS, es que el proxy **mueve tráfico real sobre
`kqueue`** en cada commit.

### 4.3 Rendimiento

Intel i7-11800H · 8 vCPU · 7,7 GB · build release · `wrk` 4.1.0

| Escenario | Req/s | Latencia | Errores |
|---|---|---|---|
| `-t4 -c400 -d30s` | **345.120** | 1,10 ms | 0 |
| `-t4 -c200 -d30s` | **312.288** | 609 µs | 0 |
| `-t2 -c100 -d30s` | **246.254** | 366 µs | 0 |

Meta de 50.000 req/s superada por 5–7×. Son las cifras de la **ejecución más
conservadora de dos** (la otra dio 382.786 / 346.581 / 253.741): publicar la
mejor sin decirlo no sería una medición, sería una selección.

Las de referencia del enunciado (54.183 / 52.657 / 55.311, macOS/ARM64) se
mantienen aparte y marcadas como **no reproducidas**: son de otro hardware.

---

## 5. Limitaciones, y por qué se dejaron así

Todas están documentadas en `CLAUDE.md` y visibles en el código.

| Limitación | Por qué | Qué costaría cerrarla |
|---|---|---|
| **La recarga no admite cambiar puertos ni número de frontends** | Los listeners ya están abiertos y las conexiones vivas se enrutan por índice de frontend; si la lista cambia, ese índice pasa a significar otra cosa. Se **rechaza con un mensaje** en vez de enrutar mal. | Abrir y cerrar sockets en caliente y reindexar las conexiones en vuelo. Medio día. |
| **`stats` solo lo publica el worker 0** y sus cifras son las suyas, no la suma | Un socket UNIX no se comparte como un puerto con `SO_REUSEPORT`. | Memoria compartida entre procesos — justo lo que el diseño evita para no tener contención. |
| **Sin pipelining** | Mientras se espera una respuesta no se lee del cliente, así que la petición siguiente espera en el socket. Ningún cliente real lo usa. | Casar respuestas con peticiones fuera de orden. Complejidad alta, beneficio nulo. |
| **Sin terminación TLS** | El README dibuja `:443` como puerto de escucha pero no menciona certificados ni handshake en ningún sitio. | Cambio de alcance, no un detalle. |

Ninguna es un descuido: en los tres primeros casos el código **detecta la
situación y actúa de forma segura** en lugar de comportarse de forma
impredecible.

**Cerrada durante la revisión**: el cuerpo troceado *de la petición* sí se
delimita ahora, con el mismo escáner que ya usaba la respuesta. Era la única de
las cinco cuyo coste (un par de horas, reutilizando un patrón ya probado) no
justificaba dejarla abierta. El e2e comprueba que un POST troceado llega al
backend **y que la conexión se reutiliza después**, que es lo que antes no podía
ocurrir.

---

## 6. Lo que enseñó el proceso

Tres episodios que cambiaron el resultado más que cualquier decisión de diseño.

**El CI de macOS pagó su coste tres veces.** Compilando limpio en Linux con gcc
y clang, macOS falló por: cmocka 2.x usando `##__VA_ARGS__` (extensión GNU que
clang rechaza), la misma cmocka deprecando `assert_in_range` sin que su relevo
existiera en la versión de Ubuntu, y las diferencias de `SO_REUSEPORT`. Nada de
eso se ve en local.

**El benchmark midió otra cosa durante una ejecución entera.** Un Jetty
escuchando en el 8080 hizo que `wrk` lo midiera a él: el informe salió con
28.715 req/s y 863.144 «errores del proxy» que eran 403 ajenos, presentados con
tabla, CPU y RAM como una medición seria. Y el propio medidor los ocultaba,
porque anclaba `Non-2xx or 3xx responses` a principio de línea cuando `wrk` la
indenta. Ahora comprueba que el puerto esté libre, que el `bind` no haya
fallado, y guarda la salida cruda.

**La medida de control distingue el código del entorno.** Medir los backends a
pelo antes que el proxy es lo que convirtió un «hay 48 errores, algo falla en el
keep-alive» en «el entorno produce 197 errores él solo». Sin ella habría
arreglado un bug inexistente — de hecho empecé a hacerlo.

El patrón se repite: **los tests y los medidores fallan igual que el código, y
sus fallos son más caros porque parecen resultados.**

---

## 7. Cómo ejecutarlo

```bash
meson setup build && meson compile -C build     # build
meson test -C build                             # 11 suites, 54 casos
./build/src/proxy -c proxy.toml                 # ejecutar
kill -HUP <pid>                                 # recargar sin cortar

sudo ./tests/e2e/run_e2e.sh                     # e2e con /etc/hosts
./tests/e2e/run_e2e.sh --no-hosts               # sin privilegios

meson setup build-rel --buildtype=release && meson compile -C build-rel
./bench/bench_proxy.sh                          # benchmark

socat - UNIX-CONNECT:/tmp/proxy-stats.sock      # estadísticas
```

**Documentos del repositorio**

| Fichero | Para qué |
|---|---|
| `README.md` | Presentación y cifras. Es el documento contra el que se evalúa. |
| `CLAUDE.md` | Especificación: criterios de aceptación de E1–E20, contratos de módulo, esquema TOML, reparto de pruebas. |
| `requerimiento.md` | Entorno de desarrollo: qué hace falta, por qué, y qué no se puede hacer desde Windows. |
| `epoll_queue.drawio` | Los cuatro diagramas descritos en §1. |
| `resumen.md` | Este documento. |
