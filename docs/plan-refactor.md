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

## Que necesita una corrida

Build 679861 (71aa84c) instalado en los tres el 2026-09-11, mode 6. Trae
ademas el arreglo del contador de Cyberpunk (a5a35ce). Que mirar:

- Cyberpunk desde Steam: `runtime PresentCount this window` avanza y el HUD
  muestra fps. La linea nueva `present: vtable ya enganchada; se adopta la
  instancia nueva` tiene que aparecer despues de `swapchain con ventana nueva`.
- F0/F1/F2 en los tres: `numFramesToGenerate` recibido == 6, las lineas
  `force_into:` iguales a las del log `*.pre-refactor-prev.log`, un solo
  `VEREDICTO ACTIVO` con `ejecuta_cuenta>0 ejecuta_pacer>0`, y ninguna
  `INVARIANTE`. `config: claves leidas` no aparece (nadie tiene
  mfg-config.txt todavia).
- Si algo difiere: bisecar por commit (a5a35ce, 67c76a5, a422a66, 486173e,
  71aa84c); cada uno compila solo con `sh build-proxy.sh`.
