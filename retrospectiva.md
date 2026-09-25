# Retrospectiva

Qué funcionó, qué no, y qué haría distinto. Documento de proceso, no de
producto: para el **qué** se construyó está [`resumen.md`](resumen.md).

**Datos**: 27 commits, del 23 al 25 de septiembre. ~8.000 líneas de C. Los 20
requisitos del README y los 9 del enunciado, completos.

---

## 1. Dónde aparecieron los fallos

Es el dato más revelador del proyecto, y no es el que esperaba.

| Dónde estaba el fallo | Cuántos | Cuáles |
|---|---|---|
| **En el andamiaje de pruebas y medición** | 7 | puerto ocupado por Jetty · `parse_wrk` sin contar errores · `echo_server` inmortal ante SIGTERM · dominios sin dar de alta · muestreo sesgado del reparto · limpieza que dejaba procesos vivos · control comparado contra un solo backend |
| **En el proxy** | 2 | índice de `least_conn` · byte NUL en la línea de registro |

**Siete a dos.** El código que mide y comprueba falló tres veces y media más que
el código que se medía y comprobaba. Y sus fallos fueron más caros, porque un
test que pasa cuando no debería, o un medidor que informa de una cifra falsa,
**parecen resultados**. Nadie los audita.

El caso extremo: un informe de benchmark con tabla, CPU, RAM, sobresuscripción
de procesos y aviso de saturación —todo el aparato de una medición seria— que
en realidad estaba midiendo un **Jetty ajeno escuchando en el 8080**. Las
863.144 «respuestas de error del proxy» eran 403 de Jetty. Nada en el formato
lo delataba.

Y el medidor ocultaba esos errores por su cuenta: `parse_wrk` anclaba
`Non-2xx or 3xx responses` a principio de línea cuando `wrk` la indenta con
espacios, así que **nunca** se contaban. El resumen decía «0 errores» con
cientos de miles en el fichero crudo.

---

## 2. Lo que funcionó

### 2.1 Escribir los criterios de aceptación antes que el código

`CLAUDE.md` convirtió cada afirmación del README en algo comprobable: no
«balanceo round-robin» sino «300 peticiones ⇒ 100 ±1 por backend». Cuando llegó
el momento de escribir el test, la pregunta ya estaba respondida.

Más útil todavía fue la **separación explícita de lo que no se hace**: TLS,
IOCP, HTTP/2. Cuando surgió la tentación de sustituir `kqueue` por IOCP —lo que
parecía un atajo razonable— había un documento que decía por qué no, y el
análisis se resolvió en minutos en vez de en un desvío de días.

### 2.2 Integración continua desde el primer commit

El CI de macOS entró con el esqueleto, cuando todavía no había nada que probar.
Pagó su coste tres veces, siempre con fallos **invisibles en local**:

- cmocka 2.x usa `##__VA_ARGS__`, extensión GNU que clang rechaza con `-Werror`.
- La misma cmocka deprecó `assert_in_range` y su relevo no existe en la versión
  de Ubuntu: usar cualquiera de las dos rompía una plataforma.
- `SO_REUSEPORT` no reparte carga en macOS.

Ninguno se ve compilando en Linux con gcc **y** clang. Si el job de macOS
hubiera entrado al final, esos tres fallos habrían aparecido juntos y en el peor
momento.

### 2.3 La medida de control

Medir los backends a pelo antes que el proxy es lo que distingue «tu proxy va
lento» de «tu máquina va lenta». Convirtió un «hay 48 errores, algo falla en el
keep-alive» en «el entorno produce 197 errores él solo, sin proxy de por medio».

Sin esa medida habría arreglado un bug inexistente. De hecho **empecé a
hacerlo**: ya tenía localizado el punto de `connection.c` y diseñado el
reintento en conexión nueva antes de mirar el desglose de `wrk` y descubrir que
los errores eran timeouts, no fallos de socket.

### 2.4 Publicar la medición conservadora

Dos ejecuciones del mismo binario dieron 401k y 339k req/s. Se publicó la baja,
diciendo que había otra mejor. Quedarse con la mejor sin mencionarlo no habría
sido una medición sino una selección, y nadie lo habría notado.

---

## 3. Lo que hice mal

### 3.1 Predecir en vez de medir

Afirmé que un Ryzen 5 3400G con 4 núcleos compartidos con Windows **no llegaría
a 50.000 req/s**. Dio entre 99.000 y 110.000. El razonamiento —`wrk` con 4
hilos, el proxy con 8 workers y 3 backends sobre 4 núcleos físicos— era
plausible y completamente equivocado, porque no contaba con que el keep-alive
elimina el `connect` por petición y con ello el grueso del coste.

Esa predicción tuvo consecuencias: orientó la estrategia hacia «hace falta un
VPS» durante un buen rato, cuando la máquina de casa ya doblaba el umbral.

### 3.2 Diagnosticar antes de tener los datos

Ante los primeros errores del benchmark propuse una carrera en la reutilización
de conexiones keep-alive, con su mecanismo detallado y su plan de arreglo. Era
falso: los errores eran **timeouts por saturación**, y el backend sin proxy
producía más que el proxy.

La pista estaba en la salida cruda de `wrk` —`connect 0, read 0, write 0,
timeout 23`— que yo mismo no estaba guardando. El script solo empezó a
conservarla **después** de que me hiciera falta.

### 3.3 Documentar el diseño peor de lo que era

`CLAUDE.md` afirmaba que tres ficheros tienen `#ifdef` de plataforma. Solo tiene
uno: la selección entre `epoll` y `kqueue` ocurre **en el build**, no en el
preprocesador. Estuve vendiendo la abstracción peor de lo que realmente quedó, y
no lo detecté hasta la verificación final.

### 3.4 Afirmar sin comprobar, en pequeño

- Dije que Ubuntu 24.04 no trae paquete `wrk`. Sí lo trae.
- Dije que `admin.localhost:8080` daría 502 en la demo. Da 200, porque ese
  frontend tiene ruta `default` — que había configurado yo diez minutos antes.

Ninguna es grave por separado. Juntas marcan un patrón: **la diferencia entre
saber algo y haberlo comprobado**, que es justo lo que este proyecto ha
demostrado que importa.

---

## 4. Lo que haría distinto

1. **Escribir los medidores con el mismo rigor que el código medido.** El e2e
   comprobaba desde el principio que sus puertos estuvieran libres; el
   benchmark, no. La misma clase de comprobación, ausente justo donde más caro
   costaba: un informe entero de cifras que no eran del proxy.

2. **Guardar la evidencia cruda desde el primer día.** La salida de `wrk` se
   empezó a conservar cuando ya la había necesitado dos veces. Es barato
   guardarla siempre y carísimo no tenerla cuando hace falta.

3. **No estimar rendimiento.** Ni hacia arriba ni hacia abajo. La única
   afirmación defendible sobre velocidad es la que viene con una medición y su
   contexto al lado.

4. **Comprobar el entorno antes de creerse el resultado.** Puerto libre, build
   release, máquina ociosa, ningún proceso residual. Cuatro comprobaciones que
   cuestan segundos y evitan conclusiones falsas que cuestan horas.

---

## 5. Decisiones técnicas que resistieron bien

Ninguna se revirtió, y dos se pusieron a prueba de verdad:

- **Los pools dentro del router.** Al principio `router_lookup` devolvía el
  `cfg_backend`, lo que dejaba el estado de salud fuera del refcount. Moverlos
  dentro hizo que el mismo contador que protege la tabla de rutas proteja el
  estado al que apunta — y eso es lo que permitió que las sondas activas, que
  corren en otro hilo, no se quedaran nunca con memoria liberada.

- **Delimitar antes de reutilizar.** El keep-alive se construyó *después* del
  framing de mensajes, no a la vez. Intentarlo al revés habría producido el
  fallo más difícil de depurar del proyecto: media respuesta servida como si
  fuera la siguiente.

- **Rechazar en vez de adivinar.** Un `Content-Length` con basura es 400, dos
  cabeceras `Host` son 400, un troceado mal formado no se reenvía, una recarga
  que cambia puertos se rechaza con un mensaje. En todos los casos el código
  prefiere negarse a comportarse de forma impredecible.

---

## 6. La lección transferible

> **Los tests y los medidores fallan igual que el código, y sus fallos son más
> caros porque parecen resultados.**

Un bug en el proxy se manifiesta: una petición falla, una conexión se cuelga.
Un bug en el medidor produce un número, y los números no se cuestionan cuando
vienen con una tabla bien formateada al lado.

De ahí salen las tres prácticas que más valor aportaron, y que son las que
repetiría en cualquier proyecto parecido:

1. **Una medida de control** que aísle el entorno del objeto medido.
2. **La evidencia cruda guardada**, porque el resumen puede estar mintiendo.
3. **Comprobar las precondiciones** antes de dar por bueno lo que sale.
