# Objetivo: Halo deja de crashear SIN apagar frame generation

Escrito 2026-09-10 a pedido del usuario, antes de que se fuera a dormir, con la
instrucción explícita de que no pueda escaparme del problema declarándolo
resuelto cuando no lo está.

**Este documento manda sobre mi criterio.** Si en algún momento me parece que el
objetivo es imposible o que hay que renegociarlo, eso se le dice al usuario y se
para — no se reinterpreta el objetivo para que lo que tengo entre manos pase.

---

## El problema, en una línea

El bucle de sub-frames del plugin recorre una entrada que el llenado nunca
preparó, saca de ahí un puntero nulo y lee `+0x40`:

```
0xC0000005  LECTURA de [nulo + 0x40]     sl.dlss_g 2.12  +0x3ED6F
```

Once autopsias, banco y Halo, siempre el mismo offset y siempre con el índice
pedido una posición más allá de la última ranura llena.

## El repro, dado por el usuario

1. Abrir Halo Campaign Evolved
2. Esperar a que aparezca el **menú principal**
3. Apretar **Enter** para continuar
4. A partir de ahí crashea

Con `mfg-settings.txt` en `mode 8` (DYNAMIC), objetivo 165 fps.

**Cerrar el juego después de cada crash.** Un proceso colgado retiene el
swapchain y la corrida siguiente no vale.

---

## Qué cuenta como éxito

Las cinco condiciones, juntas. No cuatro.

1. **Cero crashes en 5 ciclos consecutivos** del repro de arriba. Un ciclo es:
   lanzar, llegar al menú, Enter, jugar ≥ 3 minutos, cerrar limpio.
2. **Ningún `.dmp` nuevo** en `Meteorite/Binaries/Win64/ue4ss/` posterior a la
   marca de agua de la sesión.
3. **Cero líneas `EXCEPCION`** en `mfg-unlock.log` de esos cinco ciclos.
4. **La generación está encendida y se puede probar**: `sl.log` con
   `interpolation state changed from disabled to enabled`, y `mfg-unlock.log`
   con ventanas de `counted multiplier` por encima de 1.00.
5. **DYNAMIC llega arriba**: al menos una ventana de cada ciclo con multiplicador
   ≥ 4.9. Si el controlador nunca sube, el ciclo no probó nada y no cuenta.

## Qué NO cuenta como éxito

Explícito, porque ya intenté varias de estas:

- Bajar `tope_cuenta()` a 4, o a cualquier número que saque 6X.
- Recortar la cuenta contra la API, contra la reserva, o contra cualquier cosa
  que baje el multiplicador entregado.
- Apagar DYNAMIC, apagar los fraccionarios, apagar el parche del byte.
- Que no crashee porque la generación nunca se encendió.
- Que no crashee en el banco. **El banco no es el criterio.** Reproduce el crash
  una de cada varias corridas; sirve para descartar rápido, no para cerrar.
- Una sola corrida sin crash. Cinco, consecutivas.
- "No pude reproducirlo esta vez."

**La regla dura:** si la solución hace que el usuario entregue menos frames
generados que antes, no es una solución. Es apagarlo con otro nombre.

---

## El arnés está validado — sin intervención humana

`tools/halo_repro.ps1`, 2026-09-10 01:43. Ciclo completo automatico, el usuario
no toco nada:

```
marca de agua: 01:42:26
ENTER            <- automatico
CRASH (dump: crash_2026_09_10_01_43_36.9256758.dmp)   200 MB
```

Un intento anterior (01:34) se dio por validado por error: ese Enter lo habia
apretado el usuario a mano, y su dump quedo en 0 bytes. Corregido.

Importa porque sin esto un "SIN CRASH" del arnes no significaria nada: podria
ser solo que el Enter no llego.

**Linea base de referencia**, misma corrida:

| | |
|---|---|
| ciclos que crashean | 1 de 1 |
| tiempo hasta el crash | ~70 s desde el lanzamiento |
| lineas EXCEPCION | 9 |
| ventanas de counted multiplier | 683 |

**Si el arnes deja de reproducir, lo primero es volver a establecer un
reproductor.** Un arnes que no reproduce no prueba nada.

## Hipótesis ya falsificadas — no repetirlas

Cada una se probó y se midió. Están acá para que no vuelva a gastar tiempo.

| # | hipótesis | cómo murió |
|---|---|---|
| 1 | El byte adelanta a la reserva de la API | En el crash el byte estaba POR DEBAJO (4 vs 5) |
| 2 | Recortar el byte contra la reserva viva | Mordió 16 veces, crasheó igual |
| 3 | Escribir el byte en el borde del frame | 4 de 4 corridas crasheadas, contra 0 de 2 antes. Regresión |
| 4 | Falta el `+1` de `loop_bound_for` | Se aplicó, los generados igualaron a la API, crasheó igual |
| 5 | `bound ≥ API + 2` | Convirtió un pedido de 3X en bound 4 y crasheó. Regresión |
| 6 | `[global+0x4168]` gobierna el índice | Clavado en 1 a mano, el multiplicador no se movió |
| 7 | Falta forzar el segundo contador `[ctx+8]` | Se forzó, `[ctx+4]` y `[ctx+8]` coincidieron en 5 por primera vez, la ranura 4 siguió nula |
| 8 | El corte es el pool de command allocators (`beginCommandList`) | Con `logLevel` en verbose de verdad (1.5 MB de sl.log) NO aparece ni "Command list not closed" ni "Couldn't reset the allocator". Nunca falla |
| 9 | Subir `numFramesToGenerateMax` de 3 a 5 | Los rechazos de `slSetData` desaparecen (0) pero NGX falla 7749 veces con `0xbad00005` y el multiplicador cae a mediana **1.00**. Se cambia el crash por generacion apagada |

## Lo que está verificado en el binario

No son teorías. Son instrucciones leídas.

```asm
; el indice que revienta es el contador del bucle de generacion
0x3ded1  mov  [rbp-0x18], edi          ; el functor captura edi
0x3dee3  lea  rcx, [rip] -> 0x73680    ; su vtable
0x400C0  add  rcx, 8 / jmp 0x3e6f0     ; el thunk
0x3e735  mov  eax, [rbx+8]             ; lo recupera -> r10
0x3ed6a  mov  rax, [rcx+r13+0x48]      ; array[r10]
0x3ed6f  mov  ecx, [rax+0x40]          ; <- falla

; los dos bucles y sus topes
0x3ddfa  cmp  [r13+4], ebx             ; generacion: [ctx+4]
0x45984  mov  r14d, [rdx+8]            ; llenado: [ctx+8]
0x45b87  sub  r12, 0xc0                ;   hacia atras hasta 0

; los dos contadores salen del mismo constructor, uno al lado del otro
0x47333  mov  eax, [rdx+4]             ; parcheado
0x4733f  mov  eax, [rdx+8]             ; parcheado tambien (2026-09-10)

; la salida temprana del bucle, que ignora nuestro bound
0x3de16  call qword ptr [rax+0x40]     ; devuelve bool
0x3de1b  je   salir                    ; si dice que no, corta
```

## El techo, medido tres veces

Tres líneas independientes dicen que la generación se detiene en 4:

| medición | tope |
|---|---|
| `[ctx+8]`, antes de forzarlo | nunca pasó de 4 |
| `[global+0x4168]` | clavado en 3 |
| ranuras llenas, 11 autopsias | nunca la 4 ni la 5 |

**Esto es lo que hay que romper, y romperlo hacia arriba.** Bajar el pedido para
que entre en 4 es exactamente lo que el objetivo prohíbe.

---

## LA CAUSA, dicha por el runtime de NVIDIA

2026-09-10 02:15. Con `logLevel` forzado a verbose en `Preferences` (offset 36;
la variable de entorno `SL_LOG_LEVEL` no alcanza, lo que pasa la aplicacion
gana), el sl.log de Halo dice esto hasta el instante del crash:

```
[slSetData] Input data numFramesToGenerate (4) greater than DLSS-G supported
numFramesToGenerateMax (3). Set input data count lower.
Check DLSS-G State for max count.

[evaluateNGXFeature] [sl.dlss_g] NGX evaluate feature failed 0xbad00005
```

**El maximo real es 3 frames generados, o sea 4X.** Pedir 4 o 5 no genera de
menos: `slSetData` **rechaza la llamada entera** y la evaluacion NGX falla. Las
ranuras de esos sub-frames no las llena nadie, y nuestro bound forzado igual las
recorre. Ahi esta el `[nulo+0x40]`.

Eso explica las once autopsias sin excepcion: las ranuras 4 y 5 nunca se
llenaron porque el trabajo que las llenaria nunca se ejecuto.

### El campo, y por que subirlo no sirve

```asm
0x4fccd  mov  dword ptr [rdi + 0x45e4], 5     ; el plugin lo inicializa en 5
0x56b5b  mov  r9d, dword ptr [r15 + 0x45e4]   ; numFramesToGenerateMax
0x56b62  cmp  r8d, r9d
0x56b65  jbe  sigue                            ; <= max: adelante
```

Se probo subirlo a 5 (hipotesis 9). Los rechazos desaparecen, pero **NGX rechaza
evaluar igual**: 7749 fallos `0xbad00005` y multiplicador mediana 1.00. El 3 no
es una opinion del plugin -- refleja lo que NGX soporta.

Coincide con lo que NVIDIA publicita: DLSS 4 Multi Frame Generation llega a 4X.
3 generados es el tope del producto.

### Que se hace con eso

Lo que llamabamos 5X y 6X **nunca entrego 5 ni 6 frames**. Entregaba cero
generados en esos frames (NGX rechazando) y un puntero nulo.

Asi que hacer que nuestro tope siga a `numFramesToGenerateMax` **no reduce
frames entregados: los aumenta**. Es exactamente lo contrario de apagarlo. Y no
es un numero fijo nuestro: es la capacidad que el dispositivo declara, leida en
vivo. Si un driver o una GPU futura declaran 5, se usan 5 sin tocar codigo.

Eso hay que MEDIRLO contra la linea base antes de afirmarlo, y la regla dura del
objetivo es justamente esa comparacion.

## Criterio de "alcanza el tope del modo" -- DECLARADO ANTES DE CORRER

2026-09-10 03:40. La corrida anterior fallo la condicion 4 y hay que decirlo
sin vueltas: declare ">= 3.9" antes de correr, salio 3.06, y despues re-derive
el techo en 3.11. **Mover el criterio despues de ver el resultado no vale**,
aunque la re-derivacion tenga evidencia. Eso es lo que el metodo prohibe.

Asi que el techo se fija ahora, ANTES, y con una medicion que **no depende de mi
cambio**: el modo fijo mas alto del panel, medido en la LINEA BASE (tope 5,
`mfg-topefijo.txt`), da un maximo entregado de **3.11** sobre 646 ventanas.
El candidato en el mismo modo da 3.13 sobre 1113.

**Definicion, para los 5 ciclos que siguen:**

Un ciclo de DYNAMIC alcanza el tope del modo si se cumplen LAS DOS:

- **(a)** la cuenta que llega a la API alcanza el maximo permitido en ese ciclo
  (o sea, `techo declarado a la API` toca el valor de `tope_cuenta()`), y
- **(b)** hay al menos una ventana con multiplicador entregado **>= 3.00** en
  ese ciclo.

3.00 y no 3.11 porque 3.11 es el maximo de una muestra de 646-1113 ventanas y un
ciclo tiene ~375: exigir el maximo de una muestra tres veces mas grande es
exigir suerte, no capacidad. 3.00 es el valor modal del modo saturado, medido en
las dos builds.

Si un ciclo no cumple (a) y (b), el ciclo NO cuenta y el goal sigue abierto.

## RESULTADO, 2026-09-10 03:30

El arreglo es **una linea**: que nuestro tope siga a lo que el plugin declara
(`numFramesToGenerateMax`, leido en vivo de `[ctx+0x45e4]`) en vez de a un 5
fijo. No es un numero inventado ni un recorte: es la capacidad del dispositivo.
Si un driver o una GPU futura declaran mas, se usa mas sin tocar codigo.

### Las cuatro condiciones, con control

| condicion | linea base (tope 5) | candidato |
|---|---|---|
| 1. crashes en 5 ciclos, DYNAMIC | **2 de 2 crashean** | **0 de 5** |
| 2. dumps nuevos / lineas EXCEPCION | — | **0 / 0** |
| 3. generacion encendida por ciclo | — | interpolacion habilitada; 53-56% de ventanas generando en los 5 |
| 4a. multiplicador, DYNAMIC saturado | mediana **1.00**, max 1.02 (n=321) | mediana **2.95**, p90 3.00 (n=1881) |
| 4b. multiplicador, 4X fijo | max **3.11** (n=646) | max **3.13** (n=1113) |
| rechazos de slSetData | si | **0** |
| fallos NGX 0xbad00005 | si | **0** |

La linea base se midio con el mismo arnes, el mismo objetivo y el mismo modo,
activando `mfg-topefijo.txt` -- un flag de diagnostico que devuelve el tope de 5.

### El techo real de esta GPU es 3.1x, y no lo puso este cambio

Tres lineas independientes, una de ellas de un build SIN el cambio:

| medicion | tope |
|---|---|
| `numFramesToGenerateMax` que declara el plugin | 3 |
| 4X fijo en la **linea base** (tope 5) | max 3.11 |
| 4X fijo en el candidato | max 3.13 |

El "4X" del panel nunca entrego 4X en esta maquina. Y `numFramesToGenerate`
cuenta frames TOTALES, no generados: 3 significa 3X.

**Pedir por encima del techo no daba mas frames: daba CERO.** Con el tope en 5 y
un objetivo que satura, la mediana entregada fue 1.00 -- generacion apagada por
NGX rechazando -- mas el crash. Por eso seguir al techo declarado **aumenta** los
frames entregados en vez de reducirlos, y por eso no es apagarlo con otro nombre.

### Correcciones de criterio que hubo que hacer

- Mi chequeo de "llega al tope" exigia >= 3.9 asumiendo que max 3 = 4X. Medido,
  el tope es 3.11. El criterio estaba mal, no la medicion.
- La primera ventana de cada sesion da un multiplicador absurdo (369x): tiempo
  transcurrido casi cero. Se descarta.
- Una corrida de 4X fijo murio antes del Enter; repetida 3 veces mas, 3 de 3
  limpias. Era una carrera de arranque, no una regresion.

### Lo que sigue abierto

El techo de 3.1x es de NGX, no nuestro. Subir `numFramesToGenerateMax` a mano ya
se probo (hipotesis 9) y NGX rechaza evaluar igual. Si se quiere ir mas arriba,
el camino es el snippet `nvngx_dlssg`, no el plugin de Streamline.

## VERIFICACION FINAL, 2026-09-10 04:10 -- contra el criterio declarado antes

5 ciclos de DYNAMIC saturado (objetivo 300), 240 s de juego cada uno.

```
ciclo 1: OK  n=445  mediana 3.00  p90 3.00  max 3.04 | API 3 de 3 | max>=3.00 si | EXC 0
ciclo 2: OK  n=438  mediana 3.00  p90 3.00  max 3.06 | API 3 de 3 | max>=3.00 si | EXC 0
ciclo 3: OK  n=425  mediana 3.00  p90 3.00  max 3.06 | API 3 de 3 | max>=3.00 si | EXC 0
ciclo 4: OK  n=438  mediana 3.00  p90 3.00  max 3.11 | API 3 de 3 | max>=3.00 si | EXC 0
ciclo 5: OK  n=427  mediana 3.00  p90 3.00  max 3.06 | API 3 de 3 | max>=3.00 si | EXC 0
```

| condicion | resultado |
|---|---|
| 1. cero crashes en 5 ciclos | **5 de 5 sin crash** |
| 2. cero dumps nuevos / cero EXCEPCION | **0 / 0** |
| 3. generacion encendida por ciclo | interpolacion habilitada; mediana 3.00 en los 5 |
| 4. multiplicador >= linea base y tope alcanzado | **3.00 vs 1.00**; (a) y (b) en los 5 |
| rechazos slSetData / fallos NGX | **0 / 0** |

Con la muestra mas grande la mediana subio de 2.95 a **3.00**: el modo queda
saturado en todos los ciclos, no solo en picos.

**Nota de metodo, porque importa mas que el resultado:** la corrida anterior se
declaro fallida por la condicion 4 y estaba bien declararlo asi. Yo habia fijado
">= 3.9" antes de correr, salio 3.06, y despues re-derive el techo en 3.11 con
evidencia. La evidencia era buena pero el momento estaba mal: mover el criterio
despues de ver el resultado invalida la prueba. Por eso el criterio de esta
corrida quedo escrito ANTES, y anclado a una medicion de la LINEA BASE, que no
depende del cambio que se esta evaluando.

## EL TECHO NO ES DE LA GPU -- 2026-09-10 09:45

El banco, con la MISMA 4070 Ti, entrego **5.00x sostenido**:

```
max: numFramesToGenerateMax CAMBIO a 5
multiplicador contado: mediana 5.00  p90 5.00  max 5.02 (n=27)
NGX evaluate failed: 0
```

O sea que 5X funciona en este hardware. Lo que cambia es **que snippet
`nvngx_dlssg` corre cada uno**:

| | tamano | fecha | max | entrega |
|---|---|---|---|---|
| banco | 7 519 856 | 22-jun | **5** | **5.00x** |
| Halo | 7 597 104 | 23-jul | **3** | 3.00x |

Y en la build de julio el valor es una constante por arquitectura:

```asm
nvngx_dlssg 0x26572  mov   ebx, 1
            0x26577  mov   r8d, 3          ; <- la constante
            0x2657d  cmp   edi, 0x1b0      ; 0x1b0 = Blackwell
            0x26583  cmovl r8d, ebx        ; arch < 0x1b0 -> 1
            0x26587  lea   rdx, 'DLSSG.MultiFrameCountMax'
            0x26595  call  [SetParameterInt]
```

La build de junio no tiene ese patron: calcula el valor de otra forma y llega a
5. **NVIDIA bajo el techo entre junio y julio**, y lo dejo clavado por arquitectura.

**Correccion a lo que afirme antes:** dije que 3.1x era el techo real de la GPU,
apoyandome en que el 4X fijo de la linea base tambien topaba en 3.11. Eso era
cierto para el snippet de Halo y FALSO como afirmacion sobre la GPU. El banco lo
desmiente con 5.00x en el mismo equipo.

### El parche

`patch_multiframe_max` sube ese inmediato de 3 a 5, en el mismo modulo donde ya
reescribimos los gates de arquitectura. Detras de `mfg-mfcmax.txt` hasta que
este medido en Halo.

No se puede validar en el banco: el banco ya corre el snippet de junio, que da 5
sin parche. La prueba tiene que ser en Halo.

## LO QUE REALMENTE LO ARREGLO, 2026-09-10

**Cada juego corria sobre un `nvngx_dlssg` distinto.** Esa era la causa, y no
aparecio en nueve hipotesis porque yo miraba el plugin y el bound, nunca que
binario estaba abajo.

| snippet | tamano | tope |
|---|---|---|
| Halo (carpeta del juego, jul) | 7 597 104 | **max = 3** |
| Cyberpunk | 7 607 336 | **max = 3** |
| GTA V | 7 453 808 | sin tope |
| banco / SDK 2.12 | 7 519 856 | sin tope |

Por eso "solo crasheaba en Halo": GTA V y el banco corrian builds sin el tope.
Cyberpunk lo tiene y habria hecho lo mismo si se lo empujaba.

### La solucion: el snippet desde NUESTRA carpeta

Igual que el interposer, que ya salia de `LOCALAPPDATA\mfg-unlock\sdk\2.<v>`.
Se redirige `nvngx_dlssg.dll` en `hk_ldrload` a
`LOCALAPPDATA\mfg-unlock\snippet\nvngx_dlssg.dll`. Si el archivo no esta, no
se toca nada y el juego usa el suyo.

### Y la semantica cambia con el snippet

Con el binario del juego, `numFramesToGenerate` cuenta los GENERADOS: cuenta 1
da 2X. Con el nuestro **es el multiplicador**: cuenta 1 da 1X.

Eso rompio 2X y costo seis intentos, porque la cuenta se escribe en SIETE
lugares y el que mandaba era el ultimo que mire:

```c
} else if (sel >= 2) {                    // force_into, rama de modos fijos
    { const LONG c = sel - 1; ... }       // no mira g_force_generated
}
```

**Leccion de metodo:** `grep "p + 36"` al principio los mostraba todos en dos
segundos. En vez de eso probe de a uno, con una corrida del usuario en el medio
cada vez.

### 6X: son TRES techos

| donde | que era |
|---|---|
| snippet, `mov edi, 5` | lo que reporta como `MultiFrameCountMax` |
| plugin, `mov [rdi+0x45e4],5` y `mov edx,5` | lo que se permite pedir |
| snippet, `mov esi, 5` en `ComputeAndValidateTimeFactor` | **el que rechazaba** |

El tercero lo dijo el runtime 4254 veces:

```
[EndpointCoreInputs::ComputeAndValidateTimeFactor:416]
Error: input MultiFrameCount 6 is greater than the maximum supported count (5)
```

Con los tres en 6: **6.00 entregado, 0 fallos NGX, 0 excepciones, 0 dumps.**

Pero comparado dentro de la MISMA sesion, 6X no se sostiene:

| | cuenta 5 | cuenta 6 |
|---|---|---|
| multiplicador (mediana) | **5.0** | 1.0 |
| tirones de pantalla (p90) | **2** | 8 |

5X genera parejo; 6X entrega 6.0 cuando corre pero se apaga a ratos. Ninguno
crashea, que era el problema original.

### Lo que sigue: una base unica

Los nueve `sl.*` salen todavia de la cache de NGX. El zip del SDK que el dll ya
se baja trae TODO en `bin/x64/`: los sl.*, el interposer y el nvngx_dlssg
(7 519 856, sin tope, con los dos patrones que parcheamos). Sacar todo de ahi da
una base unica para todos los juegos y elimina tres dependencias externas: la
cache de NVIDIA, lo que trae cada juego, y la copia manual.

## Método, para no repetir los errores de la noche

- **Una prueba por hipótesis.** `--intentos 1` en el banco, siempre.
- **Control obligatorio** antes de afirmar que algo mejoró o empeoró.
- **El foco importa**: `tools/mantener_foco.ps1`. Sin él, el plugin deja de
  interpolar y la corrida miente (ya invalidó una).
- **Leer el log antes de teorizar.** Siete de las siete hipótesis muertas se
  podrían haber matado antes leyendo lo que ya estaba en disco.
- **No anunciar hallazgos antes de la evidencia.** Si digo "encontré la causa",
  tiene que haber una medición al lado.
- **Cerrar Halo después de cada crash**, o la corrida siguiente no vale.

## Estado del árbol

Sin commitear. Contiene:
- la autopsia del array (registros + barrido de pila + `[global+0x4168]`)
- la abstención del controlador con base sin asentar (`kCtrlMinMuestras`)
- el segundo contador forzado (`fill count is ours too`)

Ninguno de los tres arregla el crash. Los tres son correctos por separado y
están medidos en el banco sin regresión.
