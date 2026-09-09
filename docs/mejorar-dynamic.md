# Como mejorar DYNAMIC: la matematica del fraccional y donde esta el muro

> **CORREGIDO por [[dynamic-medido-bien]] (docs/dynamic-medido-bien.md).** La
> seccion "La causa, medida" de abajo atribuye 24-25 % del tiempo al
> enfriamiento de 100 ms: eso quedo REFUTADO -- la curva de recuperacion tras un
> cambio de cuenta es plana. Y la cifra de cadencia que la sostenia venia de una
> metrica con banda absoluta que confundia media con dispersion. El costo real
> medido de DYNAMIC es 1.8x de dispersion en los intervalos, no un enfriamiento.
> Lo que sigue en pie es el conteo de cambios de cuenta (~3/s contra 0), la
> imposibilidad de modular el byte bajo la reserva, y la teoria.


Investigado el 2026-09-09 con la benchmark de Cyberpunk (set sustituido, con
consentimiento) y con la teoria establecida del problema.

## La causa, medida

    configuracion   cambios de cuenta API   por segundo   fuera de cadencia
    4X fijo                  0                 0.00            23.8 %
    5X fijo                  0                 0.00             1.2 %
    DYNAMIC c1             306                 2.35            31.0 %
    DYNAMIC c2             323                 2.51            30.8 %

Cada cambio de la cuenta llama a `slDLSSGSetOptions`, que hace que el plugin
libere recursos y arranque **100 ms de enfriamiento** -- `0x1800497fd` escribe
100.0 en `[ctx+0x4488]`, ya identificado en este repo. A 2.4 cambios por segundo
eso es **24-25 % del tiempo con la generacion degradada**. Los modos fijos no
cambian la cuenta NUNCA.

DYNAMIC ademas replantea el ratio 1020 veces por corrida (7.9/s) y solo 306
cruzan un entero: **el 25 % del tiempo perdido lo produce el 3.8 % de sus
decisiones.**

Consecuencia medida contra un fijo del mismo punto de operacion (5X contra
DYNAMIC a 5.4x, dos corridas cada uno): 30 veces peor en cadencia, 4 veces mas
tiempo adentro de Present, y **menos fps** -- 161 contra 192.

**Es una estimacion, no una medicion directa.** El coeficiente de 100 ms sale de
un comentario del repo. Lo que esta medido es la tasa de cambios y la cadencia
resultante. Ver "Como confirmarlo" abajo.

## El muro estructural, y por que la salida obvia no existe

El fraccional necesita variar la cuenta. La cuenta solo se puede variar por la
API. Y la API cuesta 100 ms cada vez.

La salida obvia seria declarar la reserva en el techo y modular por debajo el
byte del bound. **Esta medido y no funciona** ([[subframe-bound-semantics]]):

    bound = API + 1   funciona
    bound > API + 1   crash 0xC0000005
    bound < API + 1   la presentacion SE DETIENE, dos de dos

El byte no reduce la generacion dentro de la reserva; con la API en 2 y el bound
alternando 2 y 3 el resultado es indistinguible de un 3.0x constante. **La cuenta
de la API decide.** No hay grado de libertad entre las dos.

## Lo que dice la teoria establecida

El problema no es nuevo. Es **sintesis de frecuencia fraccional**: producir una
razon no entera alternando un divisor entero, que la literatura llama *division
ratio averaging*. Lo que ahi se combate son los **spurs**: tonos periodicos que
salen del patron de alternancia. Un spur en el dominio del tiempo de un juego es
judder periodico.

Tres cosas que la teoria dice y que aplican:

  1. **Alternar rapido empuja el error a alta frecuencia** (noise shaping, que es
     de lo que vive un modulador delta-sigma). Bloques largos son lo contrario:
     concentran el error en baja frecuencia, que es justo donde el ojo lo ve.
  2. **Reparto maximamente parejo**: el algoritmo de Bjorklund (ritmos
     euclideanos) reparte k pulsos en n ranuras lo mas parejo posible. Su origen
     es literalmente nuestro problema -- una compuerta que debia abrirse k veces
     dentro de una ventana, lo mas espaciada posible, en un acelerador de
     neutrones.
  3. **Framerate cerca del refresh amplifica la visibilidad del beat**
     (Blur Busters): 238 fps en 240 Hz da 2 stutters por segundo.

**Pero (1) y (2) piden exactamente lo que cuesta 100 ms.** Por eso el codigo usa
bloques largos y contiguos: no es pereza, es el enfriamiento. La teoria y la
restriccion del plugin apuntan en direcciones opuestas, y la restriccion gana
mientras exista.

Y (3) NO explica lo nuestro: 5X fijo es el que MAS se pasa del panel (192
presentaciones contra 151 Hz, 21 % de frames descartados) y es el mas liso de
todos. El beat no es el problema aca.

## Lo que hay que corregir de lo que ya estaba escrito

**La refutacion de Bresenham esta retirada, no vigente.** El comentario del
codigo mide reparto parejo como peor en todo (1.25x: 26 cambios y 5 % fuera de
cadencia contiguo, contra 321 cambios y 36 % repartido) y despues dice
textualmente que esos numeros se tomaron contra un planificador que no se
quedaba quieto, con el contador 3.7 % alto y el reloj de bloques 7x rapido:
*"They are not evidence any more."* El reparto parejo esta **sin probar**, no
refutado. Lo que si sobrevive de ahi es la aritmetica: repartir multiplica los
cambios por diez, y a 100 ms cada uno eso es inviable por construccion.

**Y bloques contiguos no pueden cumplir el criterio.** Con bloques de 1.1 s una
ventana de 0.5 s cae entera adentro de un bloque y solo puede leer un entero.
Medido en GTA V: 2.75 pedido, 3.00 leido plano en todas las ventanas.

O sea que hoy el fraccional esta atrapado entre dos imposibles: repartir cuesta
demasiados cambios, y no repartir no entrega la fraccion dentro de ninguna
ventana que importe.

## Las tres vias, ordenadas por lo que rinden

### 1. Bajar la tasa de cambios del controlador -- barato, sin riesgo

Es el arreglo directo de lo medido. 2.4 cambios/s cuesta 24 %; 0.5 cambios/s
costaria 5 %. El controlador replantea 7.9 veces por segundo y solo necesita
cruzar un entero cuando el objetivo se movio de verdad.

Palancas: histeresis sobre el CRUCE de entero (no sobre el ratio, que ya tiene
banda muerta 0.10), y un minimo de permanencia por cuenta -- la memoria de
[[subframe-bound-semantics]] dice que 30 frames renderizados entre cambios es
seguro y limpio, que a base 33 son ~0.9 s.

**Cuidado:** un limite de tasa de escritura ya se probo una vez y empeoro (51 %
contra 63 % de ventanas en objetivo). Pero eso se juzgo con la metrica de
sostener el objetivo, no con cadencia, que no existia entonces. Hay que
re-evaluar el intercambio con las dos metricas a la vez: se va a perder
precision de objetivo para ganar cadencia, y hay que decidir cuanto.

### 2. Atacar el enfriamiento -- el premio grande, y el riesgo grande

Si el enfriamiento de 100 ms deja de costar, se cae el muro entero: la difusion
por frame y el delta-sigma pasan a ser viables, y con ellos todo lo que dice la
teoria. La direccion es conocida: `0x1800497fd` escribe 100.0 en `[ctx+0x4488]`.

No hacerlo a ciegas. El enfriamiento probablemente protege la transicion de
recursos que la propia llamada dispara, y este proyecto ya aprendio que aflojar
una regla y su validacion juntas termina mal ([[gpu-code-demands-more-care]]).
El orden correcto es entender que libera la llamada y si la liberacion es
necesaria cuando lo unico que cambio es la cuenta.

### 3. La matematica del reparto -- solo despues de 1 o 2

Bjorklund y delta-sigma deciden DONDE poner los cambios. Eso no importa mientras
cada cambio cueste 100 ms; importa mucho apenas sean baratos. Guardarlo para
entonces.

## Como confirmarlo antes de tocar nada

El 24 % es una estimacion con un coeficiente de segunda mano. La confirmacion
directa: **medir la cadencia en funcion de la distancia a un cambio de cuenta,
con una ventana mas ancha que el efecto.**

Hoy el codigo separa "cerca" en 8 presentaciones, que a 161 fps son ~50 ms --
**la mitad del enfriamiento que se quiere detectar.** Por eso la medicion de hoy
dio cerca ~ lejos (riesgo relativo 0.94 y 0.92) y por eso NO se puede concluir
de ella que cambiar la cuenta sea gratis: la ventana era mas corta que el efecto.

Cambiar el corte a ~32 presentaciones (~200 ms) y volver a correr. Si el
enfriamiento es real, la tasa cerca tiene que separarse de la lejana.

## Lo que este documento NO afirma

  - No afirma que el enfriamiento de 100 ms sea la causa: afirma que la tasa de
    cambios lo predice y que los modos fijos, con cero cambios, son limpios.
  - No afirma que bajar la tasa de cambios mejore la fluidez PERCIBIDA. Mejora
    una metrica de cadencia medida en el reloj de Present, que no es el reloj
    que se ve ([[medir-fluidez]]).
  - No afirma que el reparto parejo sea mejor ni peor. Esta sin probar.
