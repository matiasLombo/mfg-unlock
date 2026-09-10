# Fraccionales sobre la base propia

Escrito **antes** de medir, a proposito. El criterio se fija primero y despues se
corre; al reves ya salio mal una vez en este proyecto.

## Que estaba roto

Con nuestro snippet la cuenta de `numFramesToGenerate` es el **multiplicador**,
no los frames generados (ver [[ngx-caps-the-count]]). El fraccional estaba armado
para la semantica vieja:

- a la API se le declaraba el **techo del ciclo** (`g_ciclo_techo = lo + 1`),
  que dimensionaba la reserva;
- la fraccion la llevaba el **byte por frame** parcheado en el plugin, que era
  el bound del loop de sub-frames.

Con la base nueva la cuenta de la API es lo que se entrega, y el byte dejo de
modular. O sea que declarar el techo clava el ratio en el techo.

Medido, pedido **2.55x**, corrida valida de 27 ventanas:

| que | valor |
|---|---|
| multiplicador contado | mediana **3.00**, p90 **3.00**, max 3.06 |
| cuenta a la API | 3, en las 27 ventanas |
| byte vivo | 3, en las 27 ventanas |

Mediana, p90 y maximo iguales es la firma de un valor constante. No es una media
de 2 y 3: es un 3 fijo.

## El arreglo

Con la semantica nueva la fraccion la lleva **la cuenta de la API**, alternada
por bloques. La estructura de bloques ya existia (`g_slowalt`, encendido salvo
`mfg-noslowalt.txt`, bloques de 240 frames renderizados ~ 800 ms) y ya movia
`g_force_generated`; lo que la anulaba era `force_into` subiendo el valor al
techo del ciclo antes de escribirlo.

Cuatro cambios:

1. `force_into` **no sube al techo** cuando la cuenta es el multiplicador.
2. Al **bajar** la cuenta, el byte se escribe antes de la llamada. La tabla
   medida no tiene transitorio seguro -- byte por encima de la reserva crashea,
   por debajo detiene la presentacion -- y el peligroso solo aparece al bajar.
3. La cadencia ya no escribe el byte: lo escribe `set_count_now(g_api_aplicada)`
   pegado a la llamada, que es el unico instante en que los dos coinciden.
4. La rama de difusion por frame (sin `slowalt`) avisa una vez en el log que ahi
   el ratio queda en el entero de arriba. Cambiar la cuenta por frame es una
   llamada a `slDLSSGSetOptions` por frame y 100 ms de enfriamiento cada una, o
   sea generacion apagada. No se arregla ahi.

El enfriamiento de 100 ms se paga dos veces por ciclo, no una vez por frame.

## Criterio, fijado antes de correr

Una corrida, `--fractional 255`, mismo set y misma duracion que la linea base.

- **EXITO**: mediana entre **2.40 y 2.70** (2.55 +- 6%) **y** el log muestra la
  cuenta de la API tomando los dos valores, 2 y 3.
- **FRACASO A -- sigue clavado**: mediana >= 2.90.
- **FRACASO B -- se apago la generacion**: mediana <= 2.10, o el sample crashea,
  o quedan menos de 20 ventanas validas contra las 27 de la linea base.
- **Corrida nula**: la generacion nunca engancha. El banco entrega generacion
  ~1 de cada 3 intentos y eso no es un resultado. Hasta 3 intentos; el corredor
  para en el primero valido.

El banco descarta rapido y no decide. Lo que decide es el juego.

## Resultado

Corrida valida, `--set bundled-2.12.0 --duration 30 --fractional 255`, contra la
linea base con la misma duracion y el mismo set:

| que | antes | ahora |
|---|---|---|
| multiplicador contado, mediana | **3.00** | **2.53** |
| p90 / max | 3.00 / 3.06 | 2.71 / 2.80 |
| ventanas | 27 | 27 |
| cuenta escrita a la API | 3, las 63 veces | **2 (x31) y 3 (x31)** |
| frames renderizados | 1215 | 1215 |
| tiempo de ventanas | 20996 ms | 21028 ms |
| presentaciones | 3633 | 3089 |
| hitches > 33 ms | 5 | 5 |
| hitches de pantalla | 6 | 5 |
| refreshes salteados por ventana | 9.59 | 11.96 |

Pedido 2.55, entregado 2.53: 0.8%. La cuenta de la API alterna. **EXITO** por el
criterio de arriba.

Lo que no cambio importa tanto como lo que cambio: mismos 1215 frames
renderizados en el mismo tiempo y los mismos 5 hitches. No se pago throughput --
las presentaciones bajan de 3633 a 3089 porque 2.55 es menos que 3.00, que es
exactamente lo que se pidio. Lo unico que empeoro son los refreshes salteados,
9.59 -> 11.96 por ventana, que es el precio conocido de cambiar la cuenta (dos
veces por ciclo).

Regresion de enteros, misma build: **5X entrego 5.00** (p90 5.00, max 5.02, 27
ventanas). El cambio de `force_into` toca todos los modos y no rompio los fijos.

## Un error de medicion que costo dos corridas

Las dos primeras corridas dieron 13 ventanas contra las 27 de la linea base, y
eso disparaba el criterio de fracaso. No era el cambio: la linea base se habia
corrido con `--duration 30` y `tools/bench.py` tiene 8 por defecto. El largo de
la corrida sale de ahi y nada mas.

Las dos corridas cortas ya tenian la respuesta adentro y no en el numero de
ventanas: 45 llamadas crudas al token y ~780 ms por ventana en las dos, igual que
la linea base. La ventana es de 45 frames renderizados, asi que su duracion y su
contenido dicen si el ritmo cambio; **cuantas ventanas hay solo dice cuanto duro
la corrida**. Comparar totales entre corridas de distinta duracion no significa
nada. Ver [[my-harness-is-the-variable]].
