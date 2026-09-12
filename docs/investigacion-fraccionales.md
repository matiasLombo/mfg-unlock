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

### Disasm confirmado en sl.dlss_g.dll 2.12 (2026-09-11, sesion Opus 4.8)

OJO: los campos estan en `sl.dlss_g.dll` (el PLUGIN, 612992 bytes), NO en
`nvngx_dlssg.dll` (el snippet, 7.5 MB). El RVA 0x47333 encaja en el plugin.

- Constructor de los dos contadores en **0x47333** -- MISMO RVA que la memoria
  de 2.13, la estructura no cambio entre 2.12 y 2.13:
      0x47333  mov eax,[rdx+4]          (bound)
      0x4733c  mov [rcx+4],eax          (+9 del patron)
      0x4733f  mov eax,[rdx+8]          (reserva)
      0x47348  mov [rcx+8],eax          (+15)
- **El loop de generacion RE-LEE [ctx+4] de memoria cada iteracion** (no lo
  cachea): `cmp [r13+4],ebx` en 0x3ddfa y `cmp edi,[r13+4]` en 0x3e210. Esto
  es lo decisivo: escribir ctx+4 por frame cambia el bound por frame.
- La reserva se dimensiona de [ctx+8]: `mov r14d,[rdx+8]` en 0x45984, una vez.

Los dos campos se consumen por separado y el bound es una lectura viva. La via
para difundir por frame existe a nivel de codigo.

### Plan de fracdiff, listo para ejecutar con el usuario (2026-09-11)

Estado del disasm: generacion y pacer siguen el bound [ctx+4]; solo la memoria
sigue la reserva [ctx+8]. La unica duda es el flip queue con bound < reserva.
fracdiff la mide directo. NO escribir el codigo sin poder correrlo (parcheo del
plugin sin test = crash a ciegas); esto es el plan para hacerlo juntos.

El nucleo puro del reparto YA esta hecho y testeado sin GPU
(`scheduler::diffuse_step`, tools/test_diffuse.cpp, en test-host): promedia R
exacto y da el reparto de maxima evenness (Euclidiano) para R de 2.25 a 5.90,
incluida la banda >5.0. Correcto por construccion; lo que falta es cablearlo al
plugin (parcheo) y medir. Ver [[politica-sin-gpu]].

Implementacion del cableado (un commit, con test + regresion):

1. Config `fracdiff` (off por defecto), como dynstep/dynpin. `g_frac_diff`.
2. En fractional_tick, rama nueva cuando `g_frac_diff && sel_is_frac()`:
   - Reserva a la API = ceil(R), UNA vez: set g_force_generated = ceil, marcar
     g_opt_pending SOLO cuando ceil cambia (no por frame). Unico enfriamiento,
     al arrancar o si R cruza un entero.
   - Difusion por frame del bound con Bresenham (== Euclidiano para k/n):
     acc += frac; add = (LONG)acc; acc -= add; n = lo + add;  (add 0 o 1).
     Reparte los +1 lo mas parejo posible sin tabla.
   - Escribir n SOLO en los sitios del bound y del pacer (g_wic_sites[0] y
     g_pace_count), NUNCA en el de la reserva (g_wic_sites[1] queda en ceil).
     Hoy set_count_now escribe los dos wic sites iguales: separarlos. n nunca
     por encima de ceil.
   - gen_flag = (n>0). Cero llamadas a slDLSSGSetOptions por frame.
3. Medicion (dynpin 250 + fracdiff on, un juego de base baja):
   - ratio: modula a 2.5? Si queda en 3.0 o para -> el flip queue si se ata a
     la reserva, H1 muere y volvemos a H2/H3.
   - latencia.py: sim->driver end UNIMODAL intermedia (exito) vs bimodal
     (swing) vs clavada en L_ceil.
   - regresion: los cuatro juegos con fracdiff OFF sin cambios.

Riesgo: si el flip queue se estanca con bound<reserva, se ve como freeze o
ratio clavado -- no crash (el byte nunca pasa la reserva, el lado que crashea).
Reversible: fracdiff off.

---

### El pacer computa el timing DESDE el bound (2026-09-11, disasm 0x48728)

Disasm de la computacion del pacer (0x180048728), el sitio de patch_pacer_count:

    mov rsi, [r14+0xcc8]     ; unidad de intervalo
    mov ecx, [r13+4]         ; el BOUND (ctx+4) = nuestra cuenta
    imul rcx, rsi            ; rcx = bound * unidad
    sub  rcx, rax            ; - tiempo transcurrido (rax = 1e6*const/[r14+0xcb0])
    ... cmp/cmovne: clamp del intervalo a >=0, NO un gate por conteo de frames

El pacer computa el intervalo de present como **funcion lineal del BOUND**, no
de la reserva, y NO tiene aca un "espera hasta juntar N frames". Con reserva
fija en ceil y bound=n por frame, el pacer pacea n frames. Sube la confianza en
que fracdiff module: todo el timing sigue el bound; solo la memoria sigue la
reserva. El deadlock "Present queue is empty" de Metro (bound > reserva) es un
sitio distinto -- el flip queue con `reserva` ranuras desbordado por generar de
mas -- y es el caso POR ENCIMA, no por debajo.

Queda por confirmar en el flip queue si bound < reserva sub-llena la cola y eso
para, o si presenta n limpio. Esa es la unica duda que el disasm no cierra y que
fracdiff mediria directo.

---

### CORRECCION (2026-09-11, mismo dia, mas disasm): H1 NO refutada, SIN AISLAR

Seguir el disasm de los consumidores corrige la refutacion de abajo. Mapa
completo de quien lee que, en sl.dlss_g.dll 2.12:

- BOUND [ctx+4]: lo lee el loop de generacion (0x3ddfa, 0x3e210) Y **el pacer**
  (`mov ecx,[r13+4]` en la firma de patch_pacer_count -- r13 es el mismo ctx).
  Los dos estan parcheados (g_wic_sites[0], g_pace_count).
- RESERVA [ctx+8]: **solo la lee el fill** (0x45984), que dimensiona la memoria.
- gen-flag: parcheado (g_gen_flag).

O sea: generacion Y pacer siguen al BOUND; solo la reserva de memoria usa
[ctx+8]. Eso es lo contrario de "el metering se ata a la reserva" que escribi
abajo. El metering (pacer) sigue al bound, que controlamos por frame.

La evidencia de "detiene la presentacion" hay que releerla con esto:
- El deadlock de Metro (RSYNC "Present queue is empty", mode 2->4) fue el pacer
  POR ENCIMA de la API (pacer 3, reserva 2). Confirma que **por encima** de la
  reserva es malo. NO dice nada de por debajo.
- set_count_now HOY recorta el byte A la API (`w > ap -> w = ap`) y mueve la
  reserva en bloques junto con el byte. Asi que "reserva fija en ceil, bound por
  debajo" **nunca se aislo**: el codigo siempre movio los dos juntos.

**Estado corregido: H1 no esta refutada. Esta sin probar en aislamiento.** El
disasm muestra un camino viable (pacer sigue el bound; solo el fill usa la
reserva). El experimento definitivo, nunca corrido:

    fracdiff (modo experimental): reserva fijada a ceil(R) por la API UNA vez
    (un enfriamiento al arrancar); bound + pace + gen difundidos por frame en
    [floor, ceil] (byte NUNCA por encima de la reserva); CERO llamadas a la API
    por frame. Medir: (a) modula el ratio? (b) latencia unimodal intermedia?

Si (a) modula: H1 revive y es el premio -- difusion por frame, cero
enfriamientos, latencia intermedia estable. Si no modula: entonces si hay algo
mas atado a la reserva y ahi si esta el muro. Es un cambio de comportamiento
(config fracdiff) + una corrida con juego: no se puede cerrar solo con disasm.

---

### RESULTADO (2026-09-11): hipotesis 1 REFUTADA en 2.12

Q0 (mismo ctx): SI, probado por la regla de crash (que [ctx+4]>[ctx+8]
desreferencie nulo solo pasa si son campos del mismo objeto).

Q1 (direccionables por separado): SI. patch_work_item_count reescribe el codigo
del ctor: `mov eax,[rdx+4]` -> `push N; pop rax` (g_wic_sites[0] = inmediato del
bound) y `mov eax,[rdx+8]` -> igual (g_wic_sites[1] = inmediato de la reserva).
set_count_now escribe el MISMO valor a los dos. Separarlos seria trivial.

Q2 (presenta limpio con [ctx+4] < [ctx+8]?): **NO. Detiene la presentacion.**
Medido en [[frac-por-cuenta-de-api]]: "byte por encima de la reserva crashea,
POR DEBAJO detiene la presentacion." La idea (reserva=ceil fija, bound difundido
por debajo) ES lo que describen los comentarios viejos de fractional_tick ("the
loop is free to produce fewer"), pero eso era la base VIEJA. En 2.12 generar
menos que la reserva no presenta menos: no presenta.

Por que: el bound [ctx+4] SI es lectura viva y genera menos (el disasm no
mentia), pero el metering/pacer del plugin espera `reserva` frames por batch y
un batch que entrega menos SE ESTANCA ("a batch stalls when the loop delivers
fewer than the presentation side was promised"). El metering se ata a la reserva
/ cuenta de la API, no a nuestros bytes. Por eso la reserva solo se cambia por
la API, solo por bloques, con el enfriamiento -- de donde sale el pulso.

**Consecuencia:** la difusion por frame no se recupera en 2.12 tocando solo el
bound. El muro es que el metering se ata a la reserva. Re-ranking:

- **H2 (el enfriamiento de 100 ms)** sube: unico camino para cambiar la reserva
  por frame. ALTO riesgo.
- **H3 (ciclo mas corto)**: baja el periodo del pulso, no lo elimina. Barato,
  parcial, sin riesgo. Candidato para lo proximo con juego.
- **Nueva sub-pregunta**: el metering que se estanca -- contador SEPARADO y
  parcheable (como el bound), o intrinseco al flip queue? Si es aparte y se
  difunde junto al bound, H1 revive. Camino: disasm del consumidor de [ctx+8]
  en el camino de PRESENTACION (no el de fill 0x45984).

### Preguntas abiertas, en orden, para la proxima sesion

0. Son r13(gen), rdx(fill) y rcx(ctor) el MISMO ctx en runtime? El ctor escribe
   [rcx+4]/[rcx+8]; los consumidores leen [r13+4] y [rdx+8]. Confirmar que es un
   unico objeto persistente. Camino: ver de que puntero vienen r13 y rdx.
1. Que escribe hoy g_wic_sites (set_count_now)? Si ya escribe el ctx+4 vivo por
   frame, difundir es: parchear el [ctx+8] del ctor a ceil por separado y
   escribir floor/ceil en ctx+4 por frame. Camino: patch_work_item_count y a
   que direccion apunta g_wic_sites.
2. [ctx+4] < [ctx+8] presenta limpio o para la presentacion? Con dynpin y un
   parche de prueba que fije [ctx+8]=3 y ponga [ctx+4]=2: si presenta a 2x sin
   crash ni freeze, la desigualdad es segura hacia abajo.
3. El pacer y la latencia siguen a [ctx+4] o a [ctx+8]? Con lo anterior
   andando, latencia.py sobre un dynpin 250 por difusion: unimodal intermedia
   (exito) vs bimodal o clavada en L_ceil.

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

## Conocimiento externo (ampliado, 2026-09-11) con fuentes

### Ada no tiene flip metering por hardware; se emula en CPU
Confirmado por varias fuentes: DLSS 3 en Ada paceaba por CPU, con variabilidad
que se compone al agregar frames; Blackwell movio el pacing al display engine
(hardware flip metering). Los mods de MFG en Ada (el nuestro incluido) corren
ese codigo de metering en CPU y backportean las instrucciones de timing de
Blackwell. Consecuencia para nosotros: el "metering" que puede estancar la
presentacion ES nuestro path de CPU, no una caja negra de hardware -- se puede
inspeccionar y tocar. (nvidia.com DLSS4 news; tomshardware; tweaktown.)

### El punto medio temporal: un eje SEPARADO de la cuenta
Un mod hermano (mavismmg/MFGAdaUnlock-RenoDx) documenta un bug distinto de Ada:
el kernel de interpolacion mezcla con un **0.5 compilado**, asi que todos los
frames generados caen en el punto medio temporal -- "4x produces three
identical half-way frames". Lo corrige reescribiendo el peso de blend para que
use el parametro temporal y cada frame caiga en su posicion (0.25, 0.5, 0.75
para 4x). ESTE es otro eje de fluidez, independiente de la cuenta y del pulso:
donde se muestrea cada frame generado en el tiempo. Nuestro proyecto lo aborda
por otro lado -- los cubins Blackwell recompilados ([[cubins-are-the-fluidity-fix]])
hacen la colocacion correcta. Vale tenerlo separado en la cabeza: "latencia
intermedia" (cuenta/reserva) y "colocacion temporal pareja" (blend weight) son
dos problemas distintos. (github.com/mavismmg/MFGAdaUnlock-RenoDx.)

### Otros chocan con el mismo freeze del flip-metering
El mismo RenoDx no implementa su propio scheduler (deja el pacing a DLSS-G) y
tiene un "legacy compatibility setting" que **desactiva el path de flip-metering
del plugin y fuerza el fallback de software cuando los multiplicadores altos si
no congelan la presentacion**. O sea: el freeze del flip-metering a
multiplicadores altos es un problema conocido por mas de uno, y un fallback
posible es apagar ese path. Corrobora nuestro riesgo del flip queue en fracdiff
y sugiere una salida si fracdiff congela: probar con el flip-metering apagado.
(github.com/mavismmg/MFGAdaUnlock-RenoDx.)

### Teoria del reparto (ya anclado arriba)
Euclidiano/Bjorklund = reparto pareto optimo de k pulsos en n ranuras, con
error-feedback (delta-sigma) como la generalizacion para fracciones arbitrarias
y para el sub-2.0x. Bresenham es el mismo algoritmo. (Toussaint 2005;
docs.rs/euclidean-rhythm.)

### NVIDIA no hace fraccionales (ya anclado arriba)
DLSS 4.5 Dynamic MFG cambia entre enteros 2-6x para un target; no fracciona.
El fraccionario es nuestro. (hothardware DLSS 4.5.)

### Fuentes
- https://www.nvidia.com/en-us/geforce/news/dlss4-multi-frame-generation-ai-innovations/
- https://hothardware.com/news/nvidia-dlss-45-dynamic-mfg-tested
- https://www.tomshardware.com/video-games/pc-gaming/nvidia-app-update-fails-to-block-unofficial-dlss-multi-frame-generation-on-rtx-40-series-modders-restore-support-across-multiple-games-within-hours
- https://www.tweaktown.com/news/113411/dlss-multi-frame-generation-works-on-rtx-40-series-gpus-with-new-mod-for-cyberpunk-2077/
- https://github.com/mavismmg/MFGAdaUnlock-RenoDx
- https://docs.rs/euclidean-rhythm/latest/euclidean_rhythm/ (Bjorklund; Toussaint 2005)

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
