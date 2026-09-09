# DYNAMIC medido con una metrica que sirve, y tres retractaciones

2026-09-09. Este documento corrige tres afirmaciones de `mejorar-dynamic.md` y
`medir-fluidez.md` que salieron de mediciones mal controladas. Las tres eran
mias y las tres eran de la misma familia: comparar configuraciones que corrian a
frame rates distintos.

## La metrica anterior estaba mal

"Fuera de cadencia" contaba presentaciones cuyo intervalo caia fuera de una
banda ABSOLUTA de 4 a 8 ms. Eso mide cuanto se aleja el frame rate de ~165 fps,
mezclado con la irregularidad real:

    4X fijo   133 pres/s   intervalo 7.5 ms, rozando el borde de la banda -> 23.8 %
    5X fijo   192 pres/s   intervalo 5.2 ms, comodo en el medio          ->  1.2 %

La misma DYNAMIC dio 31 % a 5.4x y 3 % a 3.8x. **Comparar configuraciones con
distinto fps por esa banda no es valido**, y ese fue el defecto comun.

La metrica nueva es adimensional: desvio relativo de los intervalos contra la
media de SU PROPIA ventana. 133 y 192 presentaciones por segundo se pueden
comparar con ella.

## La medicion, las tres a tasas comparables

    configuracion  ventanas  mult  pres/s  DESVIO REL.  pres/refresh  Present  cambios
    4X fijo           66     4.00    159      9.0 %        1.10        9.6 %      0
    5X fijo           63     5.00    189     10.5 %        1.25       11.2 %      0
    DYNAMIC           56     4.16    164     17.3 %        1.10       10.1 %    385

## Las tres retractaciones

**1. "DYNAMIC es 30 veces peor en cadencia" -> es 1.8 veces.** 17.3 % contra
9.0-10.5 %. La diferencia es real y consistente, pero de otro orden de magnitud.

**2. "DYNAMIC bloquea Present 40.6 % contra 11 % del fijo" -> son iguales.** A
tasas comparables: 10.1 %, 9.6 % y 11.2 %. La diferencia anterior la producia el
punto de operacion, no el modo. Cae con ella la correlacion monotona que se
presento entre duracion de Present y cadencia: las dos las movia el frame rate.

**3. "El enfriamiento de 100 ms cuesta 24-25 % del tiempo" -> refutado.** La
curva de recuperacion tras un cambio de cuenta, filtrada a ventanas que generan,
es PLANA sobre 12.281 presentaciones:

    0-25 ms    3.7 %      100-200 ms   2.9 %
    25-50 ms   4.5 %      200-400 ms   2.2 %
    50-100 ms  3.4 %      400+ ms      3.0 %

Sin filtrar, el tramo 400+ daba 87 % -- pero eso son las ventanas de menu, donde
el juego corre a 625 fps y todo intervalo cae bajo el umbral. El mismo defecto
otra vez.

## Lo que queda en pie

  - DYNAMIC cambia la cuenta de la API ~3 veces por segundo; los modos fijos,
    cero. Es un conteo de eventos reales.
  - Paga por eso **1.8x de dispersion** en los intervalos entre presentaciones.
  - No hay diferencia en entrega: pres/refresh 1.10 contra 1.10 y 1.25. Los
    frames llegan al panel igual en las tres.

Eso es todo el costo medible de DYNAMIC hoy. Si 17.3 % contra 9 % se siente o no
es otra pregunta, y no se puede contestar desde aca.

## Un efecto del instrumento, que vale por si mismo

El instrumento de la curva de recuperacion escribia ~12 lineas de log por
ventana, y solo en el camino del cambio de cuenta -- o sea, solo en DYNAMIC.
Con el puesto, DYNAMIC no engancho la generacion en 4 de 4 corridas mientras 4X
y 5X validaban a la primera. Sacandolo, DYNAMIC engancho a la primera.

0 de 4 contra 1 de 1 no es prueba fuerte, pero la asimetria con los modos fijos
y el hecho de que sea un camino exclusivo de DYNAMIC lo hacen creible. **Ese
camino es sensible al tiempo**: doce lineas de log alcanzaron. Ver
[[log-must-stay-readable-live]], que ya documenta que log_line abre el archivo
por linea a proposito.

## Que medir despues

La pregunta abierta no es cuanto cuesta cambiar la cuenta -- eso ya esta: 1.8x
de dispersion. Es si esa dispersion se ve. Y para eso hace falta el instrumento
que todavia no existe: el contenido temporal de los frames generados, o sea
animation error adaptado a la fase que nosotros conocemos. Ver
[[medir-fluidez]].
