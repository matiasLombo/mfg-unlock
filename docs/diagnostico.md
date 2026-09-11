# Diagnostico: que buscar en mfg-unlock.log

Un defecto produce UNA linea, y esa linea dice la capa y el invariante:

```
grep -n "INVARIANTE\|VEREDICTO" mfg-unlock.log
```

Formato (`src/diag.h`, fijado por `tools/test_diag.cpp`):

```
INVARIANTE <capa> <nombre>: <detalle> clave=valor clave=valor ...
VEREDICTO <fase>: <detalle> clave=valor ...
```

Las capas son las de `docs/arquitectura-por-capas.md`. `tools/regresion.py`
cuenta las lineas `INVARIANTE ` y marca la corrida si hay alguna.

## Catalogo

| linea | capa | que significa | que mirar |
|---|---|---|---|
| `VEREDICTO ACTIVO` | sesion | la topologia se identifico; se parchea y se reescriben opciones | `ejecuta=N` es la copia viva; `ejecuta_cuenta` y `ejecuta_pacer` > 0 |
| `VEREDICTO PASIVO` | sesion | el interposer resolvio una funcion y no cae en ninguna copia; el mod no actua | `copias`, `vivas`, `resuelto`; ver `copia_que_ejecuta` |
| `INVARIANTE capa0/identidad copia-parcheada` | 0 | la copia que ejecuta no tiene cuenta o pacer parcheados | `cuenta=0` o `pacer=0`: el snippet/plugin cambio de build ([[mfg-ota-updates-break-patches]]) |
| `INVARIANTE capa2/politica cuenta>=2` | 2 | con multiplicador se iba a escribir cuenta 1 (= 1X); corregida a 2 si manda el parche | `sel`, `gen`, `objetivo`; si aparece seguido, el culpable es quien escribe `g_force_generated` |
| `INVARIANTE capa2/politica cuenta-del-juego` | 2 | sin parche y sin haber visto nunca la cuenta del juego: no se toca nada | `visto`; solo salta con el snippet de julio (ver plan-refactor, F0) |
| `INVARIANTE capa3/presentacion contador` | 3 | `GetLastPresentCount` fallo; el HUD queda en cero | `hr`; el swapchain adoptado esta muerto o no es el del juego |

Lo que NO es invariante y sigue en su formato: las ventanas de medicion
(`measured:`, `runtime PresentCount this window`), las decisiones
(`force_into:`, `override:`, `freno:`), y la carga (`base:`, `snippet:`).
