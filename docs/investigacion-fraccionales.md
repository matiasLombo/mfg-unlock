# Investigacion: fraccionales 2.0-6.0 con latencia intermedia estable

Documento vivo. Objetivo, hipotesis rankeadas, lo medido, y el conocimiento
externo. Se actualiza cada vez que se aprende algo; nada entra sin medicion o
sin una cita.

## CIERRE de la investigacion de fraccionales (2026-09-12): CONCLUSION DEFINITIVA

A/B medido en Cyberpunk -benchmark, instrumento honesto (gaps de presentacion):

| modo                       | cambios de conteo | hitches >=16ms |
|----------------------------|-------------------|----------------|
| fraccional 2.5x (bloques)  | 270               | 17.4%          |
| conmutacion enteros (dynstep 100) | 18         | 16.9%          |

Los hitches son CASI IGUALES pese a 270 vs 18 cambios. -> Los cambios de conteo NO
son la fuente dominante de hitches; la escena/base del juego lo es. Esto cierra
todo el hilo:

1. fracres (por frame, inmediato) -> GHOSTING (se saltea la reconfiguracion que
   re-alinea la historia). Muerto, correctamente.
2. El fraccional por BLOQUES (el dynamic default, via API) -> imagen correcta y su
   tasa de hitches ~= la de enteros. El fraccional correcto YA existe y NO es peor
   que enteros en suavidad. No hay "fix de fracres" pendiente: el camino bueno ya
   esta (bloques); fracres era el desvio equivocado.
3. La llamada slDLSSGSetOptions NO bloquea (2us); los cambios de conteo no agregan
   hitches significativos.
4. Los tirones que reporto el usuario vienen de la entrega de frames del propio
   juego (bajones de base), no del mecanismo de conteo del mod.

RESOLUCION: fraccional suave-perfecto no lo limita el mod sino el juego/base.
Opciones utiles del mod, todas validas: enteros fijos (mas nitido, conteo
constante), dynamic por bloques (adaptativo, imagen correcta, ~= enteros en
suavidad), o dynamic con dynstep 100 = conmutacion de enteros (adaptativo + conteo
casi constante + cadencia pareja, lo que hace NVIDIA en Blackwell Dynamic MFG).
fracres queda descartado. Instrumento de diagnostico (bloqueo de setoptions) vive
gateado tras mfg-dyndiag.txt para futuras mediciones. FIN del hilo de fraccionales.

## RUNTIME MEDIDO (2026-09-12): la llamada NO bloquea; el costo es DIFERIDO

Instrumente el bloqueo de slDLSSGSetOptions (gateado tras mfg-dyndiag.txt, off por
defecto -- no mete QPC en el hot path). Cyberpunk, dynpin 250 (fuerza 2<->3 por
bloques via el wrapper que el juego llama por frame):
- **270 llamadas CON cambio de conteo: mediana 2us, media 3us, p90 4us, max 117us.
  NINGUNA >=50ms.** La llamada NO bloquea, ni siquiera al cambiar el conteo. El
  drain NO es sincrono adentro de la llamada.
- PERO los cambios de conteo SI cuestan: 17.4% de los gaps de presentacion son
  >=16ms a 2.5x (contra ~5% en modo entero como GTA V mode 6). Ratio 2.48 correcto.

Conclusion: el hitch del fraccional es un costo DIFERIDO/POSTERIOR a la llamada
(reconfiguracion o generacion degradada que aparece como gaps de presentacion en
los frames siguientes), NO un bloqueo de la API. Eso DESCARTA "drain sincrono en
la llamada". Deja dos opciones para el costo diferido:
- reconfiguracion diferida necesaria (re-alinear historia en la sig. generacion)
- o un throttle/periodo degradado deliberado (removible).
Distinguirlo necesita trazar la GENERACION diferida (GPU-side, dinamico) -- mas
profundo: hookear la ruta de generacion, no solo el setoptions. Ahi quedo la
investigacion; el negativo firme es "la llamada no bloquea" y el costo esta en la
generacion posterior.

## REFINAMIENTO (2026-09-12): el costo es GPU-side; falta runtime para el fix barato

Seguimiento del disasm de la reconfiguracion (0x473d0/0x3b2b0 y calls vecinas):
- El per-element ctor (0x3b2b0) llama por la IAT a _invalid_parameter_noinfo_noreturn
  (stub de error del CRT), NO un allocator.
- Imports del plugin: malloc/free/realloc/LocalAlloc/CoTaskMemFree -- NINGUN import
  de CUDA. La memoria de GPU se maneja DINAMICAMENTE (nvcuda por GetProcAddress),
  invisible en el disasm estatico.
- Conclusion: la reconfiguracion ESTATICA (re-init del array inline de 6 slots) es
  BARATA y no aloca. El hitch de ~100ms NO es CPU -> es GPU-side (dinamico). Lo mas
  probable: un DRAIN/sync del pipeline (esperar la generacion en vuelo antes de
  cambiar el conteo) -- la misma sync que fracres salteo -> ghosting.

Disyuntiva que decide si existe fix barato (NO resoluble estatico):
- DRAIN necesario -> no hay fix barato (correccion misma; Blackwell por HW).
- THROTTLE artificial -> se podria sacar -> fix barato.
Como la llamada GPU es dinamica, distinguir necesita RUNTIME: hookear la ruta del
cambio de conteo y medir que espera y cuanto. Proximo paso concreto si se persigue.

## MECANISMO del ghosting, del disasm (2026-09-12) -- y el fix real

Disasm de sl.dlss_g.dll 2.12. El ghosting de fracres NO es misterioso:

- Cambiar el conteo correctamente exige una RECONFIGURACION, disparada por el flag
  byte [ctx+0x4488] (seteado en 0x49fb1 en la ruta de opciones, chequeado en
  0x470ca). Con el flag en 1, el plugin (0x470ca -> 0x473d0 y calls vecinas)
  RECONSTRUYE el array inline de sub-frames (0x473d0: 6 slots x 0xC0, siempre 6) y
  realoca buffers -> re-alinea la historia temporal para el nuevo conteo -> imagen
  correcta. Ese trabajo ES el costo (la tabla del baseline mide p99 121ms / 19
  hitches a 2.50x; no hay un timer de 100ms separado -> el cooldown es la
  reconfiguracion misma).
- La API setea ese flag -> reconfigura -> sin ghosting, con el hitch. fracres
  cambio el conteo por el inmediato SIN setear el flag -> sin reconfiguracion ->
  la historia queda para el conteo viejo -> GHOSTING. Confirmado.

Por que no hay fix barato trivial: setear el flag = reconfigurar = pagar el hitch;
no setearlo = ghosting. Misma disyuntiva que vio el usuario (bloques vs fracres).

El fix REAL (candidato, deep): reconfiguracion LIVIANA. El array es SIEMPRE de 6
slots, asi que reconstruirlo en un 2<->3 es redundante; un cambio dentro de [2,6]
no necesita realloc, solo re-alinear la historia. Parchear la ruta de reconfig del
plugin para hacer SOLO eso = fraccional LIMPIO (sin ghosting) y BARATO (sin el
hitch pesado). Es, en esencia, lo que Blackwell hace por hardware y Ada no.

Estado: mecanismo mapeado y confirmado. El fix es cirugia del plugin (alto riesgo)
+ validacion VISUAL; es un proyecto, no un parche. Camino concreto, no callejon.
Proximo paso si se persigue: disasm de 0x473d0 y calls vecinas (0x47430, 0x4b230,
0x3b0b0) para separar re-alinear-historia (necesario) de realloc (redundante).

## VEREDICTO VISUAL de fracres (2026-09-12): EMPEORA la imagen (ghosting) \u2014 ABANDONAR

El usuario probo fracres+grilla en GTA V (dynamic) y reporto: **"mucho ghosting y
no fluida"**. Esto es lo que la estadistica NO capturaba y lo que decide: fracres
no es "imperceptiblemente mejor", es **visiblemente PEOR**.

Mecanismo (coherente con H4): la colocacion temporal correcta la hace el kernel
Blackwell calculando la `t` de cada sub-frame para un conteo CONSISTENTE. fracres
VARIA el conteo por frame -> la `t` y las posiciones temporales se desincronizan
del cadence real -> los frames generados caen en posiciones equivocadas =
ghosting/doble imagen, y cadencia despareja = no fluido. O sea fracres PELEA
contra la colocacion temporal que los cubins arreglan. Los dos ejes NO son
independientes cuando se varia el conteo por frame.

**Decision FINAL: fracres se abandona como camino.** No default, y ni siquiera
recomendable como opt-in -- degrada la imagen. La ganancia estadistica (ratio mas
apretado) era enganosa; el render real es peor. Es "saber plegar" del modo mas
claro: NVIDIA hace enteros por algo, y variar el conteo por frame rompe la
interpolacion. El codigo queda off por defecto y documentado como via muerta; el
fix del crash del reload (independiente) se queda porque endurece el default.

Leccion: la unica prueba que valia era la VISUAL. Tres corridas de estadistica
decian "casi igual"; un minuto de juego dijo "ghosting". [[present-timing-is-not-fluidity]].

## H4 RESUELTO — YA lo arreglan nuestros cubins (CORRIGE lo de abajo)

**Correccion (2026-09-12, verificado al nivel de PTX):** NO heredamos el bug del
punto medio. La seccion de abajo ("H4 CONFIRMADO: heredamos el 0.5") solo miro el
kernel ORIGINAL de Ada (sm_89), no lo que ENVIAMOS. Verificado:
- El original Ada (sm_89) SI tiene los 104 `mul.ftz.f32 ...,0f3F000000` (0.5 fijo).
- PERO el kernel Blackwell (sm_120) del MISMO idx 7 tiene CERO de esos y en cambio:
  `ld.param.f32 %f1,[main_kernel_param_0+32]` (carga la t temporal, el MISMO
  offset +32 que RenoDx inyecta), `sub.ftz.f32 %f4,%f27,%f1` (1-t), y decenas de
  `mul.ftz.f32 ...,%f1,...` (mezcla con la t, no con 0.5).
- idx 7 (fingerprint 36992,7776,39, slot 39712) ESTA en src/cubins.h -> lo
  reconstruimos desde el PTX Blackwell y lo swapeamos (entra en el slot). O sea
  reemplazamos el kernel de Ada (0.5 fijo) por el de Blackwell (usa la t).

**Conclusion:** nuestro swap de cubins hace la colocacion temporal correcta por
un camino distinto al de RenoDx: ellos parchean el PTX sm_89 para inyectar la t;
nosotros swapeamos el kernel Blackwell entero que YA la usa (y de paso trae la
calidad Blackwell). Es lo que dice [[cubins-are-the-fluidity-fix]] -- por esto los
cubins hacen 4x fluido. NO hay nada que implementar; el fix perceptible YA ENVIA,
encendido por defecto (mfg-nocubins.txt lo apagaria). CAVEAT: depende de que el
swap ocurra (cubins on por defecto) y que idx 7 entre en el slot (entra en 2.12);
si una version futura del snippet no entra, ahi si caeriamos al 0.5 de Ada -- eso
es lo que hay que vigilar en un OTA ([[mfg-ota-updates-break-patches]]), y
tools/h4_find_blend.py + revisar que la fingerprint siga en cubins.h lo detecta.

El error que cometi: confirmar el bug en el original sin chequear que lo
reemplazamos. Verificar SIEMPRE el kernel que se ENVIA, no el de stock.

## (SUPERSEDED, ver arriba) H4 "CONFIRMADO": heredamos el 0.5 del punto medio temporal

Confirmado con nvdisasm/cuobjdump (pip nvidia-cuda-*, en site-packages/nvidia/
cu13/bin) sobre el snippet 2.12 embebido, reproducible:
- El kernel de interpolacion (idx 7 de las 31 fatbins, slot 39712, el mismo que
  llamamos "mvec" y que nuestros cubins reconstruyen) tiene su PTX sm_89 de
  99439 bytes con **exactamente 104** `mul.ftz.f32 ..., 0f3F000000` -- el 0.5
  compilado. Coincide EXACTO con la firma de RenoDx (kExpectedMidpoints=104,
  kExpectedPtxBytes=99362; el nuestro 99439, misma clase de kernel).
- O sea: TODOS los frames generados se mezclan en el punto medio temporal. A 4x
  son tres frames casi identicos a mitad de camino -- el contador sube, el
  movimiento NO se suaviza. Nuestro rebuild de cubins arregla la CALIDAD de
  mvec/inpaint pero NO el peso de blend, asi que heredamos el bug.
- Este es el eje de fluidez PERCEPTIBLE (a diferencia de fraccionales, que es
  imperceptible). tools/scratchpad blend_sm89.ptx tiene el PTX guardado.

**Receta del fix (de RenoDx midpoint.hpp, verificada contra nuestro PTX):**
1. Ubicar el kernel: fatbin con PTX sm_89 de ~99362-99439 bytes y 104
   `mul.ftz.f32 ...,0f3F000000` (o por nombre main_kernel + tamaño).
2. Inyectar despues del label `$L__BB0_3:`:
   `ld.param.f32 %f134,[main_kernel_param_0+32];` (t),
   `mov.f32 %f135,0f3F800000;` (1.0), `sub.ftz.f32 %f136,%f135,%f134;` (1-t).
3. Reemplazar los 104 `0f3F000000`: los primeros 52 por `%f136` (1-t), los
   ultimos 52 por `%f134` (t) -> cada frame cae en su posicion temporal.
4. Re-emitir: RenoDx re-arma el fatbin y deja que el driver lo JITee; nosotros
   podriamos ptxas->cubin y usar el swap de cubins que ya existe (si entra en el
   slot) o adoptar el JIT.

**Estado:** CONFIRMADO que el bug esta y que el fix es bien definido. Falta
IMPLEMENTARLO (codigo de GPU, [[gpu-code-demands-more-care]] -- se hace tras un
flag, off por defecto, con regresion) y VALIDARLO VISUALMENTE (solo un humano
juzga si el movimiento se suaviza). Es el candidato #1 de mejora VISIBLE.

## VERDICTO FINAL de grilla+fracres (2026-09-12): OPT-IN, no default

Con el crash arreglado, grilla+fracres es SEGURO (0 crashes en Cyberpunk + Halo,
23 reloads; Halo hasta 6x, count_cap=6 -- el max de Halo Campaign Evolved es 6,
NO 3; la memoria vieja era otro Halo) y reproduciblemente MAS SUAVE en la cuenta
(apiChg 9-14 vs 22-27; ratio mas apretado). PERO:
- La latencia es NEUTRA (ruido), no baja.
- La suavidad de cadencia que gana esta, por la nota MEDIDA del propio controlador
  (controller.h:224), ~10x por debajo del umbral perceptible -> probablemente NO
  se siente.
- La grilla CUESTA precision del objetivo (hasta step/2 x base en fps).
- [[present-timing-is-not-fluidity]]: el proyecto ya se quemo con mejoras
  estadisticas que no se ven.

**Decision (disciplinada): NO se hace default.** Cambiar el default de envio por
una mejora estadistica pero probablemente imperceptible, con costo de precision,
es exactamente el error que las memorias marcan. Queda como OPT-IN documentado,
seguro y listo (mfg-fracres.txt=1 + dynstep 25, modo dynamic) para quien quiera el
ratio fraccional pineado o experimentar. Si se quiere decidir el default de
verdad, hace falta un A/B VISUAL (solo un humano juzga fluidez), no mas
estadistica. La palanca de fluidez PERCEPTIBLE que queda es H4 (colocacion
temporal), no esto.

**Lo que SI fue una mejora real de esta linea: el fix de la carrera del reload**
(abajo) -- endurece el default de TODOS los juegos contra un crash real.

## FIX de la carrera del reload (2026-09-12): VALIDADO

El crash de fracres+grilla era apply_override_now llamando g_orig_setoptions, un
puntero que quedaba COLGADO al descargarse el plugin (el handler de unload
dropeaba los sitios de datos pero NO los punteros a funciones; la guarda
"== nullptr" no saltaba porque estaba colgado, no nulo). Fix en loader.h
(on_dll_load, reason UNLOADED): anular todo g_orig_* que caiga en el rango
descargado y limpiar el override pendiente; se re-resuelven al recargar. No
gated: endurece tambien el default (misma clase de crash posible con el baseline).

Validado: crash-repro (grilla+fracres) 3/3 limpio (6 reloads), + regresion en el
build con el fix: Cyberpunk 0 crash (2 reloads, 390 ventanas), Halo 0 crash
(**8 reloads**, 4292 ventanas). En total **14 eventos de reload, 0 crashes**,
contra el crash de antes. GTA V: el DLL carga limpio (plugin armado, sin crash de
init) pero el run automatico se traba en el Rockstar Launcher (necesita click en
Play); cubierto por su validacion full en el build previo (be5aa217, 113 ventanas)
+ el fix siendo codigo de unload comun, probado en 14 reloads de otros dos juegos.

**Consecuencia para el veredicto:** el crash era EL bloqueante de grilla+fracres
para default. Con el fix, grilla+fracres queda: mas suave en la cuenta (menos
cambios de API, ratio mas apretado, reproducible), latencia neutra (ruido), y sin
crash. O sea, ahora SI es una opcion de default real -- la mejora es de fluidez
de cadencia, no de latencia. Falta: confirmar la suavidad con mas corridas y
decidir si se expone (grilla+fracres como el camino fraccional del dynamic).

## CORRECCION (2026-09-12, idea del usuario: grilla 0.25 + fracres)

El veredicto "pliega" de abajo estaba basado en (a) un conteo de cooldowns MAL
medido (usaba una linea de log que solo tiene la rama fracres -> baseline daba 0
falso; el conteo justo por transiciones del "techo API" da fracres 20 vs baseline
23, casi iguales) y (b) no haber probado la grilla que el usuario propuso.
Rehecho bien:

A/B con grilla 0.25 (dynstep 25) + fracres vs baseline, dynamic moviendose, DOS
corridas por config, instrumento justo (transiciones del techo API, igual para
ambos):

| config            | apiChg (r1,r2) | ratio spread | lat p90 (r1,r2) | crash |
|-------------------|----------------|--------------|-----------------|-------|
| grilla + fracres  | 14, 14         | ~0.68        | 4201, 4097      | 0, 1  |
| grilla + baseline | 22, 27         | ~0.86        | 4447, 3873      | 0, 0  |

Lo que REPRODUCE (real):
- fracres+grilla hace MENOS cambios de API (14 vs 22-27) -- la grilla evita que
  el controlador oscile a traves del entero, y fracres difunde por debajo sin
  re-tocar la API. La idea del usuario funciona.
- fracres+grilla da un ratio mas APRETADO (~0.68 vs ~0.86 de spread) -- mas
  suave en la cuenta.
Lo que NO reproduce:
- La latencia: es RUIDO. El baseline r2 tuvo el mejor p90 (3873) de todos. No se
  puede afirmar que fracres baje la latencia.
Lo NUEVO (bloqueante):
- fracres+grilla CRASHEO una vez (r2): 0xc0000005 en una direccion sin modulo,
  2.2 s despues de "modulo descargado: sitios que quedan 0". El unload/reload del
  plugin pasa 2x por corrida en TODAS las configs (churn de las dos copias de
  Cyberpunk), pero solo fracres crasheo. Causa probable: fracres escribe LOS DOS
  sitios parcheados CADA frame (mucho mas que el baseline), asi que tiene mucha
  mas exposicion a la carrera TOCTOU cuando los sitios se desmapean en el reload.

**Veredicto corregido:** con la grilla, fracres es reproduciblemente MAS SUAVE en
la cuenta (menos cambios de API, ratio mas apretado) -- la idea del usuario
rescata la parte de fluidez. PERO no baja la latencia (ruido) y AGREGA riesgo de
crash por la carrera del reload (escribe los sitios volatiles cada frame). NO es
default-ready hasta arreglar esa carrera (sincronizar las escrituras de sitios
con el unload, o escribir menos). El fix es de riesgo (clase
[[hooking-driver-modules]]); no se hace a ciegas.

## VEREDICTO de fracres con el CONTROLADOR (2026-09-12): PLIEGA para el default (SUPERSEDED, ver arriba)

A/B medido en Cyberpunk -benchmark, mode 8 dynamic SIN dynpin (el controlador
decide y se mueve), instrumento honesto (counted multiplier del swapchain):

| fracres | crashes | enfriamientos (API a ceil) | ratio p10-p90 | lat p90 |
|---------|---------|----------------------------|---------------|---------|
| ON      | 0       | **100**                    | 4.17-5.06     | 4611 us |
| OFF     | 0       | **0**                      | 4.11-5.00     | 4365 us |

Lo que dice, sin adornar: el controlador cruza enteros TODO el tiempo mientras
persigue el target, y cada cruce hace que fracres suba el ceil por la API =
resize + 100 ms de enfriamiento. 100 en una corrida (~14% del tiempo degradado).
El baseline (bloques) varia el inmediato POR DEBAJO de la asignacion que ya
existe, sin tocar la API: 0 enfriamientos. Resultado: con el controlador
moviendose, fracres NO gana -- la ventaja de ratio estable que tenia a dynpin
FIJO desaparece (el movimiento del controlador domina la dispersion), y la
latencia p90 queda peor por los enfriamientos.

**DECISION (medida): fracres NO se promueve a default.** El baseline por bloques
ya es igual o mejor para el uso real (controlador en movimiento). fracres queda
como herramienta de NICHO: solo gana a un ratio fraccional FIJO/pineado (dynpin),
donde no hay cruces -- ahi si es 3-6x mas estable sin costo. Es "saber plegar":
el mecanismo existe y funciona, pero NVIDIA hace enteros por algo, y nuestro
baseline por bloques ya cubre el fraccional del controlador. La unica via que
haria a fracres competitivo con el controlador es H1b-opt (asignacion al max una
vez, sin resize), pero eso probablemente clava la base ([[base-rate-pinned-by-ceiling]])
-> peor negocio. fracres se queda off por defecto, documentado, para el caso
pineado.

## REGRESION CERRADA (2026-09-12, build be5aa217, config por defecto flags off)

Los tres juegos instalados, corridos en el build de envio (count_cap + fracres
un-flag, todo off por defecto):
- Cyberpunk: 0 crashes, dynamic ~4.63x, 59 ventanas de generacion. PASS.
- Halo Campaign Evolved: 0 crashes, ~4.93x (mode 5), 1330 ventanas. PASS.
- GTA V: 0 crashes, mode 6 aplicado (114 ventanas, 113 generando), 0 apagones de
  Reflex; el fastfail historico de sl.pcl NO reaparecio. PASS.
La precondicion "regresion de los tres antes del commit del cambio" queda
satisfecha. Nada del cambio (gated off) afecta el comportamiento por defecto.

## ESTADO ACTUAL (2026-09-12) — que hay implementado y como usarlo

**El mecanismo esta resuelto y medido: `fracres`.** Difunde la reserva [ctx+8]
por frame via el inmediato parcheado (sin API, sin enfriamiento). Da fraccional
real 2.25-5.5, 4-6x mas estable en el ratio que la alternancia por bloques, misma
latencia, 0 crash. El disasm (0x45984) confirma por que: [ctx+8] es el bound del
loop de slots, re-leido por presentacion. Seguro en cualquier juego: topa en
count_cap() (el max declarado, Halo=3), no en el estructural.

**Como usarlo (opt-in por ahora):** un archivo `mfg-fracres.txt` con `1` al lado
del juego, en modo DYNAMIC (mode 8). El controlador decide el ratio y fracres lo
difunde por frame. Nada mas hace falta (dynpin es solo para medir un ratio fijo).

**ALCANCE HONESTO de lo medido (correccion 2026-09-12, self-review):** el barrido
uso `dynpin` = ratio FIJO, asi que el ceil se fijaba UNA vez al arranque y nunca
cruzaba un entero -> cero enfriamientos, cero llamadas a la API. Por eso se vio
perfecto. PERO la asignacion la dimensiona la cuenta de la API
([[sub-frame-bound-semantics]]), asi que subir la reserva por encima de la
asignacion actual necesita SI o SI una llamada a la API que la agrande primero
(o la difusion se sale del array del loop 0x45984). No es crash -- count_cap + ese
resize lo cubren -- pero significa que con el CONTROLADOR dynamic moviendose y
cruzando un entero (ej. 2.8 -> 3.2, ceil 3 -> 4) fracres paga UN enfriamiento de
100 ms por cruce. Raro, no cero. Conclusion medida con honestidad:
- "fracres sin enfriamiento" y "3x mas estable" estan probados para un ratio
  FRACCIONAL SOSTENIDO dentro de una banda entera (lo que midio dynpin).
- El caso del controlador moviendose y cruzando enteros esta SIN medir: ahi
  fracres tendria un hitch por cruce, como los bloques en cada cambio. Su ventaja
  podria ser menor de lo que sugiere el barrido a ratio fijo.
- Lo que hay que validar antes del default NO es solo "anda", sino "¿fracres
  gana con el controlador MOVIENDOSE, no solo a ratio clavado?". Esa es la
  medicion que falta (gameplay real, controlador activo).

**H1b-opt (mecanismo ACLARADO por disasm + premisa del enfriamiento; falta UNA
medicion): fracres SIN enfriamiento ni en los cruces.** Cuadro completo ahora:
- El array de METADATA de sub-frames es inline y SIEMPRE de 6 (disasm 0x473d0).
- Los BUFFERS de GPU de los frames generados siguen la cuenta de la API
  (numFramesToGenerate): cambiarla libera y re-reserva con 100 ms
  (0x1800497fd escribe 100.0) -- esa ES la premisa del enfriamiento.
- Por eso: a ratio FIJO la API se setea al ceil una vez -> sin realloc ->
  cooldown-free (medido). Con el controlador MOVIENDOSE y cruzando un entero, la
  cuenta de la API tiene que crecer -> realloc -> un enfriamiento por cruce
  (mi scoping, ahora confirmado por mecanismo, no solo por la corrida a ratio fijo).
- **H1b-opt:** setear la cuenta de la API = MAX (count_cap) UNA vez al arranque
  (un solo enfriamiento) -> los buffers ya alcanzan para cualquier cuenta -> se
  difunde el bound [ctx+8] en [floor,ceil] para siempre SIN realloc, ni en los
  cruces. Queda cooldown-free de verdad con el controlador moviendose.
- Lo UNICO sin medir: declarar el max, ¿clava la base fps?
  ([[base-rate-pinned-by-ceiling]]). mfg-ceilfirst probo declarar el techo y dio
  "sin efecto en el ratio", pero el costo en la base no se cerro. Es la unica
  medicion que separa H1b-opt de "listo". NO implementar a ciegas.
- EVIDENCIA EXTERNA (2026-09-12) que INCLINA H1b-opt hacia PLEGAR: en una RTX
  4080 la base cae ~38 -> ~30 fps al subir 2x -> 6x (tweaktown), y nuestra propia
  [[base-rate-pinned-by-ceiling]] dice que el techo declarado clava la base. Los
  dos apuntan a que declarar el max clavaria la base baja -> mal negocio contra
  el enfriamiento raro del cruce. NO es concluyente: el loop de generacion corre
  [ctx+8] veces (la cuenta REAL, disasm 0x45984), asi que "declarar max pero
  generar menos" PODRIA esquivar el clavado si el costo sigue la generacion real
  y no la declaracion. Sigue necesitando LA medicion (declarar max via fracres,
  difundir menos, medir la base) para decidir. Lectura actual: probablemente
  fracres como esta (API=ceil, enfriamiento solo en cruces) ya es el buen diseño,
  y H1b-opt pliega -- pero es "probable", no medido.

**Por que sigue OFF por defecto y no lo prendi solo:** prenderlo es un cambio de
comportamiento, y la regla es que eso entra con la regresion de los juegos
CORRIDA (jugandolos), no solo con el banco. Fue validado en Cyberpunk (escena
-benchmark) pero NO en gameplay largo ni en Halo/GTA V en DYNAMIC. Con el usuario
durmiendo no se corren juegos (regla). fracres es no-op en modos enteros (Halo=3,
GTA V=6 no lo tocan), asi que el riesgo es solo en DYNAMIC.

**Para promoverlo a DEFAULT (una sesion con el usuario):**
1. Cyberpunk gameplay real (no solo -benchmark) con `fracres 1`, mode 8: sin
   crash, ratio estable, se siente bien.
2. Halo y GTA V en DYNAMIC con `fracres 1`: sin crash (count_cap ya cubre el
   max de Halo, pero hay que verlo corriendo).
3. Si pasa: en config_apply, hacer que la difusion de la reserva sea el camino
   normal del fraccional dynamic (o `g_frac_res = true` por defecto), con la
   regresion de los tres verde. Es lo que pide [[flags-only-disable]]: lo que
   mejora va encendido, no escondido en un flag que se olvida.

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

### RESULTADO MEDIDO de fracdiff / H1a (2026-09-12, Cyberpunk -benchmark)

**H1a REFUTADA, con aislamiento y medicion.** Difundir el bound [ctx+4] por
frame bajo una reserva [ctx+8] fija en ceil NO produce fraccional. La mitad de
H1 que hablaba de crash/parada SI se cumplio (bound<reserva presenta limpio); la
otra mitad no: la presentacion sigue la RESERVA, no el bound.

Corrida instrumentada (fracdiff 1, dynpin 250, mode 8; foco ok, interpolacion
habilitada x1, 0 EXCEPCION / 0 CONGELAMIENTO):

Primera version (bound difundido, reserva por la API) mostro el cableado roto:
la difusion escribia 2 y 3 mitad y mitad (`escrito =2 x22 / =3 x22`) PERO el
sitio entraba a cada frame en 3 (`entro =3 x45`), con 45 `pisadas
(set_count_now)` por ventana = una por frame. Causa medida: Cyberpunk reemite
slDLSSGSetOptions POR FRAME; el wrapper llamaba set_count_now, que escribia los
DOS sitios wic iguales y pisaba el bound difundido. [[frac-por-cuenta-de-api]].

Corregido (set_count_now, con fracdiff+modo fraccional, SOLO sostiene la reserva
[ctx+8] en ceil; el bound, el pacer y count_live quedan para la difusion). Nueva
corrida: el bound YA sobrevive por frame (`escrito =2 x23 / =3 x22`,
`entro =2 x22 / =3 x23`, byte vivo alternando 2 y 3). Sin crash, sin freeze. Y
sin embargo el instrumento honesto (counted multiplier del swapchain) dio **3.00
en las ventanas con generacion** (54 ventanas en 300, ninguna en 250; una
ventana tipica: base Reflex 43, rendered 46, PresentCount 135/ventana ->
3.00x). O sea: reserva=3 -> genera 3, con el bound difundiendo 2/3 libremente
debajo, sin efecto en lo presentado.

**Conclusion:** el bound [ctx+4] NO decide cuantos frames se presentan; la
reserva [ctx+8] si. Difundir el bound es gratis y seguro pero inerte para el
ratio. Coincide con [[sub-frame-bound-semantics]] y [[ngx-caps-the-count]]: la
cuenta de la API (=la reserva) decide la generacion Y dimensiona la memoria.
Esto corrige la lectura previa del disasm ("generacion y pacer siguen el
bound"): el pacer leera el bound, pero lo PRESENTADO sigue la reserva.

**Fork que queda (H1b, no corrido):** difundir la RESERVA [ctx+8] por frame via
el inmediato parcheado, SIN llamar a la API (sin el enfriamiento de 100 ms).
Si el plugin re-lee [ctx+8] por frame como cuenta de generacion -> fraccional
sin enfriamiento = el premio (latencia intermedia estable). Si solo la lee al
reconfigurar, o si reasigna la memoria por frame -> inerte o crash. Riesgo:
transitorio bound>reserva (se evita con el orden subir=reserva primero,
bajar=bound primero, arrancando iguales) y churn de reasignacion
([[fractional-swapchain-churn]]). Es el unico camino sin medir hacia el premio.

### DISASM del array de slots (2026-09-12): es inline y SIEMPRE de 6

En 0x473d0, el camino de config construye el array de sub-frames:
```
1800473d0: sub  $0x28,%rsp
1800473d4: add  $0x48,%rcx           ; base = ctx+0x48
1800473d8: lea  0x18003b2b0,%r9      ; fn por elemento (ctor/init)
1800473df: ba c0 00 00 00  mov $0xc0,%edx   ; stride = 0xC0
1800473e4: 41 b8 06 00 00 00 mov $0x6,%r8d  ; count = 6 (CONSTANTE)
1800473ea: call 0x1800680bc          ; init de 6 elementos de 0xC0 en ctx+0x48
```
El array de METADATA de sub-frames es INLINE en el contexto y se construye
SIEMPRE para 6 (0x6 hardcodeado, stride 0xC0, base ctx+0x48). Coincide con
[[multiframe-ceiling-is-structural]] (el array es inline, 6x es el maximo). El
loop de 0x45984 (bound = [ctx+8]) indexa ESTE array inline.

Implicacion para fracres / H1b-opt: el bound [ctx+8] se puede difundir en
[2, count_cap] contra un array que SIEMPRE tiene 6 slots -- ese array NUNCA se
reasigna, exista el ratio que exista. O sea, la metadata no es lo que forzaria un
resize al cruzar un entero. QUEDA ABIERTO (no resuelto por este disasm): si los
BUFFERS de GPU de los frames generados siguen numFramesToGenerateMax (fijado una
vez) o la cuenta por-llamada; de eso depende si el enfriamiento en el cruce que
documente es real o si fracres ya es cooldown-free hasta 6. Se cierra con el sitio
de asignacion de los buffers de GPU o midiendo con el controlador moviendose.

### DISASM que EXPLICA H1a/H1b (2026-09-12, snippet 2.12 embebido, reproducible)

Disasm del sl.dlss_g.dll 2.12 embebido (AppData\Local\mfg-unlock\sdk\2.12),
imagen base 0x180000000. Confirma con codigo lo que la medicion ya habia dicho.

**0x45984 -- [ctx+8] (la reserva) ES el bound del loop de slots, por frame:**
```
180045984: 44 8b 72 08     mov  0x8(%rdx),%r14d   ; r14d = [ctx+8] = la reserva
180045988: 41 83 ee 01     sub  $0x1,%r14d        ; count-1
18004598c: 0f 88 ...       js   0x180045b98       ; count==0 -> sale (no genera)
180045992: 49 63 c6        movslq %r14d,%rax
180045995: 48 8d 04 40     lea  (%rax,%rax,2),%rax ; *3
180045999: 48 c1 e0 06     shl  $0x6,%rax          ; *0x40  -> i*0xC0
18004599d: 4c 8d 62 48     lea  0x48(%rdx),%r12    ; base del array inline (+0x48)
1800459a1: 4c 03 e0        add  %rax,%r12          ; &slot[count-1], cada slot 0xC0
```
Se lee [ctx+8] al ENTRAR y arma un loop sobre `count` slots de un array inline
(0xC0 = 192 bytes por slot, empieza en ctx+0x48). Esta funcion corre por
presentacion y RE-LEE [ctx+8] cada vez -> por eso difundir la reserva por frame
(fracres) cambia la generacion sin llamar a la API y sin reasignar (no hay
enfriamiento). Y por eso escribir la reserva POR ENCIMA de los slots asignados
se sale del array = crash: la cota correcta es count_cap() (el max declarado por
el plugin, Halo=3), NO el estructural 5/6. El fix del 2026-09-12 topa ahi.

**0x47333 -- [ctx+4] (el bound) es una COPIA de struct, no el loop:**
```
180047326: 8b 02           mov  (%rdx),%eax
18004732b: 89 01           mov  %eax,(%rcx)        ; copia campo a campo...
18004732d: 41 b9 06 00..   mov  $0x6,%r9d          ; 6 = max frames (constante)
180047333: 8b 42 04        mov  0x4(%rdx),%eax     ; lee [ctx+4] = el bound
18004733c: 89 41 04        mov  %eax,0x4(%rcx)     ; ...y lo copia al destino
18004733f: 8b 42 08        mov  0x8(%rdx),%eax     ; sigue con [ctx+8], +0x10, ...
```
[ctx+4] se lee como UN campo mas de un memcpy de config, no como cota de
generacion. Por eso difundirlo (H1a) fue inerte para lo presentado: el loop que
genera (0x45984) mira [ctx+8], no [ctx+4]. La pregunta del goal ("se puede
difundir [ctx+4] bajo [ctx+8] fija?") queda respondida: SI se puede difundir sin
crash, pero NO sirve -- el que manda es [ctx+8]. Difundir [ctx+8] (fracres) es el
que funciona.

### BARRIDO de H1b sobre la banda (2026-09-12, Cyberpunk -benchmark)

Seis corridas (una instancia, secuencial), instrumento honesto = counted
multiplier del swapchain, ventanas con generacion. Metrica de estabilidad
ROBUSTA = ancho del IQR (la sd la infla una sola ventana con hitch; ver abajo).

| punto | modo    | n  | ratio  | IQR       | ancho | lat mediana | crash |
|-------|---------|----|--------|-----------|-------|-------------|-------|
| 2.25  | bloques | 70 | 2.24   | 217-228   | 11    | 2.98 ms     | 0     |
| 2.25  | H1b     | 74 | 2.234  | 224-226   | **2** | 2.95 ms     | 0     |
| 2.50  | H1b     | 72 | 2.48   | 248-251   | **3** | 2.97 ms     | 0     |
| 3.50  | bloques | 65 | 3.479  | 342-355   | 13    | 2.99 ms     | 0     |
| 3.50  | H1b     | 67 | 3.486  | 348-351   | **3** | 3.00 ms     | 0     |
| 5.50  | H1b     | 60 | 5.465  | 544-553   | 9     | 2.98 ms     | 0     |

Lo que dice, sin adornar:
- H1b acierta el ratio en TODA la banda 2.25-5.5 (223/248/349/546 para
  225/250/350/550), llega a 5.5 limpio, 0 crash en ningun punto.
- H1b es 4-6x mas estable en el grueso del ratio (ancho IQR 2-3 vs 11-13 de
  bloques). Bloques topa en 5.0 (scheduler.h:148), asi que a 5.5 no hay A/B.
- La LATENCIA (Reflex sim->driver) es PLANA e IGUAL en todo: ~2.95-3.00 ms en
  los seis, los dos modos. H1b NO cambia la latencia; la fluidez que gana es la
  cadencia de generacion mas pareja, no latencia mas baja. No afirmar lo otro.
- La banda alta (5.5) dispersa mas (IQR 9, p05-p95 513-562): el instrumento del
  multiplicador es mas ruidoso arriba ([[el-juego-apaga-y-no-vuelve]]), pero
  acierta y no crashea.
- Run-to-run: la confirmacion de 2.5 dio sd=10.0 contra 3.0 de la primera, PERO
  el grueso es igual de fino (IQR 3, 30 ventanas en 248 y 27 en 251); la sd la
  inflo UNA ventana en 166 (un hitch de transicion). La metrica robusta (IQR)
  concuerda entre corridas; la sd sola engana ([[mfg-lab-is-flaky]]).

### RESULTADO MEDIDO de H1b (2026-09-12, Cyberpunk -benchmark): FUNCIONA

**H1b CONFIRMADA.** Difundir la RESERVA [ctx+8] por frame via el inmediato
parcheado (sin llamar a la API) produce fraccional real y es MAS ESTABLE en el
ratio que la alternancia por bloques, sin costo de latencia ni crash.

Config: fracdiff 1 + fracres 1 + dynpin 250, mode 8. Foco ok, interpolacion x1,
0 EXCEPCION / 0 CONGELAMIENTO. La difusion escribe los dos sitios a la misma n
por frame (orden seguro: subiendo reserva primero, bajando bound primero;
arrancan iguales, nunca bound>reserva), n nunca por encima del ceil que la API
ya reservo (no se escribe fuera).

A/B contra alternancia por bloques (fracdiff off, dynpin 250), 70+ ventanas de
juego cada uno, instrumento honesto = counted multiplier del swapchain:

| modo            | ratio media | sd(x100) | IQR      | lat sim->driver (mediana) | crash |
|-----------------|-------------|----------|----------|---------------------------|-------|
| H1b (fracres)   | 2.493       | **3.0**  | 248-251  | ~2.9 ms                   | 0     |
| bloques         | 2.485       | **9.4**  | 240-255  | ~2.9 ms                   | 0     |

**Lo que dice la medicion, sin adornar:**
- El plugin RE-LEE [ctx+8] por frame como cuenta de generacion: escribir el
  inmediato 2/3 por frame da 2.5 exacto. Corrige la lectura de H1a (el bound
  [ctx+4] no decide lo presentado; la reserva [ctx+8] si).
- H1b es 3x mas estable en el ratio por ventana (sd 3.0 vs 9.4). El byte vivo
  alterna parejo (200/201) contra el bloque sesgado (87/302). Mas consistencia
  en cuantos frames se generan por frame real = cadencia de entrega mas pareja.
- La LATENCIA de render (Reflex sim->driver end) es IGUAL entre los dos
  (mediana ~2.9 ms, mismo histograma). H1b NO baja la latencia; iguala la de
  bloques y gana solo en consistencia del ratio. No afirmar mejora de latencia.
- Premisa vieja corregida: ninguno de los dos usa la API para cambiar la cuenta
  (ambos escriben el inmediato); el enfriamiento de 100 ms es del API y NO
  distingue a los dos caminos en esta config. Por eso las latencias empatan.
  El costo "por cambio de cuenta" de [[block-length-tradeoff]] NO se ve aca
  aunque H1b hace ~15x mas cambios (uno por frame) que los bloques.

**Estado:** fracres cableado, off por defecto, test-host verde (11), build
limpio. Un A/B por lado (70+ ventanas c/u, no una sola cifra). Falta: confirmar
con una segunda corrida por lado (run-to-run, [[mfg-lab-is-flaky]]); barrer
otros ratios (2.25, 3.5, 5.5) para ver si la ventaja de consistencia se sostiene
en toda la banda 2-6; y la regresion de los cuatro (fracres off = sin cambio).

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

CABLEADO HECHO (2026-09-12, apagado por defecto, EFECTO IN-GAME SIN MEDIR):
config `fracdiff` (off) + rama aislada en fractional_tick. Corre solo con los
dos sitios parcheados (g_wic_n>=2): fija la reserva a ceil por la API cuando
ceil cambia (un enfriamiento), difunde el bound (g_wic_sites[0]) y el pacer
(g_pace_count) por frame con scheduler::diffuse_step, nunca por encima de la
reserva, cero API por frame. Build limpio, test-host verde (10), default off =
el dll de envio no cambia. Falta LA MEDICION -- es lo unico que dice si
funciona. NO afirmar que anda hasta correrlo.

Como medir (con el usuario, juego de base baja): mfg-config con `fracdiff 1` +
`dynpin 250`, mode 8; una corrida; ver en el log `fracdiff: reserva (API) a
ceil`, el ratio real (presents/tokens ~2.5 = modula; ~3.0 o freeze = el flip
queue se ata a la reserva, H1 muere); latencia.py sobre el log (unimodal
intermedia = premio). Regresion: los cuatro con fracdiff off (default) sin
cambio.

Implementacion del cableado (referencia, ya hecha):

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
para, o si presenta n limpio. **El disasm estatico NO puede cerrar esto**
(intentado 2026-09-11): los strings "Present queue is empty" / "Present count
mismatch" / "Num frames to present" (en .rdata 0x778xx) no tienen xref-lea
directo -- se referencian por la capa de logging (tabla de format strings), asi
que el chequeo que estanca vive en el hilo ASINCRONO de rsync/present, no en un
if inline. Es una propiedad de runtime del pacer asincrono. Solo fracdiff (con
juego) la contesta. NO gastar mas disasm aca.

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

### STREAM CERRADO (2026-09-12): la distribucion de la cuenta es optima ("saber plegar")

El goal listaba "delta-sigma / Euclidiano" como conocimiento a explorar. Resuelto:
son EL MISMO algoritmo, ya lo usamos, y es optimo. `scheduler::diffuse_step` es
un modulador delta-sigma de primer orden (`acc += frac; add = floor(acc);
acc -= add`); para un `frac` constante produce exactamente la secuencia de
Bresenham/Euclides, que es el reparto de MAXIMA evenness (Toussaint 2005). No
existe una secuencia de CUENTA mas pareja -- probado por construccion y por
test_diffuse (2.25-5.9 + composicion con cap). Y para R variable (el controlador
dynamic moviendo el target) el acumulador arrastra el error entre cambios, que es
lo correcto.

Conclusion (saber plegar, en el buen sentido): el eje de la CUENTA esta cerrado
-- ni una distribucion mas lista mejora la fluidez, porque la que hay ya es la
optima. La fluidez que falta NO sale de repartir mejor la cuenta; sale del OTRO
eje, la colocacion temporal (H4). Ahi es donde poner el esfuerzo.

### Conocimiento externo (2026-09-12): el eje de COLOCACION TEMPORAL, confirmado

Un fork mas nuevo (ImDreamt/MFGAdaUnlock-RenoDx, `midpoint.hpp`) confirma y
detalla el segundo eje de fluidez, SEPARADO de la cuenta que trabaja fracres:
- El kernel de interpolacion mezcla con un **0.5 compilado**, asi que TODOS los
  frames generados caen en el punto medio temporal -- "4x produces three
  identical half-way frames, the counter doubles and the motion does not get
  smoother". El contador sube pero el movimiento no se suaviza.
- Su fix: descomprime el PTX del kernel, **reescribe el peso de blend para que
  venga del parametro temporal propio del kernel**, y re-emite el fatbin para
  que el driver lo JITee corregido. No da la formula de posiciones (0.25/0.5/
  0.75 para 4x) en el README; vive en midpoint.hpp.
- No tiene soporte fraccional: **fracres (cuenta fraccional por frame) es
  nuestro y es un eje distinto al de ellos.** Los dos ejes se combinan.
- Confirma tambien que en Ada hay que apagar el flip metering (ForceFlipMetering
  Off) o "it freezes the presented image" -- coincide con [[no-pelearle-el-eoff-al-juego]].

Relacion con lo nuestro: nuestros cubins ([[cubins-are-the-fluidity-fix]])
atacan el MISMO eje pero cambiando kernels enteros (Blackwell-PTX) en vez de
reescribir solo el peso de blend. PREGUNTA ABIERTA (necesita GPU, no se puede de
noche): nuestros cubins, ¿colocan los frames en su posicion temporal correcta o
tambien heredan el 0.5? Si lo heredan, el enfoque de RenoDx (reescribir el peso
desde el parametro temporal) seria un fix mas barato y quirurgico -> H4 abajo.

H4 (nueva, no empezada) — colocacion temporal pareja. HALLAZGO clave del
2026-09-12: nuestros cubins reemplazan SOLO 3 kernels -- Shared_Prev2Curr/
Curr2Prev (mvec), Shared_PackedInpaintDynamic (inpaint), Shared_NeedsInpainting
(decision de inpaint). NINGUNO es el kernel de blend/interpolacion temporal. O
sea: nuestro proyecto arregla la CALIDAD de mvec/inpaint ([[cubins-are-the-fluidity-fix]])
pero NO toca la colocacion temporal, asi que muy probablemente HEREDA el 0.5
compilado que documenta RenoDx (todos los frames generados al punto medio). Es
una hipotesis razonada con cita, no medida.

Por que puede ser la mejora VISIBLE mas grande: afecta a TODOS los modos multi
(2x/3x/4x/6x), no solo el fraccional -- a 4x hoy serian tres frames identicos al
medio (el contador sube, el movimiento no se suaviza). Es ortogonal a fracres
(cuenta) y se combina. Plan: (1) confirmar estaticamente el 0.5 en el kernel de
blend del snippet (cuobjdump/nvdisasm sobre el fatbin; es LECTURA, seguro); (2)
si esta, portar la idea de midpoint.hpp -- reescribir el peso de blend para que
salga del parametro temporal del kernel, re-emitir el fatbin, que el driver lo
JITee; (3) validar VISUALMENTE con GPU y con el usuario. Riesgo alto: codigo de
GPU, un byte mal = crash/corrupcion silenciosa ([[gpu-code-demands-more-care]]);
NO se toca ni valida de noche. Es el candidato #1 para la proxima sesion con GPU.

### Fuentes
- https://github.com/ImDreamt/MFGAdaUnlock-RenoDx (midpoint.hpp; temporal midpoint fix, fatbin JIT, ForceFlipMeteringOff)
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
