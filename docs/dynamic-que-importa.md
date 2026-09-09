# DYNAMIC: que se mejoro, que no, y por que la variable estaba mal elegida

2026-09-09, cierre de la revision del fraccional y DYNAMIC.

## El umbral que faltaba, y que cambia la prioridad

El white paper de metodologia de animation error (GamersNexus) da el unico
numero practico que se encontro: **se notan desviaciones de frametime de ~8 ms
cuando ocurren seguido.** El modelo academico de judder (Chapiro et al., ACM TOG
2019) modela frame rate, luminancia y velocidad de paneo, no dispersion a tasa
fija, asi que no aplica a esto.

Contra eso, nuestros numeros a 165 fps (intervalo medio 6.06 ms):

    configuracion        dispersion relativa   en milisegundos
    modos fijos              9-10 %                0.55 ms
    DYNAMIC                 13-15 %             0.80-0.92 ms
    umbral reportado           --                  ~8 ms

**Estamos diez veces por debajo del umbral.** La diferencia entre 12.7 % y
15.2 % son 0.15 ms. No se ve.

**Lo que si esta por encima son los hitches:** 17-34 eventos de mas de 33 ms por
corrida en ~60 s de generacion, casi uno por segundo, y picos de 350-480 ms. Eso
es 4x a 60x el umbral y con la frecuencia que la fuente llama perceptible.

**Conclusion incomoda: se estuvo midiendo y optimizando la variable equivocada.**
La dispersion rutinaria es imperceptible; los eventos raros son los que se ven.

## Lo que se midio de la responsividad y de las transiciones

**Responsividad: ya estaba bien.** Tras un salto de base mayor al 12 %, el
controlador vuelve a +-5 % del objetivo en **mediana 0 ventanas** (media 0.7,
n=10 saltos). El feedforward `target/base` reacciona dentro de la misma ventana.
No habia nada que arreglar.

**Transiciones: hay efecto y no es confound.** Controlando por estabilidad de la
base:

    base estable (n=149)    pocos cambios 12.7 %    muchos (>=6) 15.2 %
    base inestable (n=29)   pocos cambios 15.9 %    muchos       54.8 %

La densidad de cambios de cuenta no la causa la base inestable (5.8 cambios de
media contra 5.5), asi que el efecto es de los cambios. Pero son 2.5 puntos de
un 6 ms, o sea 0.15 ms: real y por debajo del umbral.

## El experimento del integrador, completo

    brazo                 desvio    dentro de +-5%   cambios   hitches
    CON integrador        14.9 %         83 %          359     17/26/34
    SIN integrador        13.2 %         67 %          261     18 (media)
    CON + anti-windup     14.8 %         83 %          347     14/19/20

Sacar el integrador bajaba la dispersion 1.7 puntos y costaba **16 puntos de
precision del objetivo**. Mal negocio: se revirtio.

Lo que quedo es el **anti-windup con back-calculation**: lo que la salida no
puede entregar se descuenta de la deuda. Elimina el modo de falla real, medido
antes: objetivo efectivo 274 contra 165 nominal en 168 de 220 cambios, con el
sesgo tambien clavado en su tope de 1.25, pidiendo 6x sin recuperarse.

Los hitches bajaron de 17/26/34 a 14/19/20, pero **los rangos se solapan: no
esta establecido.**

## Un arreglo que quedo a medias, y hay que terminarlo o sacarlo

Se acoto el reinicio del ciclo de bloques a cambios de ratio mayores a 0.5, para
que los ajustes chicos de DYNAMIC no reinicien el ciclo. **No bajo los cambios
de cuenta** (347 contra 359). La razon: se saco el reinicio del RELOJ pero se
dejo el recalculo del reparto de bloques, y ese es el que mueve la cuenta. Para
que sirva habria que latchear tambien el reparto hasta que el ciclo de la vuelta.

## Una correccion propia

Se afirmo que el integrador tenia las unidades mal (`debt` en frames sumado a un
rate). **Es falso**: el `2.0` lleva unidades de 1/s implicitas, o sea es un
horizonte de correccion de 0.5 s. Dimensionalmente cierra. El defecto real era
solo el enrollamiento sin desenrollado.

## Que atacar despues

Los hitches, no la dispersion. Y no se sabe todavia de donde salen: la escena
del benchmark tiene hitches propios (66 en 55 ventanas con el multiplicador
clavado en 4.00 y desvio 0.07), asi que hace falta separar los del contenido de
los nuestros antes de perseguirlos.
