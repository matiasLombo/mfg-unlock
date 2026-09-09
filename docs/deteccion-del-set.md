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

**VERDE** -- no se toca nada:
  - queda **una** copia viva de `sl.dlss_g`, y
  - esa copia tiene el sitio de la cuenta, y
  - tiene al menos un sitio de pacer.
  GTA V medido: 1 copia, 0 descargas, 0 `sitios que quedan 0`.

**AMARILLO** -- funciona, no se toca, pero se anota que es fragil:
  - mas de una copia mapeada, y la que sobrevive tiene el sitio.
  Cyberpunk es este caso: interposer 2.7 + plugin 2.11 + una copia OTA 2.14.
  Anda a pesar de la configuracion, no gracias a ella. **Amarillo NO habilita
  sustituir**: es la regla que faltaba anoche.

**ROJO** -- candidato a sustitucion, previa autorizacion en el panel:
  - la copia que sobrevive tiene **0** sitios de cuenta, **o**
  - todas las copias parcheadas se descargaron (`sitios que quedan 0` al final).
  Halo medido: la 2.7.30 tiene 0 sitios; con redireccion, la 2.12 se parchea y
  Streamline la descarta.

## Datos de referencia, medidos el 2026-09-09

Con el dll bueno (86fa8c0b), benchmark de Cyberpunk con foco forzado:

    copias mapeadas      2   (su 2.11 propia + la OTA 2.14 de la cache)
    parches enganchados  2
    descargas de modulo  2
    multiplicador        535 ventanas, mediana 1.00, p90 3.97, max 4.08

La mediana 1.00 no es un defecto: el benchmark incluye tramos sin generacion.
El numero que sirve de referencia es **p90 3.97 / max 4.08**.

Contraste medido la misma noche, con un filtro que dejaba sin parchear la copia
2.14: 622 ventanas, **max 2.08** -- ninguna ventana llego a 4x. Ese 2.00 era el
frame generation nativo del juego. Sirve como prueba de que la copia OTA es la
que hace el trabajo, y como umbral: si una corrida de Cyberpunk no llega a p90
cercano a 4, el cambio rompio algo.

En el banco, `gtav-213-limpio --mode 2 --base-fps 30`: mediana 2.00 exacta,
13 ventanas, engancho al primer intento (`tools/bench.py`).

## Criterio de rechazo

Si el detector devuelve algo distinto de VERDE para `gtav-213-limpio`, o algo
distinto de ROJO para `ota:132874`, el criterio esta mal y no se avanza a M2.
Para AMARILLO se usa `gtav-213-ota`, y amarillo no sustituye.

**Correccion del fixture, no del criterio.** La primera version de este documento
pedia `cp2077-271` para AMARILLO. Ese set NO se puede correr en el banco, y ya
estaba medido y escrito desde el 2026-09-08: el sample no arranca con el
interposer 2.7.1 -- cero modulos de Streamline, salida vacia, status INVALID.
Se reprodujo igual antes de darse cuenta. `gtav-213-ota` es el equivalente
correcto: el propio banco lo describe como "2.13 con enableOTA true (como
Cyberpunk)", que es justamente el caso de dos copias. El umbral no se afloja: se
cambia una probeta imposible por una que existe.

## Lo que este documento NO afirma

No afirma que sustituir el set arregle Halo. Eso es una hipotesis sin probar: el
plugin de UE de Halo se compilo contra headers 2.7 y nunca se lo corrio con un
interposer 2.13. Se prueba en M3, en el banco, con `ota:132874`.
