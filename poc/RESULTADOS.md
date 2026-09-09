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

Pendiente.

## M3. GetLastPresentCount y GetFrameStatistics

Pendiente.

## M4. D3DKMT sin privilegios

Pendiente.

## M5. DwmGetCompositionTimingInfo

Pendiente.

## M6. NVAPI: consulta publica del multiplicador

Pendiente.
