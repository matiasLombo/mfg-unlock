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
| F3 | el controlador DYNAMIC (`dyn_control`, `dyn_apply`, sat, sesgo) | `src/controlador.h` | ventanas grabadas de GTA V/Cyberpunk | |
| F4 | el planificador fraccional (`fractional_tick`, latch, bloques) | `src/reparto.h` | cadencias medidas ([[fractional-by-block-alternation]]) | |
| F5 | los parches de binario (`patch_*`, `image_has`) | `src/parches.h` | busqueda de sitios sobre el snippet real de la cache | |
| F6 | `resolve.h` y `politica.h` reconciliados caso por caso | uno solo | los dos tests, unidos | |

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

Las lineas de politica (`cuenta: el modo fijo pedia 5`, `override:`,
`numFramesToGenerateMax CAMBIO a 6`) son identicas antes y despues del rewire
de F0 (hud-cero-prev contra cp-6x-steam-ok). F0-F2 invisibles en Cyberpunk.

**El crash de DYNAMIC (1 de 3 en el mismo binario) no esta explicado.** No se
reprodujo en dos repeticiones ni en los dos builds anteriores. Primera
excepcion: ntdll+0x64125 escribiendo en 0x36 con `cuenta pedida 2, aplicada
-1, byte vivo 1`, a los 5656 ms, entre la adopcion del swapchain descartable y
el swapchain del juego. Queda como hipotesis abierta, no como causa; si vuelve,
ese log es el primero que hay que mirar.

## Que necesita una corrida con alguien jugando

- GTA V y Halo con build afd69fe (instalado, mode 6): `VEREDICTO ACTIVO`, 0
  `INVARIANTE`, `numFramesToGenerate` recibido == 6. GTA V no tiene benchmark
  ([[gtav-benchmark-roto]]) y Halo no genera en el menu.
