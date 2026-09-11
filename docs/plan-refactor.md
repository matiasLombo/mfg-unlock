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
| F1 | configuracion: 31 flags + `mfg-settings.txt` | `src/config.h` | parser sobre los textos reales de los tres juegos | |
| F2 | diagnostico: una linea por capa e invariante | `src/diag.h` | -- | |
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

- F0: los tres juegos con el mismo modo que la ultima vez, y comparar
  `numFramesToGenerate` recibido y las lineas `force_into:` contra el log
  anterior. Si difieren, el rewire no fue identico.
