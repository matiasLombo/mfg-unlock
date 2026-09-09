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

Pendiente.

## M4. D3DKMT sin privilegios

Pendiente.

## M5. DwmGetCompositionTimingInfo

Pendiente.

## M6. NVAPI: consulta publica del multiplicador

Pendiente.
