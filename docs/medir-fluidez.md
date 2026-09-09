# Medir fluidez y stuttering: que sirve, que no, y que quedo descartado

Escrito el 2026-09-09 despues de medirlo en Cyberpunk con el set sustituido.

## El estado del arte, y por que no nos alcanza

La metrica que existe para esto es **Animation Error**, de Intel PresentMon 2.0:

    MsAnimationError = (AnimationTime_N - AnimationTime_N-1) - MsBetweenDisplayChange_N

O sea: cuanto tiempo de mundo paso entre dos frames, menos cuanto estuvieron en
pantalla. Mide el stuttering directamente en vez de deducirlo del frametime. El
`AnimationTime` sale de los marcadores `SimStart` de Reflex cuando el juego los
emite, y Cyberpunk los emite.

**Pero PresentMon no puede aplicarla a frames generados.** El white paper de
metodologia de GamersNexus lo dice sin vueltas: no hay animation time para los
frames sinteticos, asi que no hay referencia contra la cual calcular el error.
La herramienta estandar de la industria no puede medir justo nuestro caso.

Nosotros si podriamos, porque estamos adentro y sabemos cuantos sub-frames se
pidieron -- o sea donde DEBERIAN caer en tiempo de mundo, repartidos k/n entre
dos frames reales. **No esta implementado.** Es la hipotesis viva para "x4 no se
ve mas fluido que x2".

## El reloj estaba mal, y esta documentado

`ProgrammingGuideDLSS_G.md` de NVIDIA manda medir **MsBetweenDisplayChange y no
MsBetweenPresents**, porque DLSS-G retrasa la imagen por hardware DESPUES de
`Present()`. Todo nuestro instrumental de stuttering media intervalos entre
llamadas a `Present`.

Eso explica retroactivamente [[present-timing-is-not-fluidity]]: el pacer daba
vuelta la distribucion entera de intervalos y no cambiaba nada visible porque
movia un reloj que no se ve.

## Un instrumento que se construyo y se descarto midiendo

Primer intento: usar `SyncQPCTime` de `DXGI_FRAME_STATISTICS` como reloj de
pantalla. Daba 34-44 Hz de cambios de imagen contra un panel de 165 Hz, y 4.66
presentaciones por cambio -- un numero alarmante que encajaba perfecto con la
queja.

**Era falso, y lo mato el control.** En las ventanas SIN generacion, donde cada
presentacion es un frame real, el mismo artefacto aparecia mas fuerte:

    generando 4x    4.66 presentaciones por "cambio de imagen"   30 imagenes/s
    sin generar     6.42 presentaciones por "cambio de imagen"   49 imagenes/s

Un artefacto que aparece igual sin un solo frame generado no esta midiendo
frames generados. `SyncQPCTime` viene repetido: el driver no lo actualiza por
presentacion.

## Lo que si sirve: PresentRefreshCount

Cuenta refreshes del panel. Medido, ventanas separadas por poblacion:

    configuracion   panel    presentaciones/s   presentaciones por refresh
    generando 4x    134 Hz         133                    0.97
    sin generar     138 Hz         303                    2.25

**Cada presentacion se gana su propio refresh cuando generamos.** Los frames
generados SI llegan a la pantalla como imagenes distintas; no se pierde
ninguno. El caso sin generar valida el instrumento: 303 presentaciones contra
138 refreshes dan 2.25, que es lo que tiene que dar.

Queda descartada la hipotesis de "de cada lote de 4 llega una sola imagen".

## DYNAMIC contra 4X fijo, dos corridas cada uno

    metrica                        4X fijo          DYNAMIC 165
    multiplicador           4.00 (desvio 0.07)   5.46 / 5.40 (desvio 0.43 / 0.36)
    presentaciones/s               133                161
    hitches >33ms            66 en 55 ventanas   50 y 44 en 50 y 49 ventanas
    hitches en cambio de cuenta      0               12 y 11  (~24%)
    pico peor                    121.3 ms        473.5 y 474.6 ms
    presentaciones por refresh     0.97               1.10

**DYNAMIC no hitchea mas que el fijo** -- al contrario, tiene menos. Lo que
tiene propio son dos cosas: un pico de ~474 ms que reproduce a 1 ms entre
corridas, y que casi un cuarto de sus hitches caen en un cambio de cuenta, que
en el fijo son 0 por construccion.

**Cuidado al leerlo:** la escena del benchmark hitchea sola. 66 hitches con el
multiplicador clavado en 4.00 y desvio 0.07 no los causa nuestro controlador.

## El hallazgo que no se buscaba: bloqueamos Present un tercio del tiempo

    configuracion   bloqueado en Present, mediana de la ventana   peor
    4X fijo                      34.7 %                          574 ms
    DYNAMIC c1                   40.7 %                          760 ms
    DYNAMIC c2                   40.6 %                          770 ms

Nuestro pacer frena al juego adentro de `Present` mas de un tercio del tiempo,
en LAS DOS configuraciones. No es un defecto de DYNAMIC. No estaba medido en
ninguna parte. Ver [[pacer-blocking-budget]].

## El override SI controla en Cyberpunk con el set sustituido

    pedido    entregado (p90 de ventanas activas)
    3X                 3.02
    4X                 4.02

Esto corrige [[cyberpunk-override-does-not-apply]], que media 4.05 pidiendo 3X y
daba por invalida toda medicion del controlador ahi. El sospechoso numero 2 de
esa memoria era "dos copias de sl.dlss_g mapeadas", y es exactamente lo que la
sustitucion del set elimina: 18 sustituciones, 7 copias todas 2.12, ninguna OTA
2.14. **Cyberpunk vuelve a ser un banco valido para el controlador.**

## Metodo: dos trampas del entorno que costaron corridas

  - El runner mataba `CrashReporter` pero NO `QmlRenderer`, el otro proceso del
    reporte de fallos de CDPR. Con dos vivos, tres corridas seguidas entregaron
    1.00 sin que DLSS-G se activara. El sintoma que lo delataba estaba en la
    salida del runner: "foco reafirmado 0 veces" contra 2 en todas las buenas.
    Ya se agrego a la limpieza.
  - Se corrio una benchmark contra un binario que no se habia recompilado, por
    encadenar el build con el deploy sin mirar la salida. El guard de SHA256
    comprueba que el juego use el archivo del repo, NO que el archivo este al
    dia. Verificar el tamano o la fecha del binario antes de desplegar.

## Lo que este documento NO afirma

  - No afirma nada sobre el contenido de los frames generados. Que lleguen a la
    pantalla no dice que lleven tiempos de mundo repartidos.
  - No afirma que el pico de 474 ms de DYNAMIC sea culpa del controlador.
    Reproduce, pero no se aislo la causa.
  - No afirma que bajar el bloqueo en Present mejore la fluidez. Esta medido que
    existe, no que se sienta.
