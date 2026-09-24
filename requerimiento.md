# requerimiento.md — Entorno de desarrollo necesario

Qué hace falta para desarrollar, probar y medir este proyecto, y qué parte puede
hacerse desde Windows. Referencias `E1–E20` = requisitos de [`CLAUDE.md`](CLAUDE.md) §1.

---

## 0. Respuesta corta

**No puedes compilar ni ejecutar este proyecto en Windows nativo, pero sí puedes
trabajar íntegramente *desde* Windows.**

`epoll` es una llamada al sistema exclusiva de Linux y `kqueue` de BSD/macOS.
Windows no tiene ninguna de las dos (su equivalente es IOCP, que el README no
contempla). Tampoco existen `SO_REUSEPORT` con la semántica de Linux, `mmap`,
`fork` de workers ni `SIGHUP` — es decir, E3, E4, E5, E13 y E16 son imposibles
de satisfacer en Win32.

Lo viable y recomendado:

- **Windows** = editor, git, terminal. No compila nada.
- **WSL2 (Ubuntu)** o **Docker** = compilar, tests unitarios y e2e. Es donde
  vivirás el 95 % del tiempo.
- **VPS Linux** = benchmark oficial (E2, ≥ 50.000 req/s). Tu equipo local no sirve
  para medir: 4 núcleos físicos compartidos con Windows (§5.4).
- **FreeBSD en VirtualBox** + **GitHub Actions en `macos-14`** = la rama `kqueue`
  (E3) sin comprar un mac: se desarrolla en la VM y se confirma en macOS ARM64
  real en cada push (§7). Docker no sirve aquí: un contenedor usa el kernel Linux
  del anfitrión.

---

## 1. Reparto de máquinas

| Tarea | Windows nativo | WSL2 / Docker | VM VirtualBox | VPS Linux | FreeBSD (VBox) | CI `macos-14` |
|---|---|---|---|---|---|---|
| Editar código, git, documentación | ✅ | ✅ | ✅ | ✅ | — | — |
| Compilar (`meson compile`) | ❌ | ✅ | ✅ | ✅ | ✅ (kqueue) | ✅ (kqueue) |
| Tests unitarios cmocka (E19) | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Test e2e con `/etc/hosts` (R9) | ❌ | ✅ | ✅ | ✅ | ✅ | ⚠️ posible |
| `epoll`, `SO_REUSEPORT`, `SIGHUP` (E3–E5, E13) | ❌ | ✅ | ✅ | ✅ | — | — |
| **`kqueue` (E3)** | ❌ | ❌ | ❌ | ❌ | ✅ **desarrollo** | ✅ **macOS real** |
| Benchmark ≥ 50k req/s (E2) | ❌ | ⚠️ orientativo | ❌ | ✅ **oficial** | ❌ | ❌ |
| ASan/UBSan | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ |

**Decisión tomada para E3**: `io_event_kqueue.c` se desarrolla en una **VM FreeBSD**
(ciclo rápido) y se confirma en **macOS ARM64 real vía GitHub Actions** (§7). Nada
de *mocks*. Docker **no** puede ejecutar FreeBSD ni macOS: un contenedor comparte
el kernel del anfitrión, que es Linux, y `kqueue` es una llamada del kernel BSD.

⚠️ **Ni WSL2, ni Docker, ni una VM valen para el benchmark evaluable** en esta
máquina: los tres comparten los mismos 4 núcleos físicos con Windows. Sirven para
ver si un cambio mejora o empeora; la cifra que se entrega sale del VPS (§5).

---

## 2. Estado actual de tu máquina (detectado el 2026-09-23)

| Componente | Estado | Acción |
|---|---|---|
| Windows 11 Pro 22631 | ✅ | — |
| CPU: AMD Ryzen 5 3400G — **4 núcleos físicos / 8 lógicos** | ⚠️ justo | Suficiente para desarrollar; insuficiente para la cifra de E2 (§5) |
| RAM 32 GB (21,6 GB libres) | ✅ de sobra | Permite VM FreeBSD + WSL sin apreturas |
| Hipervisor de Windows activo (`HypervisorPresent=True`) | ⚠️ | Lo activan WSL2/Docker Desktop. **Degrada VirtualBox** (§7.1) |
| VirtualBox 7.2.6 | ✅ | Se reutiliza para **FreeBSD** (§7), no para el benchmark |
| VM `ubuntu24` (1 vCPU, 2 GB, apagada) | ⚠️ infradimensionada | No sirve para benchmark (§5.4); puede borrarse o reaprovecharse |
| GitHub CLI 2.92, autenticado como `eduxworks` (scopes `repo`, `workflow`) | ✅ listo | Habilita el CI de macOS sin configuración extra (§7.7) |
| WSL 2.4.10, kernel 5.15.167 | ✅ instalado | — |
| Distros WSL | ⚠️ solo `docker-desktop` (interna, no sirve para desarrollar) | **Instalar Ubuntu** (§3) |
| Docker Desktop | ✅ | Opcional, alternativa a WSL (§4) |
| Git 2.x | ✅ `C:\Program Files\Git` | Configurar fin de línea (§3.3) |
| VS Code | ✅ | Instalar extensión Remote - WSL |
| OpenSSH client | ✅ | Para el VPS |
| Virtualización en firmware | ✅ habilitada | Permite WSL2 y Hyper-V |
| gcc / meson / ninja en Windows | ❌ | **No hacen falta** — no se compila en Windows |
| Python | ✅ (Store) | Meson vendrá de `apt`, no de aquí |

---

## 3. Windows: preparación (≈ 15 min)

### 3.1 Instalar Ubuntu en WSL2

```powershell
wsl --install -d Ubuntu-24.04
wsl --set-default Ubuntu-24.04
wsl -l -v            # comprobar: Ubuntu-24.04  Running  2
```

### 3.2 Dónde vivirá el repositorio — importante

El proyecto está hoy en `D:\pry-miseia\...` (NTFS). **No compiles ahí desde WSL**
(`/mnt/d/...`): el I/O entre WSL y NTFS es 10–20× más lento y NTFS no guarda el
bit de ejecución, así que `run_e2e.sh` y `bench_proxy.sh` perderán el `chmod +x`.

Mueve el repo al sistema de ficheros de WSL:

```bash
mkdir -p ~/proyectos && cd ~/proyectos
git clone /mnt/d/pry-miseia/ej1.6.10_20_proxy_en_c/1.5.10-proxy-epoll-kqueue proxy
cd proxy
```

Desde Windows lo abres con `code .` dentro de WSL, o navegando a
`\\wsl$\Ubuntu-24.04\home\<usuario>\proyectos\proxy`. Editas en VS Code (Windows),
compila Linux: el flujo normal.

### 3.3 Fin de línea — evita el fallo más tonto

Un `.sh` guardado con CRLF da `bad interpreter: /bin/bash^M` en Linux.

```bash
git config --global core.autocrlf input
printf '* text=auto eol=lf\n*.sh text eol=lf\n' > .gitattributes
```

### 3.4 Extensiones de VS Code

- **WSL** (`ms-vscode-remote.remote-wsl`) — abre el proyecto dentro de Linux.
- **C/C++** (`ms-vscode.cpptools`) — IntelliSense; apúntalo al
  `build/compile_commands.json` que genera Meson.
- **Even Better TOML** — para `proxy.toml` y las fixtures.
- **Remote - SSH** — para trabajar contra el VPS.

---

## 4. Software en Linux (WSL2 y VPS: lo mismo)

### 4.1 Paquetes obligatorios

```bash
sudo apt update && sudo apt install -y \
  build-essential      `# gcc, make, libc6-dev` \
  meson ninja-build    `# E18: sistema de build` \
  pkg-config \
  git \
  libcmocka-dev        `# E19: 22 tests en 4 suites` \
  python3 python3-pip  `# meson lo necesita` \
  curl socat jq        `# aserciones del e2e: rutas, stats JSON (E17)` \
  net-tools iproute2   `# ss/netstat para verificar SO_REUSEPORT` \
  gdb valgrind         `# depuración` \
  clang clang-format   `# ASan/UBSan alternativo y formato`
```

Comprobación: `meson --version` (≥ 0.60), `gcc --version` (≥ 11, para C11 sólido),
`pkg-config --modversion cmocka`.

### 4.2 `wrk` — generador de carga (E2, E20)

Ubuntu 24.04 lo trae en *universe* (4.1.0), que sirve de sobra:

```bash
sudo apt install -y wrk
wrk --version
```

Si tu distribución no lo tiene, o quieres la 4.2.0, se compila. Ojo a `unzip`:
el Makefile de wrk descomprime LuaJIT y sin él falla con un `Error 127` que no
dice de qué se queja.

```bash
sudo apt install -y build-essential libssl-dev unzip git
git clone https://github.com/wg/wrk.git ~/tools/wrk
make -C ~/tools/wrk -j"$(nproc)"
sudo install ~/tools/wrk/wrk /usr/local/bin/
```

En FreeBSD y macOS: `pkg install wrk` y `brew install wrk`.

### 4.3 `tomlc99` — parser de configuración (E12)

Se integra como **subproyecto de Meson**, no como paquete del sistema:

```bash
mkdir -p subprojects
cat > subprojects/tomlc99.wrap <<'EOF'
[wrap-git]
url = https://github.com/cktan/tomlc99.git
revision = master
depth = 1
EOF
```

La primera compilación necesita red. Si el VPS o el tribunal no la tienen, haz
`meson subprojects download` una vez y **versiona `subprojects/tomlc99/`** para
que el build sea reproducible sin conexión.

### 4.4 Límite de descriptores (E2)

Sin esto no pasas de unos pocos miles de conexiones:

```bash
ulimit -n                                   # suele ser 1024
echo '* soft nofile 65535' | sudo tee -a /etc/security/limits.conf
echo '* hard nofile 65535' | sudo tee -a /etc/security/limits.conf
# nueva sesión; el propio bench_proxy.sh debe hacer ulimit -n 65535
```

---

## 5. VPS Linux — para el benchmark evaluable

### 5.1 Dimensionamiento mínimo

`bench/bench_proxy.sh` **mide primero los backends a pelo** y compara. Esa
medida de control es lo que distingue «el proxy da 30.000 req/s» de «esta
máquina da 30.000 req/s»: si las dos cifras se parecen, lo que estás midiendo
es el hardware, y el script lo dice en vez de dejar que parezca un resultado
del código.

El benchmark corre `wrk` + el proxy + 3 backends **en la misma máquina**. Para
sostener ≥ 50.000 req/s con `-t4 -c400`:

| Recurso | Mínimo | Recomendado | Por qué |
|---|---|---|---|
| vCPU | 4 | **8 dedicadas** | `wrk` usa 4 hilos; el proxy arranca 1 worker/CPU (E5); los backends también consumen |
| RAM | 2 GB | 4 GB | 65.536 conexiones × buffers de 16 KB (E16) |
| Disco | 10 GB | 20 GB SSD | Toolchain + logs |
| Distro | Ubuntu 22.04/24.04 LTS o Debian 12 | igual que WSL | Mismo entorno, menos sorpresas |
| Tipo | — | **CPU dedicada**, no «burstable» | Un VPS compartido tipo t2/t3 da cifras que varían 30 % entre ejecuciones |

⚠️ **Con 1 o 2 vCPU no vas a alcanzar 50.000 req/s por mucho que optimices** — el
cuello es la máquina, no el código. Si tu VPS es pequeño, mídelo igualmente,
documenta las CPU disponibles junto a la cifra, y busca un servidor de 4–8 núcleos
para la medición final.

### 5.2 Tuning del kernel (obligatorio para la cifra)

```bash
sudo tee /etc/sysctl.d/99-proxy-bench.conf <<'EOF'
net.core.somaxconn = 65535
net.core.netdev_max_backlog = 65535
net.ipv4.tcp_max_syn_backlog = 65535
net.ipv4.ip_local_port_range = 10240 65535
net.ipv4.tcp_tw_reuse = 1
net.ipv4.tcp_fin_timeout = 15
fs.file-max = 2097152
EOF
sudo sysctl --system
```

Sin `ip_local_port_range` ampliado te quedas sin puertos efímeros a mitad del
test de 30 s y `wrk` reportará errores de conexión — que cuentan como fallo de E2.

### 5.3 Acceso y seguridad

- Usuario no root con `sudo`; login por **clave SSH**, `PasswordAuthentication no`.
- Desde Windows: `ssh-keygen -t ed25519`, `ssh-copy-id usuario@vps` (o pegar la
  clave en `~/.ssh/authorized_keys`), y un alias en `~/.ssh/config`.
- Firewall: abre solo SSH al exterior. Los puertos del proxy (8080/8081) y de los
  backends (9001-9003) se prueban **desde el propio VPS** vía `127.0.0.1`; no hace
  falta exponerlos.
- VS Code **Remote - SSH** te da el mismo flujo de edición que en WSL.

---

### 5.4 Por qué tu equipo local no es el sitio para medir

En el benchmark compiten por CPU, a la vez y en la misma máquina:

| Proceso | Demanda |
|---|---|
| `wrk -t4` | 4 hilos a tope |
| `proxy` | 1 worker por CPU (E5) ⇒ hasta 8 |
| 3 × `echo_server` | 3 procesos |
| Windows + WSL/VirtualBox | el resto |

Tienes **4 núcleos físicos (8 hilos SMT)**. Los 4 hilos de `wrk` ya se comen la
mitad de la máquina antes de que el proxy haga nada.

**Medido, no supuesto** (Docker sobre WSL2, build release, 3 backends
event-driven, 30 s por escenario):

| Escenario | Req/s | Latencia | Timeouts |
|---|---|---|---|
| `-t4 -c400` | 103.819 | 4,37 ms | 23 |
| `-t4 -c200` | 107.686 | 2,33 ms | 56 |
| `-t2 -c100` | 99.491 | 1,11 ms | 8 |
| *control: backend sin proxy* | *130.746* | — | *197* |

Esta máquina **dobla el umbral de 50.000**, así que la previsión inicial de que
no llegaría era errónea. El keep-alive es lo que lo cambia: sin un `connect` por
petición, el coste por petición baja lo bastante como para que el cuello deje de
ser la CPU del proxy.

**Para qué sigue haciendo falta el VPS**: no para la cifra de req/s, sino para
la parte de **«0 errores»** que exige E2. Los errores de arriba son *solo*
timeouts —`connect 0, read 0, write 0`— y el backend **sin proxy de por medio**
da 197, más que los tres escenarios del proxy juntos. Es saturación: ~18 hilos
activos (wrk 4 + proxy 8 + backends 6) sobre 4 núcleos físicos, con `wrk`
cortando a los 2 s. Con núcleos dedicados desaparecen.

Dos matices más:

- El proxy queda al **82 % del techo del backend**, así que lo medido es el
  conjunto de la máquina, no el proxy aislado.
- WSL2 y Docker no distorsionan tanto como cabría temer **para tráfico de
  loopback**: la CPU es casi nativa y el tráfico nunca sale de la VM, así que la
  capa de red virtualizada no interviene.

Orden de calidad de la medida, de mejor a peor:

1. **VPS con núcleos dedicados** → la cifra que se entrega. Sigue siendo
   preferible por reproducibilidad: nadie más compite por la CPU.
2. **WSL2 o Docker Desktop** → buena aproximación local, y suficiente para
   demostrar que se supera el umbral. Úsalo para detectar regresiones.
3. **VM de VirtualBox** → la peor de las tres (§7.1). No la uses para medir.

**Veredicto sobre tu VM `ubuntu24`**: con **1 vCPU y 2 GB** no sirve ni de
aproximación — el proxy arrancaría un solo worker y `wrk -t4 -c400` competiría
con él y con los tres backends en ese mismo núcleo. Si quieres conservarla,
súbela a 4 vCPU / 4 GB y úsala como entorno Linux alternativo, pero para
desarrollar tienes WSL2, que es más rápido y se integra mejor con VS Code.

## 6. Docker: sí para desarrollo y e2e, no para el benchmark ni para kqueue

**Para qué sirve** (y bastante bien, porque ya lo tienes instalado):

- Entorno de build **reproducible**: el evaluador levanta el proyecto con un comando.
- El contenedor tiene **su propio `/etc/hosts`**: el e2e (R9) lo modifica sin tocar
  tu sistema ni pedir privilegios reales. Es la forma más limpia de cumplir el
  paso 3 del enunciado.
- Desechable: si un test deja procesos zombis o puertos ocupados, se tira y ya.
- Sobre Docker Desktop corre en la VM de WSL2 ⇒ `epoll`, `SO_REUSEPORT`, `SIGHUP`
  y `mmap` funcionan de verdad (E4, E5, E13, E16).

**Para qué NO sirve**:

- **Benchmark (E2)**: comparte los mismos 4 núcleos con Windows (§5.4). Además, por
  defecto el contenedor no hereda `ulimit -n` alto — hay que pasar `--ulimit`.
- **kqueue (E3)**: imposible. Un contenedor **comparte el kernel del anfitrión**;
  en Docker Desktop ese kernel es Linux. No existen contenedores de FreeBSD sobre
  Linux. `kqueue` exige un kernel BSD ⇒ máquina virtual (§7).

```dockerfile
# Dockerfile.dev
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y \
    build-essential meson ninja-build pkg-config git libcmocka-dev \
    python3 curl socat jq gdb valgrind clang libssl-dev ca-certificates \
 && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 https://github.com/wg/wrk.git /tmp/wrk \
 && make -C /tmp/wrk -j"$(nproc)" && install /tmp/wrk/wrk /usr/local/bin/ && rm -rf /tmp/wrk
WORKDIR /work
```

```bash
docker build -f Dockerfile.dev -t proxy-dev .
docker run --rm -it -v "$PWD":/work --ulimit nofile=65535:65535 proxy-dev bash
```

Sigue sin servir para el benchmark oficial (corre sobre la misma VM de WSL2), pero
es la mejor opción para que el e2e sea reproducible y para entregar el proyecto
con un entorno que el evaluador pueda levantar en un comando.

---

## 7. La rama `kqueue` (E3) — FreeBSD en local + macOS en CI

**Decisión tomada.** `io_event_kqueue.c` es **código real, compilado y ejecutado**,
en dos entornos complementarios:

| Entorno | Para qué | Ciclo |
|---|---|---|
| **VM FreeBSD en VirtualBox** (§7.2) | Escribir e iterar `kqueue` con `sys/event.h` real; depurar con ASan | Segundos: `ssh` + `meson test` |
| **GitHub Actions `macos-14`** (§7.7) | Confirmar en **macOS ARM64 de verdad** que compila con clang de Apple y pasa los 22 tests | Minutos: `git push` |

Así E3 queda cumplido al pie de la letra — incluida la afirmación de portabilidad
macOS del README — sin comprar hardware. Solo el benchmark (E2) sigue saliendo
del VPS: un runner de CI es compartido y no da cifras defendibles.

**Queda descartado el *mock***: un `io_event_kqueue.c` que ningún compilador ha
visto nunca no es código portable, y contradice la tabla de benchmark macOS que
el propio README publica. El análisis completo está en §7.6.

### 7.1 Aviso: VirtualBox corre degradado en esta máquina

Tu Windows tiene el hipervisor activo (`HypervisorPresent=True`) porque lo
requieren WSL2 y Docker Desktop. VirtualBox 7 no puede entonces usar AMD-V
directamente: se apoya en el backend de Hyper-V y va **2–3× más lento** (la GUI
muestra un icono de tortuga).

No lo arregles desactivando Hyper-V: perderías WSL2 y Docker, que es donde
trabajas. Para compilar y pasar tests en FreeBSD, esa lentitud se nota pero no
molesta. Si prefieres evitarla, FreeBSD 14 también arranca en **Hyper-V** (VM de
generación 2 con Secure Boot desactivado), que convive de forma nativa.

### 7.2 Crear la VM

Descarga `FreeBSD-14.x-RELEASE-amd64-disc1.iso` de <https://www.freebsd.org/where/>
(la RELEASE vigente; evita `-CURRENT`). Luego, en PowerShell:

```powershell
$VB = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
$ISO = "$HOME\Downloads\FreeBSD-14.3-RELEASE-amd64-disc1.iso"   # ajusta la versión

& $VB createvm --name freebsd14 --ostype FreeBSD_64 --register
& $VB modifyvm freebsd14 --cpus 2 --memory 4096 --vram 16 --firmware efi `
      --nic1 nat --natpf1 "ssh,tcp,127.0.0.1,2222,,22"
& $VB createmedium disk --filename "$HOME\VirtualBox VMs\freebsd14\freebsd14.vdi" --size 20480
& $VB storagectl freebsd14 --name SATA --add sata --controller IntelAhci
& $VB storageattach freebsd14 --storagectl SATA --port 0 --device 0 --type hdd `
      --medium "$HOME\VirtualBox VMs\freebsd14\freebsd14.vdi"
& $VB storageattach freebsd14 --storagectl SATA --port 1 --device 0 --type dvddrive --medium $ISO
& $VB startvm freebsd14
```

En el instalador: teclado, red por DHCP, **activa el servicio `sshd`**, crea tu
usuario y añádelo al grupo `wheel`. Tras reiniciar, desde Windows o WSL:

```bash
ssh -p 2222 usuario@127.0.0.1
```

2 vCPU y 4 GB son suficientes y te dejan margen de sobra en tus 32 GB.

### 7.3 Toolchain en FreeBSD

```sh
su -
pkg install -y meson ninja pkgconf git cmocka bash curl socat jq gmake wrk
```

Detalles que sorprenden viniendo de Linux:

- **El compilador base es `clang`**, no gcc. Es una ventaja: detecta avisos que gcc
  deja pasar, y E1 exige compilar limpio.
- **`/bin/sh` no es bash.** Los scripts con `#!/bin/bash` necesitan el paquete
  `bash` (instalado arriba) y la ruta real es `/usr/local/bin/bash`. Usa
  `#!/usr/bin/env bash` en `run_e2e.sh` y `bench_proxy.sh` para que funcionen en
  las tres plataformas.
- `nproc` no existe: es `sysctl -n hw.ncpu`. Tenlo en cuenta en los scripts.
- `wrk` **sí está en los ports** de FreeBSD, no hay que compilarlo.

### 7.4 Diferencias reales entre FreeBSD y macOS — documéntalas

`kqueue`/`kevent` con `EVFILT_READ`, `EVFILT_WRITE` y `EV_CLEAR` se comporta igual
en ambos, así que FreeBSD valida la práctica totalidad de E3/E4. Lo que **no** es
idéntico y afecta a este proyecto:

| Aspecto | Linux | FreeBSD | macOS |
|---|---|---|---|
| Balanceo de accept entre workers (E5) | `SO_REUSEPORT` | **`SO_REUSEPORT_LB`** (desde 12.0) | `SO_REUSEPORT` **sin** reparto de carga |
| Accept no bloqueante | `accept4(SOCK_NONBLOCK)` | `accept4` | **no existe**: `accept` + `fcntl(O_NONBLOCK)` |
| Aviso de cierre del par | `EPOLLRDHUP` | `EV_EOF` en el filtro | `EV_EOF` |

Es decir, `SO_REUSEPORT` **no significa lo mismo en los tres sistemas**: en macOS
no reparte conexiones. Si la implementación depende de ese reparto, en macOS hace
falta un plan B (un solo aceptador que distribuya, o varios `kqueue` sobre el
mismo socket). Decídelo al diseñar `listener.[ch]`, no después.

### 7.5 Cómo llevar el código a la VM

Lo más cómodo es que la VM haga `git clone` por SSH contra tu repo de WSL, o
trabajar con un remoto común (GitHub/GitLab). Evita las carpetas compartidas de
VirtualBox para compilar: mismo problema de rendimiento y de permisos que
`/mnt/d` (§3.2).

### 7.6 Alternativa estudiada y descartada: sustituir macOS/kqueue por Windows/IOCP

La idea es tentadora («ya tengo Windows, me ahorro la VM»), pero no sale a cuenta.

**Problema 1 — cambia un requisito evaluable.** El README dice literalmente
«portable entre Linux (`epoll`) y macOS (`kqueue`)», nombra `EV_CLEAR`, exige la
abstracción `io_event.[ch]` sobre **esas dos** y publica cifras de macOS/ARM64.
Cambiar `kqueue` por IOCP no es un detalle de implementación: es reescribir E3,
E4 y E5, es decir, el documento contra el que se corrige. **Eso solo lo puede
autorizar quien evalúa**, nunca se decide por cuenta propia.

**Problema 2 — IOCP no encaja detrás de la misma API.** No es un tercer
`io_event_iocp.c` y listo: son dos modelos de concurrencia distintos.

| | `epoll` / `kqueue` — *readiness* (reactor) | IOCP — *completion* (proactor) |
|---|---|---|
| Lo que preguntas al SO | «¿puedo leer ya?» | «avísame cuando esta lectura **haya terminado**» |
| Quién pone el buffer | tú, cuando el descriptor está listo | tú, **por adelantado**, y queda inmovilizado durante toda la operación |
| Drenar hasta `EAGAIN` (E4) | obligatorio | **no existe**: no hay nivel ni flanco |
| Unidad de concurrencia (E5) | proceso por CPU + `SO_REUSEPORT` | pool de **hilos** sobre un único puerto de compleción |
| Recarga (E13) | `SIGHUP` + self-pipe | no hay señales: `PostQueuedCompletionStatus` o evento con nombre |
| Arena de buffers (E16) | `mmap` | `VirtualAlloc` |
| Escucha | `fork` tras `bind` | no hay `fork`; `SO_REUSEPORT` no existe en Winsock |

La API que describe el README (`io_loop_add/mod/del(fd, mask)` y «lee hasta
`EAGAIN`») es **de disponibilidad**. Para meter IOCP debajo habría que invertirla
a una API de compleción y **emular esa semántica sobre epoll/kqueue** — que es
exactamente lo que hacen libuv y Boost.ASIO, y es un proyecto en sí mismo. Con
ello desaparece el drenado edge-triggered, que el propio README llama «el corazón
didáctico del proyecto».

**Problema 3 — arrastra el resto del proyecto.** El e2e (R9) necesitaría una
variante para `C:\Windows\System32\drivers\etc\hosts` y los `.sh` no correrían
sin WSL o Git Bash. Y `wrk` no tiene versión nativa de Windows, así que la
comparación con la tabla del README dejaría de ser homogénea (E2, E20).

**Coste comparado**: FreeBSD en VirtualBox son ~30 minutos y satisface la rúbrica
tal cual está escrita; el puerto a IOCP son varios días y la incumple.

**Si aun así te interesa IOCP** —y como ejercicio es muy instructivo— hazlo como
**añadido, no como sustituto**: primero E1–E20 en verde con epoll y kqueue, y
después un `io_event_iocp.c` documentado como extra. Así suma en lugar de
arriesgar la nota.

### 7.7 macOS real y gratis: GitHub Actions

Los runners `macos-14` / `macos-15` son **ARM64**, la misma arquitectura de las
cifras del README, y ejecutan macOS de verdad. Crea `.github/workflows/ci.yml`:

```yaml
name: ci
on: [push, pull_request, workflow_dispatch]

jobs:
  linux:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update &&
             sudo apt-get install -y meson ninja-build pkg-config libcmocka-dev
      - run: meson setup build
      - run: meson compile -C build
      - run: meson test -C build --print-errorlogs
      - uses: actions/upload-artifact@v4
        if: failure()
        with: { name: meson-logs-linux, path: build/meson-logs/ }

  macos:                      # E3: kqueue en macOS ARM64 real
    runs-on: macos-14
    steps:
      - uses: actions/checkout@v4
      - run: brew install meson ninja pkg-config cmocka
      - run: meson setup build
      - run: meson compile -C build
      - run: meson test -C build --print-errorlogs
      - uses: actions/upload-artifact@v4
        if: failure()
        with: { name: meson-logs-macos, path: build/meson-logs/ }
```

**Dos remotos: entrega y CI** — ya configurado. La entrega del curso va al GitLab
de la academia; GitHub existe **solo** para los runners de macOS:

| Remoto | URL | Papel |
|---|---|---|
| `origin` (upstream) | `gitlab.codecrypto.academy/eduxworks/1.5.10-proxy-epoll-kqueue` | **Entrega evaluada** |
| `github` | `github.com/eduxworks/proxy-epoll-kqueue` (público) | Espejo para CI macOS |

`origin` tiene **dos `pushurl`**, así que un solo `git push` actualiza los dos y es
imposible que el CI quede mirando código viejo:

```bash
git remote -v | grep push      # origin -> gitlab  +  origin -> github
git push                       # sube a los dos
gh run watch                   # sigue la ejecución del CI
gh run view --log-failed       # si algo falla
```

Si alguna vez necesitas subir solo a uno: `git push github main` o
`git push https://gitlab.codecrypto.academy/... main`.

**Coste en minutos**: con el repositorio **público**, los runners son gratis e
ilimitados para este uso. Si lo prefieres **privado**, macOS consume minutos con
multiplicador ×10 sobre los 2.000/mes del plan gratuito: un job de ~3 min gasta
30, así que salen unas 60 ejecuciones al mes — suficiente si dejas el job de
macOS solo en `main` y en `workflow_dispatch`, e iteras en la VM FreeBSD.

**Extra opcional**: `vmactions/freebsd-vm@v1` añade un job FreeBSD en CI. No hace
falta teniéndolo en local, pero deja constancia en el repositorio de que las tres
plataformas se compilan.

Con esto en verde, el README pasa a estar respaldado: Linux y macOS compilados y
probados en cada push, con el enlace al workflow como evidencia.

### 7.8 Qué declarar en el README

Con §7.2 y §7.7 en verde, la portabilidad deja de ser una afirmación y pasa a ser
evidencia: enlaza el workflow en el README. Queda un punto por declarar y no se
puede dar por supuesto:

- **Compilación y 22 tests**: Linux (WSL/VPS), FreeBSD (VM) y **macOS ARM64 (CI)**. ✅
- **Benchmark**: medido en **Linux/VPS**. La tabla del README son cifras de
  macOS/ARM64 que tú **no has reproducido**, porque un runner de CI no sirve para
  medir. Publica tus propias cifras indicando SO, CPU y número de núcleos, y deja
  claro cuáles son las de referencia y cuáles las tuyas. Presentar como propia una
  medición que no hiciste es lo único que aquí sería deshonesto.
- **Comportamiento en carga de `SO_REUSEPORT` en macOS** (§7.4): sin reparto por
  parte del kernel. Documenta qué hace tu `listener` en esa plataforma.

---

## 8. Checklist de verificación del entorno

Ejecuta en WSL y en el VPS; todo debe responder:

```bash
uname -sr                                   # Linux 5.x/6.x
gcc --version | head -1                     # >= 11
meson --version                             # >= 0.60
ninja --version
pkg-config --modversion cmocka              # E19
wrk --version 2>&1 | head -1                # E20
curl --version | head -1 ; jq --version ; socat -V | head -1
ulimit -n                                   # 65535 (E2)
nproc                                       # workers que arrancará (E5)
grep -c EPOLLET /usr/include/sys/epoll.h    # >0 ⇒ cabeceras epoll presentes (E4)
sysctl net.core.somaxconn                   # 65535 en el VPS
```

Y la prueba de fuego, ya con el proyecto:

```bash
meson setup build && meson compile -C build   # E18
meson test -C build                           # 22/22 (E19)
./tests/e2e/run_e2e.sh                        # R9
./bench/bench_proxy.sh                        # E2 — solo en el VPS
```

---

## 9. Lo que NO necesitas

No gastes tiempo ni dinero en esto:

- **MinGW, MSYS2, Cygwin, Visual Studio.** No acercan Windows a `epoll`; solo
  añaden una capa de emulación que rompe la semántica de E4 y E5.
- **CMake.** El README fija Meson (E18).
- **nginx, HAProxy, Envoy.** El proyecto es escribir el proxy, no configurarlo.
- **Certificados TLS.** Fuera de alcance según `CLAUDE.md` §1.3.
- **Un segundo VPS para generar carga.** `wrk` corre en la misma máquina, como en
  la tabla del README.
- **Kubernetes, CI en la nube.** Irrelevante para lo que se evalúa.

---

## 10. Orden sugerido de puesta en marcha

1. `wsl --install -d Ubuntu-24.04` y mover el repo a `~/proyectos/proxy` (§3).
2. Instalar paquetes (§4.1), `wrk` (§4.2), `.wrap` de tomlc99 (§4.3).
3. `git config core.autocrlf input` + `.gitattributes` (§3.3).
4. Levantar el esqueleto Meson y que `meson compile` pase en vacío.
5. **Crear la VM FreeBSD (§7.2–7.3) antes de escribir `io_event.h`**: la API común
   se diseña mirando a las dos implementaciones a la vez, y las diferencias de
   §7.4 (`SO_REUSEPORT_LB`, `accept4`) condicionan también `listener.[ch]`.
6. **Publicar el repo y el CI (§7.7) en cuanto el esqueleto Meson compile**, con el
   job de macOS desde el primer commit. Así macOS falla pronto y en pequeño, en vez
   de a lo grande la semana de la entrega.
7. Compilar en FreeBSD desde el primer día, aunque solo sea un `main()` vacío. Si
   dejas la rama `kqueue` para el final, se convierte en una reescritura.
8. Preparar el VPS (§5) cuando haya algo que medir, no antes.
