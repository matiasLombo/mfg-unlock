# Medir fluidez y stuttering: que sirve, que no, y que quedo descartado

> **DOS CORRECCIONES de [[dynamic-medido-bien]] (docs/dynamic-medido-bien.md).**
> (1) Todo lo que abajo se compara por "fuera de cadencia" usa una banda
> ABSOLUTA de 4-8 ms, que mide en parte la distancia a 165 fps: las
> comparaciones entre configuraciones con distinto fps no valen, incluido
> "DYNAMIC 30 veces peor" y "Present tarda 40 % contra 11 %", que a tasas
> comparables dan 1.8x y ninguna diferencia.
> (2) La linea que el runner reportaba como "DLSS-G: presentados/renderizados"
> NO era de DLSS-G ni independiente: se calcula como frames renderizados por la
> cuenta que pedimos, o sea que confirma nuestra propia peticion. Donde abajo
> diga que dos instrumentos coincidieron, habia uno solo.


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

## DYNAMIC contra 4X fijo -- SUPERADA por la seccion siguiente

Se deja porque muestra el error: se comparo DYNAMIC a 5.4x contra un fijo a
4.0x, o sea distinto punto de operacion. Sus dos conclusiones -- "DYNAMIC no
hitchea mas" y "un cuarto de sus hitches caen en un cambio de cuenta" -- las
corrige el control apareado de mas abajo. LAS DOS eran falsas.

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

## DYNAMIC contra un fijo APAREADO: el control que faltaba

La comparacion anterior era contra 4X mientras DYNAMIC corria a 5.4x. Rehecha
contra 5X fijo, dos corridas de cada uno:

    metrica                 4X fijo    5X FIJO         DYNAMIC 5.4x
    multiplicador           4.00       5.00            5.46 / 5.40
    presentaciones/s        133        192             161
    FUERA DE CADENCIA       23.8 %     1.4 % / 1.0 %   31.0 % / 30.8 %
    Present tarda (ventana) 34.7 %     10.9 % / 11.1 % 40.7 % / 40.6 %
    hitches >33ms           66 / 55    20 y 17 / 63    50 y 44 / 50 y 49
    pico peor               121 ms     108 y 127 ms    473.5 y 474.6 ms

**DYNAMIC es 30 veces peor que un fijo del mismo punto de operacion en cadencia,
y encima entrega menos fps (161 contra 192).** Ya no hay confound: el control
esta apareado y reproduce dos de dos.

### Y el costo NO esta en cambiar la cuenta

Era la sospecha natural. Normalizado por presentaciones -- no por conteo crudo,
que solo refleja cuantas presentaciones cayeron cerca -- estar cerca de un
cambio de cuenta NO aumenta el riesgo:

    corrida       cerca de un cambio    lejos      riesgo relativo
    DYNAMIC c1    29.3 % (1909 pres)    31.3 %          0.94
    DYNAMIC c2    28.6 % (1969 pres)    31.2 %          0.92

Reproducido. El "24 % de los hitches caen en un cambio de cuenta" que se
reporto antes era casi la proporcion de presentaciones que hay ahi, sobre 12
eventos. La tasa mide sobre 12.000 presentaciones y dice cerca ~ lejos.

### Lo que si correlaciona: cuanto tarda Present

    configuracion   Present tarda   fuera de cadencia
    5X fijo             11 %              1.2 %
    4X fijo             34.7 %           23.8 %
    DYNAMIC             40.6 %           30.9 %

Monotono en las tres. **CORRECCION de una version anterior de este documento:
ese numero NO es un freno nuestro.** `g_present_block_us` cronometra solamente
la llamada original `g_orig_dxgi_present`; es el pacer de DLSS-G reteniendo el
frame. Se habia escrito "bloqueamos Present un tercio del tiempo", y era falso:
no lo bloqueamos nosotros. Lo que se midio es que **DYNAMIC hace que el pacer de
DLSS-G se quede 4 veces mas adentro de Present que un multiplicador fijo del
mismo tamano**, y que la cadencia sigue a ese numero.

NO esta probado por que. Candidato sin probar: 5X fijo entrega 192 fps, por
encima del panel, y DYNAMIC apunta a 165 y entrega 161, justo en el refresh.

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
