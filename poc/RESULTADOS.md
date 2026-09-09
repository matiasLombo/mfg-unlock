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

Pendiente.

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

Limite NO probado todavia: la POC crea sus dos swapchains con la misma llamada y
en el mismo proceso. Falta ver si la vtable sigue siendo la misma cuando otro
overlay devuelve un objeto ENVUELTO -- un proxy con vtable propia. Ese es el caso
de Halo y esta POC no lo reproduce.

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

Pendiente.

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

