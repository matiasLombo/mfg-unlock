# Objetivo autónomo — mfg-unlock

Trabajá hasta terminar. Parás solo cuando necesites una corrida.

## Objetivo
Que mfg-unlock deje de romperse juego por juego: cada juego nuevo debe dar, a lo sumo, una línea de diagnóstico y un fixture, nunca una regla reactiva. Concreto: Halo, GTA V y Cyberpunk entregando el multiplicador pedido (2x–6x) sin crash, sobre la copia que EJECUTA.

## Reglas de método (la causa de todo lo que salió mal)
1. Medí, no argumentes: ninguna conclusión sin dato del log o del binario.
2. Verificá el efecto en la copia viva (la que devuelve `slGetFeatureFunction`), no la existencia del sitio.
3. Antes de llamar algo "la causa", comprobá que produzca el efecto ("supports 5" no recorta un 4).
4. Un experimento por corrida. No mezcles modos ni flags.
5. Distinguí capacidad de estado, y medido de hipótesis.

## Verificado (no re-litigar)
- Proxy carga, engancha `slInit` en Halo, sustituye el interposer por el 2.12 propio.
- Fix del crash de present por vtable escrito y compilado (tabla por vtable, `present_original_de`; ver `docs/present-por-vtable.diff`). Halo corrió 285 s sin excepción (antes 53 s).
- Halo mode 4 fijo: entrega 2.007x. `sl.log` confirma que el plugin recibió `numFramesToGenerate=4` y quedó `eOn`: la escritura llega, el halving pasa DESPUÉS. Semántica MULTIPLICADOR correcta.
- Anomalía real que NO explica el halving sola: `SL Plugin supports 5` mientras el log dice "tope subido a 6" — el parche cayó en una copia que no es la viva.

## Hilo abierto — por qué 4 entrega 2x
Hipótesis: A) nuestros parches (pacer, sub-frame, tope) están en la copia que NO ejecuta y la viva corre el nativo de Ada = 2x; B) se generan pero se caen (sin flip metering; sin `mfg-nometer.txt` se descartan).
Sin recompilar: `grep -iE "skip the present|out of order|drop" sl.log` (~2 skip por presente → B); `grep -o "CPU pacer enabled, sites: [0-9]*"` y "in ...dlss_g" (a qué base cayeron pacer y tope).
Después, una línea (Capa 0): en `hk_slGetFeatureFunction` logear en qué copia `[base,base+size)` cae el puntero devuelto; compará con qué copia recibió pacer/tope. Difieren → A; el arreglo es dirigir todos los parches a la copia viva.

## Pendiente sin mezclar — probar el fix de vtables
Sigue sin ejercitarse: la 2ª vtable solo aparece cuando el juego recrea el swapchain (cambio de modo de pantalla/resolución o alt-tab en fullscreen). Corrida dedicada: forzá esa recreación. Éxito = "otro swapchain con vtable distinta" + "vtables enganchadas 2" + "llamada anidada" y sin crash. Mueve la cuenta; no mezclar con la línea base.

## Fin real — arquitectura por capas (`docs/arquitectura-por-capas.md`)
Por extracción, midiendo entre fases con el gate:
- A: `Session` (fase ARMADO→VERIFICADO|PASIVO→ACTIVO) + registro por hash + copia viva por `slGetFeatureFunction` + invariantes en el primer token; parches y `force_into` gateados por ACTIVO.
- B: `resolve()` puro (modo passthrough, cuenta traducida entre dialectos) + trazas CSV + replay.
- C: `Latch` con dwell (100 ms + K presentaciones + confirmación), un solo escritor.
- D: `slInit` apaga OTA y apunta `pathsToPlugins`; `LdrLoadDll` a una regla sin política (verificá primero si `pathsToPlugins` gana sobre la copia del juego).
- E: `.ini` en vez de flag-files; borrar globales muertos.

## Protocolo con Matías
No podés lanzar los juegos: los corre él. Por corrida: dejá el build instalado, `mfg-settings.txt` en el modo del experimento y el log borrado; decí en una línea qué hacer; con "listo" leé logs y reportá medido / sin probar / siguiente. Compilá con `sh build-proxy.sh`.

## Terminado
- Los tres juegos entregan el multiplicador pedido 2x–6x (`numFramesToGenerate` recibido == pedido y presentadas/renderizadas ≈ pedido), sobre la copia viva.
- Fix de vtables probado con recreación forzada, sin crash.
- Un juego con topología rota cae en PASIVO con diagnóstico, no crash.
- Fases A–C verdes; D–E si alcanza.
