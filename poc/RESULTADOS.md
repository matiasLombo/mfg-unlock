# Veredictos de las POCs

Un mecanismo sin POC no tiene veredicto. Cada linea usa una de tres formas:

    ANDA: <numero medido> contra <numero del oraculo M7>, diferencia <X>%
    NO ANDA: <que devolvio> y por que no sirve
    NO SE PUDO PROBAR: <que falto>, y que haria falta para probarlo

Cada veredicto dice de que corrida salio y con que binario.

## M7. PresentMon como oraculo externo

**NO SE PUDO PROBAR**: falta elevacion. Falta que alguien acepte una vez el
aviso de UAC; despues la POC corre sola.

Lo que si esta establecido, y ahorra la descarga: **no hay que bajar nada**.
`PresentMon_x64.exe` version 1.9.12728.0 (1.108.080 bytes) ya esta en la maquina,
incluido en `C:\Program Files\NVIDIA Corporation\FrameViewSDK\bin\`. Junto a el
hay un SDK con header (`FvSDK.h`) que expone FPS, AvgFPS, FPS99, FPSLow,
RenderLatency, RenderPresentLatency, AvgSWPCLatency, ProcessName y Frame, y un
servicio (`nvfvsdksvc_x64.exe`) que NO esta registrado.

Medido: sin elevar, `PresentMon_x64.exe --timed 3 --output_file X
--no_console_stats --stop_existing_session --terminate_after_timed` sale con
**exit code 1**, sin stdout, sin stderr y sin crear el CSV. Coincide con lo que
dice la documentacion de Microsoft: consumir ETW en tiempo real solo lo pueden
hacer procesos elevados, el grupo "Performance Log Users" o servicios del
sistema.

POC: `poc/m7-oraculo/correr.ps1`. Corrida del 2026-09-08, sin elevar.

## M1. slDLSSGGetState observado, no consultado

**NO ANDA para contar presentaciones**: observado da ~20 veces de mas, no el
total. La hipotesis de que el contador se reiniciaba por llamadas del juego
queda REFUTADA.

**Pero el campo resulto ser otra cosa, y mejor**: el offset 48 es el
MULTIPLICADOR VIVO, no un contador de frames.

    multiplicador pedido    valor medio por llamada
        2x                       1.94
        3x                       2.99
        4x                       3.99

Devuelve lo mismo en cada llamada sin importar con que frecuencia se pregunte --
por eso sumarlo daba 2396 cada 2 segundos contra 120 presentaciones reales. No es
"frames presentados desde la ultima llamada"; es cuantos frames se estan
presentando por cada uno renderizado, ahora.

**Esto es exactamente lo que M6 dijo que NVAPI no expone.** Esta disponible por
una API publica de Streamline, en cualquier juego con DLSS-G, sin parche de
bytes, sin ETW y sin privilegios. Es el numero que el HUD no tiene en Halo.

Como se llego, y los dos intentos que fallaron primero:

  1. Reemplazar el dll del banco por la POC: el sample murio en STATUS INVALID a
     los 4 segundos.
  2. Convivir cargando el dll real y enganchar `slGetFeatureFunction`: el sample
     vivio bien pero se observaron 0 llamadas. El dll real engancha esa misma
     funcion y se carga primero, asi que las peticiones pasan por SU hook y el
     nuestro queda a la sombra.
  3. Lo que si funciono: pedir el puntero uno mismo con
     `slGetFeatureFunction(1000, "slDLSSGGetState", fn)` y enganchar **el cuerpo**
     de la funcion. Es la misma direccion para todos los llamadores, asi que no
     importa quien intermedie.

        punteros obtenidos: GetState 00007ff8ebbf8660  SetOptions 00007ff8ed987de0

El viewport y las opciones que hacen falta para consultar se capturan de la
llamada que el anfitrion hace a `slDLSSGSetOptions`.

POC: `poc/m1-observado/m1.cpp`, g++ con MinHook, m1.dll de 109.644 bytes.
Corridas del 2026-09-08 sobre `bundled-2.12.0`, 20-25 s, multiplicadores 2x, 3x
y 4x.

## M2. Vtable del swapchain via swapchain propio

**ANDA**: 300 presentaciones contadas contra 300 reales, diferencia 0.0%.

La verdad no viene del oraculo M7 sino de la propia POC, que es mejor testigo
para este caso: la app ES la que llama a Present, asi que su contador no estima
nada.

Como se probo. Se crean dos swapchains en el mismo proceso:

    A -- el "del juego", creado PRIMERO, sobre su ventana
    B -- el nuestro, descartable, creado DESPUES, sobre otra ventana

Se lee la vtable a traves de B, se parchea la ranura 8 (Present) y se presenta
300 veces SOLO por A. El hook conto 300.

    vtable de A: 00007ff910b0c688
    vtable de B: 00007ff910b0c688
    comparten vtable: SI

Las dos instancias comparten vtable porque la implementacion de DXGI es la
misma, asi que parchear por cualquiera engancha a todas las del proceso. No
depende de ver la creacion del swapchain del juego ni de quien envolvio la
factory.

Por que importa: hoy el dll engancha `CreateSwapChainForHwnd` en la factory, y en
Halo -- con ReShade como dxgi.dll, DLSS Enabler, renodx y UE4SS encadenados -- no
hay una sola linea de swapchain en todo el log. Este metodo no tiene esa
dependencia.

**Ampliado, y el limite que estaba marcado quedo cubierto en parte.** El mismo
metodo se probo despues dentro del sample del banco, que es una aplicacion real
de Streamline con DLSS-G andando -- no un caso de juguete. Ahi el hook por vtable
conto **4288 presentaciones, unas 480 cada 2 segundos, ~240 fps sostenidos**
durante toda la corrida (ver la POC de M1, que lo usa como testigo).

Asi que el metodo funciona con Streamline en el medio. Lo que sigue SIN probar es
el caso de Halo, con ReShade como `dxgi.dll`, DLSS Enabler, renodx y UE4SS
encadenados: ahi puede haber un proxy con vtable propia y esta POC no lo
reproduce.

POC: `poc/m2-vtable/m2.cpp`, compilado con g++
(`g++ -std=c++17 -O2 -o m2.exe m2.cpp -ld3d11 -ldxgi -luser32 -lole32`).
Corrida del 2026-09-08, binario m2.exe de 100.090 bytes.

## M3. GetLastPresentCount y GetFrameStatistics

**ANDA GetLastPresentCount**: delta 120 contra 120 reales, 0.0%, en los dos modos
de swapchain.

**NO ANDA GetFrameStatistics**: falla en los dos modos.

    modo DISCARD (clasico)
      GetLastPresentCount  delta 120 de 120   hr=0
      GetFrameStatistics   hr=0x887A0004

    modo FLIP_DISCARD (moderno, el que usan los juegos nuevos)
      GetLastPresentCount  delta 120 de 120   hr=0
      GetFrameStatistics   hr=0x887A000B = DXGI_ERROR_FRAME_STATISTICS_DISJOINT

Los dos son metodos del propio swapchain: no necesitan ETW, ni privilegios, ni
enganchar la creacion. Alcanza con TENER el swapchain, que es justo lo que M2
resolvio.

**Esto contradice lo que hace hoy el dll.** `src/proxy.cpp` toma el contador de
presentaciones con `GetFrameStatistics`, que es el que falla en los dos modos
probados aca. `GetLastPresentCount` es exacto en los dos y ni siquiera se usa.

Ojo con no sobreinterpretar: esta POC corre en una ventana chica sin composicion
exclusiva, y `GetFrameStatistics` documenta que necesita condiciones que aca no
se dan. En GTA V si devuelve datos -- el campo aparece 6206 veces en una partida.
Lo que la POC demuestra es que **hay un contador que anda donde el otro no**, no
que el otro este roto siempre.

POC: `poc/m3-contadores/m3.cpp`, g++
(`g++ -std=c++17 -O2 -o m3.exe m3.cpp -ld3d11 -ldxgi -luser32 -lole32`).
Corrida del 2026-09-08, 120 presentaciones con vsync por modo.

## M4. D3DKMT sin privilegios

**NO SE PUDO PROBAR**: falta el layout exacto de `D3DKMT_QUERYSTATISTICS` para
esta version de Windows. Haria falta sacarlo de los headers del WDK, que no estan
en esta maquina, o de una fuente confiable -- no inventarlo.

Lo que si quedo establecido, y no es poco:

    entry points en gdi32.dll, todos presentes:
      D3DKMTQueryStatistics      SI     D3DKMTGetPresentHistory   SI
      D3DKMTOpenAdapterFromHdc   SI     D3DKMTQueryAdapterInfo    SI
      D3DKMTCloseAdapter         SI     D3DKMTGetDeviceState      SI

    D3DKMTQueryStatistics(PROCESS) sin elevar -> NTSTATUS 0xC000000D
    con el LUID del adaptador (00000000-000130EB) sacado de DXGI -> mismo codigo

**0xC000000D es STATUS_INVALID_PARAMETER, no STATUS_ACCESS_DENIED.** Eso importa:
la barrera NO es la elevacion. Las funciones se pueden llamar desde un proceso
comun; lo que falta es armar bien la estructura.

Es la diferencia con ETW, donde la barrera si es de privilegios y esta cerrada
(ver `docs/medicion-agnostica.md`, punto 2). Aca la puerta esta abierta y lo que
falta es la llave correcta.

Que haria falta para cerrarlo: el `d3dkmthk.h` del WDK, o la definicion publicada
de la estructura para esta build de Windows. Con eso la POC ya esta escrita y
solo hay que corregir el armado.

POC: `poc/m4-d3dkmt/m4.cpp`, g++ con -lgdi32. Corrida del 2026-09-08, sin elevar.

## M5. DwmGetCompositionTimingInfo

**NO ANDA**: la app presento 120 frames y todos los contadores de DWM se movieron
+1. No sigue a la aplicacion.

    presentadas por la app (verdad): 120
      cFrame            +1
      cRefresh          +1
      cDXPresent        +1
      cFramesDisplayed  +0

La llamada funciona (hr=0) y no necesita privilegios, pero mide composicion del
escritorio, no presentaciones por proceso. Descartado para contar frames de una
aplicacion.

POC: `poc/m5-dwm/m5.cpp`, g++ con -ldwmapi. Corrida del 2026-09-08, 120
presentaciones con vsync en FLIP_DISCARD.


## M6. NVAPI: consulta publica del multiplicador

**NO ANDA**: no existe. En la tabla publica de entry points de NVAPI, 152
entradas, no hay ninguna consulta del multiplicador de frame generation en curso.

Como se respondio, sin buscar opiniones: `nvapi64.dll` de esta maquina (574.032
bytes, 2026-08-22) **no contiene los nombres** de las funciones -- NVAPI resuelve
por ID y los strings estan afuera, asi que leer el binario no sirve. Se leyo
entonces la tabla de interfaces que dxvk-nvapi mantiene, que es el espejo publico
de la lista de NVIDIA (`src/nvapi_interface.cpp`, 152 entradas).

Lo unico cercano:

    NvAPI_NGX_GetDriverFeatureSupport   soporte del driver, no el multiplicador
    NvAPI_NGX_GetNGXOverrideState       estado del override
    NvAPI_NGX_SetNGXOverrideState       lo mismo, escribiendo

Ninguna devuelve cuantos frames se estan generando ahora.

Coherente con lo que ya se sabia: el indicador de NVIDIA no consulta nada, es una
opcion de depuracion de NGX (`DLSSGIndicator`) que dibuja desde adentro de la
tuberia.

**Hallazgo lateral que puede servir para la latencia**: `NvAPI_D3D_GetLatency` SI
es publico, junto con `NvAPI_D3D_GetSleepStatus`, `NvAPI_D3D_SetLatencyMarker`,
`NvAPI_D3D_SetSleepMode` y `NvAPI_D3D_Sleep`. Es el informe de latencia de
Reflex por NVAPI, un camino distinto del informe de Streamline -- que en Halo da
0 en 316 ventanas. NO PROBADO todavia; seria una POC nueva.

Salvedad honesta: dxvk-nvapi espeja la lista PUBLICADA. Si NVIDIA tiene entradas
privadas sin documentar, esto no las ve.

