# M1: criterio de deteccion, escrito antes de codificar

Que decide si un juego "va a funcionar bien", con que se mide, y por que no se
puede decidir en el momento de la carga.

## El problema de orden, que anoche costo dos juegos rotos

Anoche la decision se tomaba dentro del hook de `LdrLoadDll`, mirando el archivo
que se estaba por cargar. Eso no alcanza y no puede alcanzar:

  - En Halo, la copia buena y la mala se cargan con **segundos** de diferencia, y
    la que sobrevive la elige Streamline despues, no nosotros.
  - En Cyberpunk hay **varias** copias; una sin el sitio disparaba la sustitucion
    aunque la copia que el juego realmente usa estuviera perfecta.

[[snippet-vs-streamline-axes]] ya lo decia de la otra mitad del sistema: *"la
linea `NGX will load OTA build <id>` es una **prediccion**, no una observacion"*.
Lo mismo vale para el plugin. **Cual copia queda viva es un hecho observado al
final, no deducible al principio.**

## Por eso: la primera corrida observa, la segunda actua

  1. Corrida 1: no se sustituye nada. Se observa y se escribe un veredicto.
  2. Corrida 2 en adelante: con el veredicto ya escrito, se decide antes de que
     el juego cargue nada, que es cuando se puede hacer algo util.

Es exactamente lo que pidio el usuario ("si detecta que es la primera vez que se
usa"), y ademas es la unica forma correcta dado lo de arriba.

El veredicto NO va al lado del juego: [[ships-as-one-dll]] es explicita. Va en
carpeta propia, indexada por la ruta del ejecutable.

## Los hechos que se observan

Todos salen de cosas que el dll ya imprime hoy. Ninguno es nuevo.

| hecho | de donde | ya existe |
| --- | --- | --- |
| copias de `sl.dlss_g` mapeadas | `sl.dlss_g mapped` | si |
| ruta y version de cada copia | `in <ruta>` + recurso del archivo | si |
| sitios de la cuenta por copia | `work item count is ours` / `sites: 0` | si |
| sitios del pacer por copia | `CPU pacer enabled, sites: N` | si |
| copias descargadas | `modulo descargado` | si |
| sitios vivos al final | `sitios que quedan N` | si |

## El veredicto

**VERDE** -- el set esta LIMPIO. No se toca nada. Exige las cuatro cosas:
  - una unica copia de `sl.dlss_g` mapeada en toda la corrida,
  - viva al final,
  - con el sitio de la cuenta y con sitio de pacer,
  - y de la misma version que el interposer.

**ROJO** -- cualquier otra cosa. Se ofrece reemplazo, previa autorizacion.

### Por que tajante, y por que mi objecion era falsa

Hubo primero un AMARILLO ("anda pero es fragil") y despues un VERDE con nota.
Las dos formas eran un tercer estado disfrazado. El usuario pidio dos estados y
que cualquier rareza, por minima que sea, caiga en rojo.

Yo lo resisti con este argumento: "sustituirle el set a un juego que anda lo
rompe; a Cyberpunk lo crasheo a los 8 segundos". **Ese argumento era falso.** No
habia dump, ni modulo, ni offset -- el juego murio en una corrida donde
sustituimos y yo llame a eso una causa. Ademas el crash fue con un mecanismo que
ya no existe: sustituia un plugin suelto cruzando versiones, sin coherencia de
set.

Se midio en vez de discutir. Cyberpunk forzado a ROJO con la sustitucion activa:

    corrida   sustituciones   duracion    p90     max
    1         7               127 s       4.00    4.11
    2         7               135 s       3.93    4.08
    3         7               130 s       3.97    4.08
    linea base sin sustituir              3.97    4.08

Indistinguible, tres de tres. **Marcar rojo a un juego que anda no lo rompe.**

**Y el hallazgo que mas sirve:** esas tres corridas fueron con el interposer 2.7
del juego y siete plugins 2.12 -- un cruce de trenes. Funciona. Era exactamente
la combinacion que yo trataba como la causa del crash de Halo y evitaba por eso.

Gate con la regla tajante, estado limpio (primera instalacion), sin autorizacion:

    corrida 1  ROJO  0 sustituciones  p90 3.95  max 4.17
    corrida 2  ROJO  0 sustituciones  p90 4.00  max 4.13   (aviso de falta de permiso)

Rojo marca, no actua: sin el si explicito no se sustituye nada.

## Criterio de rechazo

Si el detector devuelve algo distinto de VERDE para `gtav-213-limpio`, o algo
distinto de ROJO para `ota:132874`, el criterio esta mal y no se avanza a M2.
El caso de varias copias (`gtav-213-limpio --ota`) tiene que dar VERDE y no
sustituir nada.

**Correccion del fixture, no del criterio.** La primera version de este documento
pedia `cp2077-271` para AMARILLO. Ese set NO se puede correr en el banco, y ya
estaba medido y escrito desde el 2026-09-08: el sample no arranca con el
interposer 2.7.1 -- cero modulos de Streamline, salida vacia, status INVALID.
Se reprodujo igual antes de darse cuenta. `gtav-213-ota` es el equivalente
correcto: el propio banco lo describe como "2.13 con enableOTA true (como
Cyberpunk)", que es justamente el caso de dos copias. El umbral no se afloja: se
cambia una probeta imposible por una que existe.

## M2 y M3: resultado medido, 2026-09-09

Regla de M2: se llevan los PLUGINS a la version del INTERPOSER, nunca al reves.
El interposer es el borde contra el que el juego se compilo; los plugins los
carga el interposer y ese borde es interno. Si la version del interposer no
tiene un dlss_g parcheable, se dice y no se arma nada.

Caso ROJO del banco (`ota:132874`), ciclo completo probado POR PASE:

    pase sin estado:   "sin veredicto ROJO previo"  ->  VEREDICTO: ROJO
    pase con ROJO:     interposer 2.12, 9 modulos en la cache, 4 sustituciones
                       -> 1 copia, 1 parche, VEREDICTO: VERDE

Multiplicador CONTADO despues de sustituir, base 30, n=24 ventanas cada uno:

    x2 pedido -> 2.00   (p10 2.00  p90 2.00)
    x3 pedido -> 3.00   (p10 2.97  p90 3.00)
    x4 pedido -> 4.00   (p10 3.97  p90 4.00)

Todo desde la cache de la maquina. Sin red.

Gate de regresion, benchmark de Cyberpunk con estado borrado (primera instalacion):

    linea base 86fa8c0b   535 ventanas  mediana 1.00  p90 3.97  max 4.08
    M1  36d5bad           611 ventanas  mediana 1.00  p90 3.91  max 4.11
    M2+M3                 590 ventanas  mediana 1.00  p90 3.93  max 4.17
    copias 2, parches 2, sustituciones 0, veredicto AMARILLO en los tres

### Tres defectos que solo apareceieron midiendo

1. La clave del estado usaba la ruta completa del ejecutable, que cambia en cada
   corrida del banco: el veredicto no persistia y el mecanismo era intesteable.
   Ahora la clave es nombre + tamano del ejecutable.
2. El veredicto se pisaba a si mismo. Se recalculaba sobre el set YA sustituido
   -- que es sano, o sea VERDE -- y la corrida siguiente no sustituia, volviendo
   al set roto: una corrida si y una no. En el banco no se veia porque cada
   corrida lanza DOS pases y el de referencia reescribe ROJO. Ahora el
   diagnostico se guarda solo cuando no hubo sustitucion.
3. La emision del veredicto estaba solo en el lazo de teclas, que no corre bajo
   el banco. Va tambien en el camino de present.

## M4: el consentimiento, medido

Sustituir el Streamline de un juego cambia que binarios corre. Con ROJO solo NO
alcanza: hace falta un si explicito, y hasta que lo haya el dll no toca nada.

    sin permiso   sustituciones REALES = 0, veredicto ROJO, el set sigue roto
    con permiso   sustituye, x2 -> 2.00, x3 -> 3.00, x4 -> 4.00 (n=13 c/u)

La pregunta va en una franja al pie del panel, SUMADA al alto para que no mueva
nada de lo existente: F7 acepta, F8 rechaza. La respuesta se anexa al archivo de
estado sin pisar el diagnostico. Un juego VERDE o AMARILLO no ve nada.

Cuidado al medir esto: la linea del gate dice "no se sustituye", y contar con
"se sustituye" toma la negativa como si fuera una accion. Hay que contar
"se sustituye un modulo" o "se sustituye el INTERPOSER".

Gate de Cyberpunk sobre el binario exacto que se commiteo:

    linea base 86fa8c0b   535 ventanas  p90 3.97  max 4.08
    este build            625 ventanas  p90 3.86  max 4.17
    copias 2, parches 2, sustituciones 0, veredicto AMARILLO

Los cuatro gates de builds equivalentes dieron p90 3.97, 3.91, 3.93 y 3.86.

### Un error propio que vale mas que el arreglo

El log venia diciendo "version: 2.0" para copias que son 2.11 y 2.14. Se afirmo
que la causa era un conflicto de FILE_SHARE al abrir un modulo ya mapeado, se
cambio el modo de apertura, y se escribio esa causa como comentario en el codigo
SIN medirla. Era falsa.

La causa real: al sacar el filtro de version, la edicion borro la llamada a
version_soportada y dejo el log imprimiendo una variable en cero. Se borro la
medicion y quedo el cartel. Restaurada la llamada, el log dice 2.11 y 2.14.

## Lo que NO esta probado

  - Que la franja de la pregunta se vea bien. Se garantiza que no desplaza nada
    (se suma al alto) y que sin respuesta no se sustituye; como queda en pantalla
    no se puede verificar desde aca.
  - Que sustituir el interposer arregle Halo. El sample del banco lo importa
    ESTATICAMENTE -- verificado leyendo su tabla de importaciones -- asi que ese
    camino no se ejercita ahi. Halo lo carga dinamicamente y por eso deberia
    funcionar, pero deberia no es una medicion.

## Sustitucion completa en un juego que YA funciona, medida

Cyberpunk forzado a ROJO con consentimiento, con el arreglo de la comparacion de
rutas puesto -- o sea sustituyendo TAMBIEN la copia OTA:

    corrida   sustituciones   versiones mapeadas    DLSS-G    contador p90
    1         18              todas 2.12            4.00x     3.97
    2         18              todas 2.12            4.00x     3.97
    3         18              todas 2.12            4.01x     4.00
    linea base (sin sustituir)                      4.02x     3.97

Indistinguible, tres de tres. Ninguna copia 2.14 sobrevive. Cambiarle a un juego
que anda TODO su set de plugins no lo degrada ni lo rompe.

Antes del arreglo eran 7 sustituciones y la copia OTA entraba igual; ahora son
18 y no queda ninguna.

## El permiso duraba UNA corrida, 2026-09-09

`emitir_veredicto_si_toca` abre el archivo de estado con CREATE_ALWAYS -- trunca --
y escribia solo veredicto, vivas y con_sitio. La linea "consentimiento=" que M4
anexa se borraba, asi que un juego autorizado volvia a "nunca preguntado" en el
arranque siguiente y no se sustituia nada. Es el mismo defecto de "una corrida si
y una no" que ya se habia corregido para el veredicto, ahora en el permiso.

`g_ya_sustituimos` no lo tapaba: Cyberpunk levanta DOS procesos con el dll
adentro y alcanza con que llegue ahi el que no sustituyo.

Se corrigio releyendo el estado del disco antes de truncar y arrastrando la
linea. Se relee en vez de usar g_consentimiento porque quien lo llena es el lazo
de teclas, que no corre en todos los procesos ni bajo el banco.

Como se comprobo, con el binario 821bdde8. Hace falta una corrida que NO
sustituya, que es la unica que llega al camino corregido:

    estado antes    ROJO + consentimiento=no
    sustituciones   0                      (sin permiso no toca nada)
    estado guardado 1                      (el camino corregido corrio)
    log             "consentimiento conservado (1 = si) 0"
    estado despues  ROJO + consentimiento=no

La corrida anterior, con permiso, NO sirve como prueba: sustituyo 18 modulos y
salio por el guard viejo sin tocar el codigo nuevo. Se la conto como verificacion
antes de mirar el log y era falso.

### Gates de ese binario

    banco ota:132874, modo 3      2.97 contado, valido al primer intento
    Cyberpunk sustituyendo        18 sustituciones, 7 copias TODAS 2.12,
                                  301 ventanas  p90 3.97  max 4.11  DLSS-G 4.00x
    Cyberpunk sin sustituir       set propio (2.11 + OTA 2.14),
                                  111 ventanas  p90 4.04  max 4.08  DLSS-G 4.00x
    linea base                    p90 3.97  max 4.08
    GTA V                         VERDE, 1 copia 2.13 viva y parcheada, 31 min

### Una regresion que reporte y no era

La primera corrida de Cyberpunk del dia murio a los 13 s con
`D3D11CreateDeviceAndSwapChain fallo, hr 0x8007000E` (E_OUTOFMEMORY) y cero
ventanas. No era el codigo: era un GTA V mio de una prueba anterior que seguia
vivo desde hacia media hora ocupando 7,4 GB. Sin dump de Cyberpunk y con un
dump de QmlRenderer -- el CrashReporter de CDPR -- en la misma hora.

Antes de leer una corrida de Cyberpunk hay que mirar que no quede ningun proceso
pesado vivo. Es la segunda vez en el dia que el entorno contaminado se parece a
una rotura del codigo; la primera fueron los dialogos de CrashReporter robando
el foco.

## Lo que este documento NO afirma

No afirma que sustituir el set arregle Halo. Eso es una hipotesis sin probar: el
plugin de UE de Halo se compilo contra headers 2.7 y nunca se lo corrio con un
interposer 2.13. Se prueba en M3, en el banco, con `ota:132874`.
