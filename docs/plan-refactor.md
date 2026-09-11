# Refactor: escalable, mantenible, depurable

Objetivo y reglas: el goal del 2026-09-11. Esto es el orden de trabajo y el
estado, para que cualquier sesion sepa donde esta parada.

Cada fase son DOS commits como minimo: extraer (archivo nuevo + test, proxy.cpp
intacto) y cablear (proxy.cpp llama al archivo nuevo, comportamiento identico).
Mejorar es un tercer commit, con su medicion, o no se hace.

Antes de cada instalacion: `sh tools/test-host.sh`.

| fase | que se extrae | a donde | test de host | estado |
|---|---|---|---|---|
| F0 | la decision de `force_into` | `src/politica.h` | `tools/test_politica.cpp` (30 casos de los logs) | extraido b02a46e, cableado 67c76a5; falta correr los 3 juegos |
| F1 | configuracion: 43 archivos + `mfg-settings.txt` | `src/config.h` | `tools/test_config.cpp` (carpetas reales) | extraido b60b811, cableado a422a66, `mfg-config.txt` 486173e; falta correr |
| F2 | diagnostico: una linea por capa e invariante | `src/diag.h` | `tools/test_diag.cpp` (formato exacto) | extraido 9107123, cableado 71aa84c; falta correr |
| F3 | el controlador DYNAMIC (`dyn_control`, `dyn_apply`, sat, sesgo) | `src/controlador.h` | `tools/test_controlador.cpp` (la tabla de GTA V, 28 casos) | extraido ee4cca5, cableado 3ac6c14; Cyberpunk DYNAMIC 72% en banda |
| F4 | el reparto por bloques de `fractional_tick` | `src/reparto.h` | `tools/test_reparto.cpp` (juego simulado, 2.10-2.90 al 1%) | extraido c8d7384, cableado 5c518e6; Cyberpunk CUSTOM 2.55 -> 2.55 |
| F5 | los sitios de los `patch_*` que corren en sesion | `src/sitios.h` | `tools/test_sitios.cpp` (los dll reales de la cache 2.12) | extraido b345fd6, cableado 2f02034; Cyberpunk 6X -> 5.98, mismos conteos |
| F6 | `resolve.h` contra `politica.h` | -- | en `test_politica`, seccion F6 | a0e8a88: coinciden en fijos y pausa; DOS divergencias pinchadas, no unificadas (necesitan juego) |

## Lo que salio de F0 y no se corrigio (a proposito)

- La guarda "nunca vimos que cuenta pide el juego" solo salta con el snippet de
  julio. Con multiplicador, un 0 del juego se traduce a 1, pasa como dato, y
  el freno escribe 1 (= 1X). Pinchado en `test_politica`; corregirlo es un
  cambio de comportamiento y necesita un juego que lo produzca.
- `force_into` asume que el juego habla GENERADOS siempre. Es cierto para los
  tres juegos medidos (snippet de julio en los tres); no esta derivado del
  binario del juego.

## Corridas hechas (Cyberpunk desde Steam, `tools/run_cp_focus.ps1 -Steam`)

El benchmark de Cyberpunk lanzado por Steam es la unica corrida desatendida
con la topologia real (overlay, sin hook de Present). 2026-09-11, todas con
foco sostenido, cero `window not focused`:

| build | modo | resultado | log |
|---|---|---|---|
| 71aa84c | 6 | adopcion OK; 351 "disagrees", 0 PresentCount: el contador alimentaba la cuenta del hook | `refactor-steam-1-prev` |
| afd69fe | 6 | 386 ventanas, 58 de juego, base 35, **5.93x** (p10 5.50, p90 6.20), 0 INVARIANTE, 0 excepciones | `cp-6x-steam-ok` |
| afd69fe | 8 | **crash a los 5.6 s** en ntdll (escritura en 0x36, luego fatal en ntdll+0xfa7d), justo tras la adopcion | `dyn-refactor-crash-prev` |
| a5a35ce | 8 | corrida entera, 421 ventanas, 0 excepciones (sin PresentCount: bug previo) | `dyn-a5a35ce-ok-prev` |
| 71aa84c | 8 | corrida entera, 410 ventanas, 0 excepciones (idem) | `dyn-71aa84c-ok-prev` |
| afd69fe | 8 | corrida entera, 401 ventanas, 65 de juego, presentadas 176 (p10 155, p90 184), **68% dentro del 5% de 180**, 0 INVARIANTE | `cp-dyn-steam-ok` |

| 3ac6c14 (F3) | 8 | 437 ventanas, 61 de juego, presentadas 174 (p10 164, p90 184), **72% dentro del 5% de 180**, 52 `dynbias`, 0 INVARIANTE | `cp-dyn-f3-ok-prev` |
| 5c518e6 (F4) | 7 (CUSTOM 2.55) | 421 ventanas, 70 de juego, base 42, **2.55 mediana** (p10 1.93, p90 2.97 por ventana), 134 cambios por lado, 0 INVARIANTE | `cp-custom255-f4-ok-prev` |
| 2f02034 (F5) | 6 | 415 ventanas, 54 de juego, base 35, **5.98** (p10 4.32, p90 6.58), conteos de sitios identicos (2/2/2/1/1/1/1), 0 INVARIANTE | `cp-6x-f5-ok` |

| a0e8a88 (final) | 8 | dos corridas: **46%** y **58%** dentro del 5% de 180 (medianas 170 y 174), 0 excepciones | `dyn-final-2-prev`, `dyn-final-3-prev` |
| 3ac6c14 (pre-F4) | 8 | dos corridas: **74%** y **58%** (medianas 176 y 172), 0 excepciones | `dyn-pref4-1`, `dyn-pref4-2` |

DYNAMIC va de 46% a 74% en banda con el MISMO binario (58% aparece en los
dos); es ruido de corrida, no F4/F5 ([[measure-the-noise-first]]). Con seis
corridas DYNAMIC mas, el crash del inicio no volvio: 1 de 8.

Las lineas de politica (`cuenta: el modo fijo pedia 5`, `override:`,
`numFramesToGenerateMax CAMBIO a 6`) son identicas antes y despues del rewire
de F0 (hud-cero-prev contra cp-6x-steam-ok). F0-F2 invisibles en Cyberpunk.

**El crash de DYNAMIC (1 de 3 en el mismo binario) no esta explicado.** No se
reprodujo en dos repeticiones ni en los dos builds anteriores. Primera
excepcion: ntdll+0x64125 escribiendo en 0x36 con `cuenta pedida 2, aplicada
-1, byte vivo 1`, a los 5656 ms, entre la adopcion del swapchain descartable y
el swapchain del juego. Queda como hipotesis abierta, no como causa; si vuelve,
ese log es el primero que hay que mirar.

## Donde quedo (2026-09-11, build 681341 = a0e8a88, instalado en los tres)

- `src/proxy.cpp`: 10.200 -> 9.408 lineas; 248 -> 216 globales; 0 `flag_file`
  sueltos. Siete headers puros con siete tests de host (`sh tools/test-host.sh`,
  ~1 s): politica, config, diag, controlador, reparto, sitios, resolve.
- Objetivos del goal: diagnostico en una linea (F2, `docs/diagnostico.md`);
  politica probable en el host (F0, F3, F4, F6); configuracion en un archivo
  (F1, `docs/configuracion.md`); parches verificables contra los dll reales
  (F5). "Un modulo en un archivo" avanzo para esos siete; los hooks, la
  carga del set, la ventana de medicion y el HUD siguen en proxy.cpp.
- Cada rewire quedo verificado en Cyberpunk desde Steam (la topologia real);
  GTA V y Halo tienen el build instalado y sin correr.

## Que necesita una corrida con alguien jugando

- GTA V y Halo con build 681341 (instalado, mode 6): `VEREDICTO ACTIVO`, 0
  `INVARIANTE`, `numFramesToGenerate` recibido == 6, y el multiplicador
  entregado como antes (Halo 4X 4.02 / 6X 5.98). GTA V no tiene benchmark
  ([[gtav-benchmark-roto]]) y Halo no genera en el menu.
- Las dos divergencias de F6 (Halo con eOff sin eOn previo; sel 0 con base
  sustituida) solo se pueden decidir con esos juegos.
