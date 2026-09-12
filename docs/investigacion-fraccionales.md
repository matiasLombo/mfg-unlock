# Investigacion: fraccionales 2.0-6.0 con latencia intermedia estable

Documento vivo. Objetivo, hipotesis rankeadas, lo medido, y el conocimiento
externo. Se actualiza cada vez que se aprende algo; nada entra sin medicion o
sin una cita.

## El objetivo, afinado

Un ratio fraccionario R (ej. 2.5x) que entregue:
- fps promedio = R x base (ya se logra a 2.x-4.x),
- **latencia intermedia ESTABLE** entre floor(R) y ceil(R) -- no el promedio de
  un swing L_floor <-> L_ceil,
- cadencia pareja (fluidez),
- para todo R en [2, 6] (hoy el planificador se corta en 5.0, ver mas abajo).

El modelo mental del usuario (2.5 -> latencia entre 2 y 3, estable) describe la
**difusion por frame**, no la **alternancia por bloques** que corre hoy.

## Por que hoy es por bloques, y por que eso pulsa

DLSS-G genera un numero ENTERO de sub-frames entre dos frames reales. 2.5 no
existe a nivel de un intervalo: son 2 o 3. Algo tiene que alternar; es fisico.

Lo unico que se elige es el PATRON. Y manda una restriccion: con el snippet
2.12 (el que embebemos), la cuenta se cambia de dos formas y las dos tienen
costo:
- `slDLSSGSetOptions` (la API): 100 ms de enfriamiento del plugin por llamada.
- el byte del bound del loop: acoplado a la reserva (ver disasm abajo).

Por eso hoy: alternancia por bloques via la API, 2 cambios por ciclo (~768 ms),
minimo de enfriamientos. Costo: durante el bloque alto corres LITERALMENTE a
ceil(R) (latencia L_ceil), durante el bajo a floor(R) (L_floor). La latencia
PULSA entre los dos en cadencia de medio segundo. El promedio es intermedio;
el instantaneo hace el swing completo.

## El hallazgo del disasm (2026-09-11): dos campos, no uno

`patch_work_item_count` (src/patches.h) documenta que el constructor del snippet
copia DOS contadores seguidos, y cada uno acota un bucle distinto:

    0x47333  mov eax, [rdx+4]   -> [ctx+4]  = BOUND del loop de generacion
    0x4733f  mov eax, [rdx+8]   -> [ctx+8]  = FILL: cuantas ranuras se reservan

    generacion  0x3ddfa  cmp [r13+4], ebx   (recorre hasta [ctx+4]-1)
    llenado     0x45984  mov r14d, [rdx+8]  (reserva [ctx+8] ranuras)

Regla de crash medida (10 autopsias): si `[ctx+4] > [ctx+8]`, la ultima
iteracion de generacion desreferencia una ranura sin puntero -> `[nulo+0x40]`
en 0x3ED6F. Por eso hoy se fuerzan los dos AL MISMO valor.

**La idea nueva:** mantener `[ctx+8] = ceil(R)` fijo (la reserva alta, una sola
vez, sin tocar la API) y **difundir `[ctx+4]` entre floor(R) y ceil(R) por
frame**. Mientras `[ctx+4] <= [ctx+8]` no hay deref nulo: la ranura extra queda
reservada y sin usar (desperdicio de reserva, no crash). Eso seria difusion por
frame SIN enfriamiento -- el mecanismo viejo, resucitado sobre la base nueva.

Esto es hipotesis, no resultado. Lo que hay que medir:
1. `[ctx+4] < [ctx+8]` -> presenta limpio, o "para la presentacion" (la memoria
   [[subframe-bound-semantics]] vio eso, pero con `[ctx+8]` tambien bajo)?
2. El pacer, con `[ctx+8]=ceil` fijo, pacea a ritmo de ceil (los frames "bajos"
   salen muy rapido) o sigue a `[ctx+4]`?
3. Latencia: con la reserva en ceil, queda clavada en L_ceil (malo para el
   objetivo) o sigue al bound `[ctx+4]`?

Si (1) limpio, (2) sigue al bound y (3) sigue al bound: difusion por frame con
latencia intermedia real. Es el premio.

## Lo que hicimos antes y por que se perdio

- [[per-frame-diffusion]] (05-09, base VIEJA -- snippet propio del juego): la
  difusion por frame `acc += per_frame; api = floor(acc); acc -= api` daba
  2.75x con IQR 0.02, "solved and free above 2.0x". Suave.
- [[frac-por-cuenta-de-api]] (base NUEVA -- snippet SDK 2.12): con el 2.12 el
  byte "dejo de modular", la fraccion se movio a la API por bloques. **La
  suavidad por frame se perdio al cambiar de snippet, no se eligio dejarla.**
- [[subframe-bound-semantics]]: la cuenta de la API se puede cambiar en runtime
  pero LENTO (30 frames entre cambios) y con el byte en step. Rapido crashea.

El hallazgo de los dos campos es el candidato a recuperar (1) sobre la base (2).

## Conocimiento externo

- **La distribucion pareja optima de k pulsos en n ranuras es el ritmo
  Euclidiano (algoritmo de Bjorklund)** -- el mismo que Bresenham. Nacio, de
  hecho, en un acelerador de particulas: abrir una compuerta k veces en n slots
  lo mas parejo posible (Bjorklund, Los Alamos SNS). Es EXACTAMENTE nuestro
  problema de repartir los +1. Si algun dia difundimos por frame, el patron
  optimo esta resuelto y probado. (Toussaint 2005; docs.rs/euclidean-rhythm.)
- **NVIDIA, DLSS 4.5 "Dynamic MFG" (RTX 50, 09-2026): NO usa fraccionales.**
  Escala el multiplicador para pegarle a un fps objetivo y "simplemente elige
  entre los enteros 2x/3x/4x/5x/6x" (hothardware, en Nioh 3). O sea: la
  respuesta de la industria a "pegarle a un target" es cambio de entero, no
  fraccional. El fraccionario es NUESTRO diferenciador -- vale saber que nadie
  mas lo hace, quizas porque el hardware (flip metering de Blackwell) hace el
  cambio de entero suave y no lo necesitan.
- **Blackwell tiene flip metering por HARDWARE** (el display engine pacea los
  frames). Ada no. Nuestro pacer es CPU. Por eso el cambio de cuenta nos cuesta
  lo que a ellos no. Cualquier comparacion con lo que hace NVIDIA tiene que
  descontar esa ventaja de hardware.

## Hipotesis, rankeadas por promesa/riesgo

1. **Difusion por frame via `[ctx+4]` con reserva `[ctx+8]=ceil` fija.** El
   hallazgo del disasm. Suavidad por frame + latencia intermedia, cero
   enfriamientos. Requiere: confirmar las tres preguntas de arriba con dynpin.
   ALTO valor, riesgo medio (crash conocido si se invierte la desigualdad).
2. **Atacar el enfriamiento de 100 ms** (0x1800497fd escribe 100.0). Si cae,
   la API se puede cambiar por frame y todo se abre. ALTO valor, ALTO riesgo:
   el enfriamiento probablemente protege la transicion de recursos.
3. **Ciclo mas corto** en la alternancia por bloques (hoy 768 ms): el swing mas
   rapido que el ojo/mano. Barato, mejora parcial, no elimina el pulso.
4. **Metering en Present** (generar ceil, soltar en el swapchain al ritmo
   fraccional): cero enfriamiento pero desperdicia GPU y la latencia queda en
   L_ceil. BAJO valor para el objetivo de latencia.
5. **Aceptar el cambio de entero** como NVIDIA: abandona el fraccionario. Es el
   "saber plegar" si 1-3 no rinden.

## Metodo (el de siempre)

- Ninguna afirmacion sin medir; dynpin fija el ratio para aislar la variable.
- La latencia se lee del reporte de Reflex (sim->driver end), por frame, asi
  que se ve la DISTRIBUCION (unimodal intermedia vs bimodal con swing), no solo
  la mediana. tools/latencia.py.
- Un cambio de comportamiento por commit, con la regresion de los cuatro juegos.
- El snippet 2.12 esta embebido: su disasm es reproducible en cualquier maquina.
