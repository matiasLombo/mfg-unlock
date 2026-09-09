# DYNAMIC sin integrador: por que se saca y que se gana

2026-09-09. Leido el codigo directo, sin las memorias de por medio, y medido con
A/B del MISMO binario detras de `mfg-deuda.txt`.

## El defecto: dos lazos para el mismo trabajo

`dyn_apply` tenia tres cosas haciendo control:

  - **feedforward** `raw = target / base_fps` -- ya calcula el ratio necesario;
  - **sesgo aprendido por banda** -- corrige el error de modelo;
  - **integrador** `debt` sobre el error de presentaciones, con
    `target_eff = target + debt * 2.0`.

El integrador es redundante con el sesgo: cuando la base se mide directamente no
hay perturbacion persistente que rechazar. Y es el que rompe.

**Cuatro problemas concretos, del codigo:**

1. **Unidades mezcladas.** `debt` acumula FRAMES
   (`target*secs - presentaciones`) y se sumaba a un rate. La ganancia efectiva
   dependia del periodo de tick.
2. **Clamp demasiado ancho.** `debt` limitado a +-33 % del target y multiplicado
   por 2 da +-66 % del setpoint.
3. **Anti-windup de un solo lado.** `salida_saturada` congela el `inc` positivo
   cuando `want >= 6.0` pero NO desenrolla la deuda ya acumulada.
4. **El sesgo aprendia con la salida recortada**, o sea de un tramo donde la
   entrega estaba limitada por el techo y no por el modelo.

**Se ve en los logs.** Dos corridas del mismo binario:

    corrida   objetivo efectivo (nominal 165)   sesgo
    buena     153-160                           0.88-1.06
    mala      274 en 168 de 220 cambios         1.25 (su tope)

En la mala, integrador y sesgo quedaron los dos clavados en el riel y el
controlador pidio 6x permanente sin recuperarse.

## El cambio

  - El integrador deja de entrar al objetivo: `target_eff = target`. Queda
    detras de `mfg-deuda.txt` para poder correr el A/B con el mismo binario.
  - El sesgo no aprende mientras la salida esta recortada (`want >= 6.0` o
    `want <= 2.0`).

## Medido, 3 corridas por brazo, mismo binario

    brazo             desvio relativo        cambios de cuenta   pres/s (obj 165)
    CON integrador    14.9 %  (14.4-15.3)          359                165
    SIN integrador    13.2 %  (12.4-14.0)          261                169

Los rangos no se solapan. **-11 % de dispersion y -27 % de cambios de cuenta.**

**No baja al 9-10 % de los modos fijos**, asi que el integrador no era la unica
causa de la dispersion de DYNAMIC.

## El costo, que hay que decir

Aparece un **error estacionario de +2.4 %**: 169 presentaciones/s contra 165
pedidas, donde antes clavaba 165. Es exactamente lo que un integrador existe
para evitar. Se considera aceptable porque quedar apenas por encima del objetivo
es mejor que por debajo, pero es un cambio de comportamiento real.

Y explica algo que estaba anotado como sin explicar en [[dynamic-controller]]:
"un +2 % estacionario en la mediana". El integrador estaba tapando un sesgo
sistematico del feedforward, no corrigiendo una perturbacion.

## Lo que NO afirma

  - No afirma que la dispersion restante (13.2 % contra 9-10 % de los fijos) sea
    inevitable. No se busco su causa.
  - No afirma que 13.2 % contra 14.9 % se note al jugar. Es una metrica de
    intervalos entre presentaciones, no de percepcion.
