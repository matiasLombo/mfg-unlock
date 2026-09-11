# Arquitectura por capas: que romperse por juego deje de ser una opcion del codigo

Diseno propuesto despues de leer `src/proxy.cpp` (10.201 lineas), los docs de
`docs/`, `FINDINGS.md`, `TESTING.md`, `tools/regresion.py` y `tools/bench.py`.
Las referencias a lineas son del `proxy.cpp` con mtime 2026-09-10.

## 1. Lo que dice el codigo, no el resumen

Los seis fallos del dia no son seis causas. Son tres, y las tres estan en la
forma del codigo, no en los juegos.

**No hay una fuente de verdad sobre que binario corre.** Hay cuatro que se
pisan: `g_dlssg_base` se asigna a *la ultima copia mapeada* (8368), asi que
con tres copias en Halo las lecturas por RVA (`0x8f1e8`, `+0x4168`, `+0x45a8`)
caen en la que mapeo ultima, no en la viva; `copia_registrar` lleva su propia
lista (8393); las listas de sitios `g_wic_sitios`/`g_imm*` son otra (2023,
8263); y `hk_ldrload` pregunta `GetModuleHandleW` (9403). `g_snippet_cargado`
lo escribe `detectar_semantica` en *cada* modulo con "dlssg" en la ruta (8461):
ultimo gana, y antes de que mapee el primero vale 0, que es "la cuenta son los
generados". De ahi sale la semilla 1 = 1X (fallo 4): no es un bug del
controlador, es que la semantica es una variable global sin dueno que se
consulta antes de existir.

**El gancho de carga mezcla cuatro politicas en una funcion.** `hk_ldrload`
(9194-9477) decide, en este orden y bajo el loader lock: snippet propio
(`idx == -3`), "base propia" para los 9 plugins y el interposer (9412), guarda
de "ya mapeado" solo para el interposer (9402), y despues el camino viejo de
veredicto ROJO + consentimiento + `armar_set_objetivo` que mueve tambien el
interposer (9427-9476). Los comentarios registran las inversiones: "La guarda
vale para TODOS los modulos" y once lineas mas abajo "La guarda vale SOLO para
el interposer. Aplicarla a todos fue el defecto" (9381-9386). Eso es el fallo 1
y el fallo 5 en su forma original: reglas que se escriben mirando la topologia
del juego que acaba de romper.

Y hay **dos canales de observacion** que no se hablan:
`LdrRegisterDllNotification` (`on_dll_load`, 8275) parchea *toda* copia de
`sl.dlss_g` que mapee, incluidas las que el gancho dejo pasar, y decide
"shadow/live" por si la OTA ya mapeo (8457-8460); el gancho sustituye. Ninguno
de los dos sabe cual copia va a usar el interposer. `deteccion-del-set.md` ya lo
dice con todas las letras: *"cual copia queda viva es un hecho observado al
final, no deducible al principio"*. El codigo sigue decidiendo al principio.

**La politica de opciones es un arbol de casos sobre estado mutable, con varios
escritores.** `force_into` (1028-1312) tiene cinco ramas anidadas y tres de
ellas dependen de `cuenta_es_multiplicador()`; escribe `eOn` siempre que
`sel >= 2` y solo despues pregunta si el juego apago (1031, y ahi esta el
fallo 3 en su version anterior). `g_opt_pending = 1` se escribe desde nueve
lugares (`dyn_apply` 4244, `dynamic_tick` 4297, settings 5050, el token 5351,
cuatro teclas del panel 7218-7274, `DllMain` 9905), y `apply_override_now`
(1577) repite la llamada desde el hook de present. O sea que el plugin tiene
dos escritores de `slDLSSGSetOptions` en dos hilos: el juego a 170 Hz y
nosotros cuando el controlador quiere. El enfriamiento de 100 ms esta
documentado en 1073 y en 1579 y no se hace cumplir en ningun tipo. Eso es el
fallo 6.

**El estado es plano.** 248 globales `g_*`, 30 `flag_file` en `DllMain`
(9799-10088) mas ocho archivos numericos, y cinco hilos que los tocan: el
callback del loader (bajo el loader lock), el hilo de render (SetOptions y
token), el de present, el del panel/teclas y el recorder. No hay una maquina de
fases; hay banderas que "solo saben apagarse" (fallo 1, `g_wic_ok`).

**El banco mide lo que no falla.** `regresion.py` corre cinco modos sobre
`bundled-2.12.0` en el sample, que importa el interposer estaticamente, con una
copia de cada modulo y un swapchain. Lee "entregado" como presentadas sobre
renderizadas. Los seis fallos son de topologia (1, 5), de traduccion (2, 4) y
de escritores concurrentes (3, 6); ninguno cambia el ratio en el sample. Que
diera 5 de 5 en verde era lo esperable.

Con esto, la respuesta a la pregunta original queda asi: **no existe un diseno
que elimine la variabilidad por juego, porque el juego es dueno del orden de
carga y de la intencion de sus llamadas. Pero si existe uno donde la
variabilidad tiene un solo lugar donde entrar, se verifica antes de tocar un
byte, y cuando no se cumple el resultado es "no hago nada y digo por que" en
vez de un crash.** Eso convierte cada juego nuevo en, a lo sumo, una linea de
diagnostico y un fixture, nunca una regla nueva en `force_into`.

## 2. El diseno: tres capas y un estado con fases

```
  disco (una vez, en DllMain)         proceso (observado)          juego (por llamada)
  ┌───────────────────────┐   ┌──────────────────────────┐   ┌─────────────────────────┐
  │ Config                │   │ Registro de imagenes      │   │ resolve()               │
  │  - archivo ini/panel  │   │  - por notificacion Ldr   │   │  pura: pedido del juego │
  │  - dialecto del juego │──▶│  - hash, version, rol     │──▶│  x config x dialectos   │
  │    (snippet en disco) │   │  - VIVA = la que ejecuta  │   │  → pedido al plugin     │
  └───────────────────────┘   │  - invariantes → Fase     │   │  un solo escritor,      │
                              └──────────────────────────┘   │  latch con dwell        │
                                                             └─────────────────────────┘
  Fase: ARMADO → VERIFICADO | PASIVO → ACTIVO      (monotona, sin retroceso)
```

### Capa 0 — Identidad: que binario corre

Un solo registro, escrito unicamente desde el callback de
`LdrRegisterDllNotification`, con una entrada por modulo de interes:
base, tamano, ruta, rol (interposer / plugin / snippet), version del recurso,
y un hash barato del `.text` (CRC32 o FNV sobre la seccion, calculado al
mapear; los archivos son nuestros, asi que el valor esperado se conoce en
tiempo de compilacion y va en una tabla `{hash → constantes}`).

La copia **viva** no se deduce del orden de mapeo. Se observa por ejecucion:
ya enganchas `slGetFeatureFunction` (1939); el puntero que devuelve para
`slDLSSGSetOptions` cae dentro de exactamente un modulo del registro, y ese es
el `sl.dlss_g` vivo. Para el snippet, la copia viva es la unica que sigue
mapeada en el primer `slGetNewFrameToken`; si hay dos, no hay copia viva: hay
un invariante roto. Para el interposer, viva es la que exporta la funcion que
enganchaste.

Las constantes por RVA (`0x8f1e8`, `+0x4168`, `+0x45a8`, `+0x45e1`, `+0x45e4`,
tamano `0x97000`) dejan de ser "suposiciones cableadas": son entradas de la
tabla por hash, y **se usan solo sobre la copia viva y solo si su hash esta en
la tabla**. Toda escritura de parche verifica los bytes esperados antes de
escribir (los `patch_*` ya matchean forma; lo que falta es el rechazo cuando el
hash del modulo no es conocido, y que `sites: 0` sea un error de fase, no una
linea de log).

Los invariantes, evaluados una vez, en el primer `slGetNewFrameToken` (para
entonces todo el grafo ya cargo):

  - exactamente un `sl.interposer.dll` mapeado;
  - exactamente un `sl.dlss_g` vivo, con hash conocido, con sitio de cuenta y
    sitio de pacer;
  - exactamente un `nvngx_dlssg` mapeado, con hash conocido;
  - la semantica de la cuenta detectada sobre *ese* snippet coincide con la de
    la tabla para *ese* hash.

Si se cumplen: fase VERIFICADO y se pasa a ACTIVO. Si no: fase PASIVO, cero
parches de runtime, cero reescritura de opciones, y un bloque de log que dice
que invariante fallo y con que modulos. **Un juego nuevo que rompe la
topologia produce ese bloque, no un dump.** Las banderas del tipo `g_wic_ok`
desaparecen: "hay sitio de cuenta" es una propiedad de la copia viva que se lee
del registro, no una global que un modulo sin sitios apaga para toda la sesion.

### Capa 1 — Suministro: que se carga

Una sola decision, tomada en `DllMain` a partir de hechos en disco, y un solo
mecanismo para ejecutarla.

La decision es binaria: `SDK_PROPIO` si existe la carpeta
`%LOCALAPPDATA%\mfg-unlock\sdk\2.12` completa; `PASSTHROUGH` si no. Nada de
veredicto de la corrida anterior, nada de consentimiento por juego, nada de
"llevar los plugins a la version del interposer": `deteccion-del-set.md` ya
midio tres de tres que el interposer 2.7 del juego con nuestros nueve plugins
2.12 es indistinguible de la linea base en Cyberpunk. Ese cruce es el caso
normal, no la excepcion.

El mecanismo tiene dos partes que se refuerzan:

**a) `hk_slInit` deja de encender OTA y pasa a apagarla y a nombrar nuestra
carpeta.** En `Preferences`, con `flags` en +88 (ya verificado), quedan
`pathsToPlugins` en +40 y `numPathsToPlugins` en +48. El hook escribe un
arreglo estatico con nuestra ruta, pone `numPathsToPlugins = 1` y limpia los
bits 3 (`eAllowOTA`) y 6 (`eLoadDownloadedPlugins`). Con eso el plugin manager
del interposer *del juego* enumera nuestra carpeta y nunca mira la cache de
NGX: la copia OTA 2.14 de Cyberpunk y de Halo no llega a mapearse, y las
"2-4 copias" y "3 copias" — que casi seguro son el plugin manager mapeando
candidatos para leerles el JSON y descartandolos — se reducen a una. Esto
corre despues de todos los `DllMain`, asi que funciona igual con el interposer
en el import #5 de Cyberpunk que con la carga dinamica de Halo a los 16 s.

Cuidado con una cosa que hay que medir en el banco antes de confiar: el plugin
manager, cuando encuentra el mismo plugin en dos rutas, se queda con el mas
nuevo ("A duplicate was found, but a newer plugin version was available"). Si
el juego trae 2.13 en su carpeta y nosotros 2.12, puede preferir el suyo aunque
`pathsToPlugins` apunte al nuestro. Por eso existe la parte b.

**b) El gancho de `LdrLoadDll` queda, pero con una sola regla y sin
politica.** Si el nombre base del pedido es uno de los diez nombres conocidos
(nueve `sl.*.dll` y `nvngx_dlssg.dll`), o la ruta normalizada esta bajo
`\ngx\models\`, y la ruta pedida no esta ya en nuestra carpeta: se carga el
nuestro con `LoadLibraryW` y se devuelve ese handle (`cargar_propio`, que es
lo que arreglo el fallo 5). Nunca el interposer. Nunca una segunda copia,
porque el handle lo lleva Windows. Nada de leer versiones ni de comparar con
el set objetivo bajo el loader lock. Se borran `armar_set_objetivo`,
`leer_veredicto_previo`, `guardar_consentimiento`, `emitir_veredicto_si_toca`,
`veredicto_del_set`, `buscar_interposer`, `g_set_inter`, `g_inter_fuera`,
`g_twocopies` y todo lo que cuelga de "ROJO".

El consentimiento cambia de pregunta: ya no es "te reemplazo el Streamline de
este juego", es una sola vez, global, "usar el SDK 2.12 propio: si / no". Es la
misma decision que el usuario toma al copiar `version.dll`.

Si a pesar de a y b el invariante de capa 0 falla — un juego que verifica
integridad, un launcher que inyecta, un NGX que trae un snippet por otro
camino — la respuesta es PASIVO con diagnostico, y ahi si se evalua si vale
una entrada en una tabla de quirks **por hash de modulo** (no por nombre de
exe). Con el alcance de este proyecto (Ada, SL DLSS-G, DX12 y Vulkan) espero
una docena de entradas a lo largo de su vida, no 150.

### Capa 2 — Politica: que se le pide al plugin

Una funcion pura, sin globales, con esta firma:

```
struct GameRequest  { mode, count, structVersion, viewport };
struct Dialects     { game: GENERADOS|MULTIPLICADOR; ours: GENERADOS|MULTIPLICADOR };
struct Decision     { target_x100 (del modo fijo o del controlador), cap };
struct PluginRequest{ mode, count };

PluginRequest resolve(GameRequest g, Dialects d, Decision dec);
```

Reglas, y a que fallo mata cada una:

  - **El modo no se toca.** `eOff` va `eOff`; `eOn`/`eAuto` es la unica
    condicion bajo la que se aplica el override. Forzar `eOn` sobre un juego
    que escribio `eOff` 170 veces por segundo es pelear contra la pausa
    (fallo 3). Hoy `force_into` escribe `eOn` en 1108 y 1301 antes de
    preguntar; el orden correcto es al reves.
  - **La cuenta se traduce, no se reemplaza.** `mult_pedido =
    a_multiplicador(g.count, d.game)`; `mult_objetivo = dec.target` si hay
    modo fijo o DYNAMIC, si no `mult_pedido`; `count_out =
    desde_multiplicador(clamp(mult_objetivo, 2, dec.cap), d.ours)`. El
    dialecto del juego se determina **una vez, en disco**: se escanea el
    `.text` del `nvngx_dlssg.dll` que el juego trae en su carpeta con el mismo
    patron de `detectar_semantica` (653), sin mapearlo; si no trae ninguno, se
    asume GENERADOS, que es lo que documenta el header. El dialecto nuestro es
    una constante de la tabla por hash del snippet vivo. Halo pidiendo
    `count 1` se lee como 2X en su dialecto y se escribe como 2 en el nuestro
    (fallo 2). La semilla de DYNAMIC es `mult_pedido`, nunca un literal
    (fallo 4).
  - **Idempotencia.** Si el juego escribe lo mismo 170 veces por segundo,
    `resolve` devuelve lo mismo 170 veces; "cambio la cuenta" se define como
    `PluginRequest` distinto del ultimo *enviado*, no como llamada recibida.
  - **Un solo escritor.** `slDLSSGSetOptions` hacia el plugin se emite solo
    desde el hilo y el camino en que el juego la llama. El controlador no
    llama: decide, y su decision entra en `dec` en la proxima llamada del
    juego. La repeticion desde present (`apply_override_now`) queda solo para
    juegos que llaman una vez (DOOM via cvar), condicionada a que el juego no
    haya llamado en los ultimos N ms, y pasa por el mismo `resolve`.
  - **El dwell vive en el tipo.** `Decision.target` es un `Latch<LONG>` cuyo
    `set()` rechaza cualquier cambio si no pasaron 100 ms *y* K presentaciones
    desde el ultimo cambio aplicado, y ademas exige que el cambio anterior se
    haya observado efectivo (que `leer_max_generados`/el estado del plugin
    reflejen la cuenta escrita). 4 → 2 → 4 en 94 ms se vuelve imposible de
    escribir, no algo que el controlador "no deberia hacer" (fallo 6). La
    histeresis de 0.10 que ya tiene `dyn_apply` (4233) se conserva; lo que
    falta es que el tiempo minimo entre cambios este en el mismo lugar que el
    cambio.

El fraccional (el byte del bound que reescribe `fractional_tick`) es una
extension de esta capa con un invariante propio que hoy esta en comentarios
(1111-1121) y no en codigo: `byte + 1 <= reserva declarada`. Va como
`assert` en el unico lugar que escribe el byte, y el bound se escribe solo en
fase ACTIVO.

### El estado

Cuatro structs reemplazan a los 248 globales:

  - `Config` — inmutable despues de `DllMain`. Los 38 archivos-bandera y
    numericos pasan a un `mfg-unlock.ini` al lado del dll (mas el panel), que
    se parsea una vez. Esto no arregla ningun fallo por si solo, pero baja la
    combinatoria que el banco tendria que cubrir de 2^38 a "los perfiles que
    existen", y saca de `DllMain` 290 lineas.
  - `ImageRegistry` — capa 0. Escrito solo en el callback del loader; leido
    por snapshot (copia bajo un SRW lock) desde los demas hilos.
  - `Session` — `enum Fase { ARMADO, VERIFICADO, PASIVO, ACTIVO }`, monotona,
    `std::atomic`. Toda funcion que parchea o reescribe opciones empieza con
    `if (fase != ACTIVO) return;`.
  - `Controller` — el estado de DYNAMIC (anillo, deuda, sesgos, saturacion),
    propiedad exclusiva del hilo de render. Nadie mas lo lee; publica su
    salida a traves del `Latch`.

### Verificacion

Tres piezas, en orden de rendimiento por hora:

**Trazas y replay.** En fase ACTIVO, con `mfg-trace` encendido, cada
`slDLSSGSetOptions` deja una fila CSV: `t_us, hilo, mode_in, count_in,
structVersion, mode_out, count_out, fase`. Diez segundos de Halo, de GTA V con
pausa y de Cyberpunk con su swapchain descartable son tres fixtures. Un test
en C++ (compila con el mismo MinGW; `tools/test_metering_patch.cpp` ya es el
molde) alimenta las filas a `resolve` con los dialectos de cada juego y
compara. Los fallos 2, 3, 4 y 6 se reproducen en milisegundos y sin GPU.

**Topologias en el sample.** El sample importa el interposer estaticamente
(como Cyberpunk) y el fixture `ota:132874` ya produce dos copias. Faltan dos
casos que hoy no existen y que cuestan una tarde: una copia extra de
`sl.dlss_g` 2.7 en la carpeta del sample (Halo), y un lanzador que alterne
`eOn`/`eOff` cada 6 ms durante 5 s (GTA V en pausa). Con la capa 1 puesta, el
criterio de verde para estos deja de ser "ratio en banda": es **"fase
VERIFICADO con los hashes esperados, una copia de cada rol, y cero lineas de
invariante"**, ademas del ratio. `regresion.py` hoy solo cuenta
`INVARIANTE ROTO`; pasa a leer el bloque de fase.

**El juego real como aceptacion, no como descubrimiento.** La vuelta por Halo,
GTA V y Cyberpunk sigue siendo obligatoria, pero lo que se mira es el bloque
de fase del log. Si dice ACTIVO con los hashes esperados y el juego se
comporta mal, el defecto esta en capa 2 y se captura con una traza. Si dice
PASIVO, el defecto esta en capa 0 o 1 y el bloque nombra el modulo. Ya no hay
"aparece un crash, se lee el log, se agrega una regla".

## 3. Mapa de los seis fallos al diseno

| fallo | donde muere | por que no vuelve |
| --- | --- | --- |
| 1 Halo, copia 2.7 detras de la nuestra | capa 0 + 1 | la copia viva se identifica por el puntero que devuelve `slGetFeatureFunction`; un modulo con hash desconocido no se parchea ni apaga nada; con OTA apagada y el handle devuelto, no mapea una segunda copia |
| 2 Halo, `count 1` leido como 1X | capa 2 | traduccion de dialectos: GENERADOS del juego (en disco) a MULTIPLICADOR nuestro (por hash) |
| 3 GTA V, `eOn` sobre `eOff` en pausa | capa 2 | el modo es passthrough por regla, no por rama |
| 4 semilla DYNAMIC = 1 | capa 2 | la semilla es el multiplicador que pidio el juego, traducido |
| 5 Cyberpunk, dos interposers | capa 1 | el interposer no se sustituye nunca; los plugins entran por handle |
| 6 Cyberpunk, 4→2→4 en 94 ms | capa 2 | `Latch` con dwell de 100 ms + K presentaciones + confirmacion; un solo escritor |

## 4. Plan, del mas barato al mas caro

Se hace por extraccion, no por reescritura: los `patch_*` son funciones puras
de `(base, len)` y se mueven tal cual; el panel, el overlay, el recorder y la
instrumentacion de banco salen del camino critico a archivos propios. Cada
fase deja el binario instalable y se mide con el gate.

**Fase A (2 dias). `Session` + `Config` + reporte de invariantes.** Sin
cambiar comportamiento: se agrega la fase, el registro por notificacion con
hash, la identificacion de la copia viva por `slGetFeatureFunction`, y el
bloque de diagnostico en el primer token. Todo parche de runtime y
`force_into` quedan gateados por `fase == ACTIVO`. Desde este dia, un juego
nuevo produce un bloque de log en vez de un dump. Se mide: los tres juegos
tienen que dar ACTIVO con los hashes de la carpeta 2.12.

**Fase B (2 dias). `resolve()` + trazas + replay.** Se extrae la politica de
`force_into` y de `hk_slDLSSGSetOptions` a la funcion pura; se agrega el CSV;
se graban los tres fixtures; el test de replay entra al gate. Mata 2, 3 y 4.

**Fase C (1 dia). `Latch` y un solo escritor.** Se reemplazan los nueve
`g_opt_pending = 1` por `latch.set()`; `apply_override_now` se restringe a
"el juego no llamo en N ms". Mata 6.

**Fase D (2 dias, con banco). Capa 1.** `hk_slInit` apaga OTA y nombra la
carpeta; el gancho de `LdrLoadDll` se reduce a la regla unica; se borra el
subsistema de veredicto y consentimiento. Se mide en el sample con
`ota:132874` (dos copias → una) y con el fixture nuevo de copia 2.7. Mata 1
y 5 en su forma actual y, mas importante, mata la *clase*: la topologia que
llega a capa 2 es siempre la misma.

**Fase E (1 dia). Ini en vez de 38 banderas, y limpieza.** Se borran los
globales que quedaron sin lector.

Total del orden de ocho dias de trabajo, con el gate corriendo entre fases.
Las fases A, B y C no tocan como se carga nada, asi que no pueden romper un
juego que hoy anda; solo cambian que pasa cuando algo no cuadra. D es la unica
que cambia la topologia y es la que tiene banco.

## 5. Lo que este diseno NO promete

No promete que Halo funcione a 4x con el interposer 2.7.30: promete que si no
funciona, el log dice ACTIVO o PASIVO y en cual invariante murio, y que el
arreglo, si existe, es una entrada en una tabla por hash o una fila en un
fixture de replay. No promete cero quirks; promete que cada quirk tenga una
clave verificable (un hash) en vez de un nombre de ejecutable, y que ninguna
politica se escriba en el gancho del loader. Y no promete que el banco apruebe:
como dice `regresion.py`, el banco descarta, el juego decide. Lo que cambia es
que el juego decide sobre una topologia que ya fue verificada, no sobre una
que se descubre en el dump.
