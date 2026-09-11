# Mapa de modulos

Todo es un solo translation unit: `src/proxy.cpp` incluye cada header en el
lugar donde su codigo vivia, asi que el orden de declaracion es el de
siempre. Cada header tiene una cabecera con que ENTRA, que SALE y de que
DEPENDE; esta tabla es el indice.

"Test" es lo que lo cubre sin GPU (`sh tools/test-host.sh`, ~1 s). "Corrida"
es la red para lo que necesita el juego: `tools/run_cp_focus.ps1 -Steam`
(Cyberpunk desde Steam, la topologia real) y `tools/check_run.py` contra la
corrida anterior. Un build es `sh build-proxy.sh`; un renombre puro se prueba
con `tools/compare_sections.py` (.text byte a byte).

## Puros (sin Windows, sin globales): la politica, probable en el host

| archivo | lineas | responsabilidad | test | depende de |
|---|---|---|---|---|
| `policy.h` | 147 | la decision de `force_into`: que modo y cuenta se escriben, con el freno y el invariante | `test_policy` (37 casos de los logs) | `resolve.h` |
| `resolve.h` | 103 | los dialectos (generados / multiplicador) y la politica deseada, con dos divergencias documentadas | `test_resolve`, seccion F6 de `test_policy` | -- |
| `config.h` | 332 | las 36 banderas y 7 numericos en tabla, `mfg-settings.txt`, `mfg-config.txt` | `test_config` (las carpetas reales) | -- |
| `diag.h` | 89 | `INVARIANTE capa nombre: ... k=v` y `VEREDICTO` en una linea | `test_diag` (formato exacto) | -- |
| `controller.h` | 232 | DYNAMIC: control por ventana, apply por frame, techo (sat) y sesgo por tramo | `test_controller` (la tabla de GTA V) | -- |
| `scheduler.h` | 156 | el reparto fraccional por bloques, latcheado por ciclo | `test_scheduler` (juego simulado, 2.10-2.90 al 1%) | -- |
| `adopted.h` | 95 | los swapchains adoptados: cual se cuenta (el que mas presento en el ultimo periodo, entre los vivos) y cuando se suelta; vida y cuenta se inyectan | `test_adopted` (los dos casos de GTA V del 11/09) | -- |
| `present_policy.h` | 75 | que se hace con un swapchain nuevo (registrar la vtable, escribir el slot, adoptar) segun overlay, dueno del slot, modo host, vtable ya vista; y la guarda de `write_slot` | `test_present_policy` (una fila por incidente medido) | -- |
| `sites.h` | 189 | los patrones de bytes de cada parche y el `.text` de un PE | `test_sites` (los dll REALES de la cache) | -- |
| `zipmini.h` | 258 | leer el zip del SDK (deflate) | `test_zipmini <zip>` | -- |

## Con Windows: los mecanismos, cubiertos por la corrida

| archivo | lineas | responsabilidad | corrida | depende de |
|---|---|---|---|---|
| `state.h` | 1138 | las 168 globales compartidas, cada una con quien escribe y quien lee (`tools/globals_report.py`) | -- | los tipos que declara |
| `config_apply.h` | 181 | de la configuracion leida a los globales, en orden, con los avisos | `config: claves`, `tope: 6X activo` en el log | `config.h`, todos los globales |
| `slinit.h` | 148 | el hook de `slInit`: leer y, si se pidio, cambiar las Preferences | `banderas en +88` en el log | `loader.h` lo instala |
| `reflex.h` | 268 | la latencia y la base por Reflex, el estimador de base del controlador | `latency sim to driver end us` por ventana | -- |
| `patches.h` | 1837 | las escrituras de parche sobre `sl.dlss_g` y `nvngx_dlssg` | los conteos de sitios (2/2/2/1/1) | `sites.h`, `cubins.h` |
| `measurement.h` | 436 | la ventana de medicion: foto (`snapshot_window`), volcado (`dump_window`), acciones (`close_window`); los `hud_*` | `runtime PresentCount this window`, el ratio de `check_run.py` | `reflex.h`, `controller.h`, `present.h` |
| `writer.h` | 1191 | el camino de escritura: `hk_slDLSSGSetOptions`, `force_into`, el latch, `set_count_now`, `fractional_tick` | `numFramesToGenerate` recibido == pedido, `force_into:` | `policy.h`, `controller.h`, `scheduler.h`, `patches.h` |
| `frametoken.h` | 233 | el pulso por frame: `hk_slGetNewFrameToken` y su armado | `VEREDICTO ACTIVO` (evalua invariantes ahi) | `measurement.h`, `writer.h`, `reflex.h`, `present.h`, `loader.h` |
| `recorder.h` | 494 | el grabador F9, el CSV y el hilo del panel | `pacing:` en el log, `mfg-frames.csv` | `present.h`, `overlay.h` |
| `loader.h` | 1682 | que set y que snippet corren: copias del plugin, veredicto, cache, `LdrLoadDll`, `on_dll_load` | `VEREDICTO ACTIVO copias=N vivas=1 ...`, `se carga el nuestro` | `patches.h`, `slinit.h`, `diag.h` |
| `present.h` | 1056 | el swapchain y Present: `write_slot` (el unico escritor de slots de vtable), la ejecucion de `present_policy`, la lista de adoptados comprobada antes de usarse, el contador del runtime | `present: overlay de Steam presente`, `se adopta la instancia nueva`, `el swapchain adoptado ya no existe` | `present_policy.h`, `recorder.h` (note_present) |
| `host.h` | 508 | modo host (`host 1`): nosotros como aplicacion de Streamline en un juego que no lo trae -- slInit, proxies de fabrica/device, tags y constantes desde los parametros de NGX, PCL y Reflex, `slDLSSGSetOptions` | `host: slInit -> 0`, `EvaluateFeature, llamadas`, PresentCount = 2x tokens en Metro | los headers del SDK, `present.h` (los hooks de fabrica y Present), `loader.h` |
| `exceptions.h` | 257 | el testigo de excepciones y la autopsia del sub-frame | `EXCEPCION ----` (que no aparezca) | -- |
| `overlay.h` | 1124 | el panel y el HUD (ventana propia, GDI) | `panel:` / `hud:` en el log | `state.h` |
| `proxy.cpp` | 1214 | DllMain en cuatro pasos, el log, el forwarding de `version.dll` y de `winmm.dll` (el mismo dll con cualquiera de los dos nombres; `g_own_name` decide), `hk_slGetFeatureFunction` (Capa 0), los helpers de PE | todo lo anterior | todo lo anterior |
| `forwards_winmm.S` + `forwards_winmm_names.h` + `exports.def` | generados | los 180 exports de `winmm.dll` como stubs que resuelven el real la primera vez (`tools/gen_forwards.py`); cuatro a mano no alcanzaban: Metro se reiniciaba en bucle | el juego arranca con `winmm.dll` | -- |
| `cubins.h` | 14659 | los kernels Blackwell recompilados, como bytes (`tools/rebuild_cubins.py`) | `gates and cubins OK` | -- |

## Como se lee un defecto nuevo

1. `grep "INVARIANTE\|VEREDICTO\|EXCEPCION" mfg-unlock.log` -- la capa y el
   nombre (`docs/diagnostico.md`).
2. El archivo de esa capa, arriba de todo: que ENTRA y que SALE.
3. Si la variable es compartida, `state.h` dice quien mas la escribe.
4. Si es politica, se reproduce en `tools/test_*.cpp` antes de abrir un juego.
