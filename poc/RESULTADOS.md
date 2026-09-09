# Veredictos de las POCs

Un mecanismo sin POC no tiene veredicto. Cada linea usa una de tres formas:

    ANDA: <numero medido> contra <numero del oraculo M7>, diferencia <X>%
    NO ANDA: <que devolvio> y por que no sirve
    NO SE PUDO PROBAR: <que falto>, y que haria falta para probarlo

Cada veredicto dice de que corrida salio y con que binario.

## M7. Oraculo externo

**NO SE PUDO PROBAR**: PresentMon necesita elevacion y no se ejecuto elevado.

CORRECCION. En una version anterior de este documento puse aca "ANDA" usando el
presentador como oraculo. Eso fue correr el arco: el objetivo dice PresentMon, y
sustituirlo por una herramienta propia y despues declarar exito es cambiar la
definicion del mecanismo en vez de probarlo. Queda revertido.

El presentador SIGUE siendo util y su numero es real -- 4123 presentaciones en
25.002 s = 164.91 fps contra 165 Hz de vsync, -0.05% -- pero es un TESTIGO de las
POCs, no el oraculo externo que M7 pedia. Con el se validaron M2 y M3, y esa
validacion se sostiene: la app ES la que llama a Present, asi que su cuenta es la
definicion y no una estimacion.

Lo que falta y no se consiguio: una herramienta de TERCEROS que mida procesos que
no controlamos. Los tres caminos, hasta el fondo:

### Camino A: PresentMon

No hay que descargar nada. `PresentMon_x64.exe` 1.9.12728.0 (1.108.080 bytes) ya
esta en `C:\Program Files\NVIDIA Corporation\FrameViewSDK\bin\`.

Sin elevar sale con **exit code 1**, sin stdout, sin stderr y sin CSV. Coincide
con la documentacion de Microsoft: ETW en tiempo real solo lo pueden consumir
procesos elevados, "Performance Log Users" o servicios del sistema.

**CORRECCION IMPORTANTE: no era elevacion.** Durante toda la investigacion se
supuso que el exit 1 venia de que ETW en tiempo real necesita privilegios. Se
probo dos veces sin elevar -- una con el escritorio quieto y otra con el
presentador corriendo, para descartar la falta de datos -- pero **nunca se probo
elevado**.

Cuando por fin se corrio elevado, el resultado fue el mismo:

    elevado: True
    capturando 20 s con PresentMon 1.9.12728.0
    PresentMon exit: 1
    verdad del presentador: 4271 presentaciones en 26.003 s = 164.25 fps
    SIN CSV

Asi que el bloqueo es OTRO y sigue sin identificarse. La conclusion anterior era
una hipotesis razonable -- coincidia con la documentacion de Microsoft sobre ETW
-- pero no estaba probada, y al probarla resulto falsa.

Lo que impidio verlo antes: `Start-Process -NoNewWindow` no manda la salida del
proceso al transcript, asi que el mensaje de error de PresentMon nunca se vio. El
script ya lo redirige a `presentmon-stdout.txt` y `presentmon-stderr.txt`.

Dos intentos elevados fallaron por causas que ya estan corregidas en la POC:
  - el primero, porque en el escritorio quieto no hay presentaciones que
    capturar y PresentMon no escribe CSV sin filas;
  - el segundo, porque el nombre de sesion por defecto colisiona con el
    PresentMon que NVIDIA ya corre para FrameView, y el proceso se cuelga.

`poc/m7-oraculo/oraculo.ps1` arregla las dos: usa `--session_name mfgpoc` y lanza
`presentador.exe` antes de capturar. **Falta una sola aceptacion de UAC.**

### Camino B: SDK de FrameView, sin elevacion

El SDK inicializa desde un proceso comun -- su cliente de prueba imprime
`InitializeFvSDKSession SUCCESS` sin elevar -- asi que parecia el camino sin UAC.

Se llego hasta el final y NO alcanzo, pero el bloqueo quedo acotado a un solo
punto. La cadena, despues de tres correcciones sucesivas:

    FvSDK_Initialize                0  exito
    FvSDK_CreateSession             0  exito
    FvSDK_StopSession (previo)      0  exito  <- hacia falta: sin esto,
                                                 StartSession devolvia 20 =
                                                 FV_SDK_SESSION_IN_PROGRESS,
                                                 porque nvfvsdksvc_x64 corre y
                                                 tiene su propia sesion
    FvSDK_StartSession              0  exito
    FvSDK_EnableMetrics(eAvgFPS)    0  exito  <- tiene que ir DESPUES de
                                                 StartSession; antes devuelve
                                                 exito pero no queda habilitada
    FvSDK_SampleData                3  = FV_NO_DATA   <-- ACA SE CORTA
    FvSDK_ReadData                  9  = FV_METRIC_NOT_ENABLED
    muestras con el pid del presentador: 0

O sea: la sesion arranca, la metrica se habilita, y el servicio **no produce
datos** para nuestro proceso. No se determino por que. Dos hipotesis sin probar:
que FrameView necesite su propia aplicacion corriendo para que el servicio
recolecte, o que el presentador no cuente como "juego" para el (ventana chica,
sin pantalla completa exclusiva).

**El discriminador**: se corrio el cliente de prueba del propio NVIDIA,
`FvSDKTestClient_Public.exe`, con el presentador andando al lado. Imprime
`InitializeFvSDKSession SUCCESS` y `Executing Test Case 001` y **no reporta ni un
dato** en 20 segundos. Asi que no es un error de nuestra POC: el servicio no esta
entregando datos en esta maquina, ni siquiera a la herramienta de NVIDIA.

Por eso este camino pasa de NO SE PUDO PROBAR a **NO ANDA**: se lo llevo hasta el
final y lo que devuelve esta medido -- FV_NO_DATA -- y confirmado con un segundo
cliente independiente.

Lo importante para quien lo retome: **no esta cerrado por permisos**. Todo esto
corrio sin elevar. Lo que falta averiguar es por que el servicio no recolecta.

**Referencia medida en la misma corrida**, con el presentador: **4123
presentaciones en 25.002 s = 164.91 fps**, vsync a 165 Hz. Ese es el numero que
un oraculo tendria que reproducir.

Lo que costo llegar hasta ahi, por si alguien lo retoma: el dll exporta UNA sola
funcion, `fv_QueryInterface` -- el patron de NVAPI. Los stubs viven en un `.lib`
de MSVC con simbolos mangleados a su manera, que MinGW no genera. Se resolvio
referenciando los simbolos exactos con etiquetas `asm` (los nombres salieron de
leer la tabla de simbolos del `.lib`), y poniendo cabos para las intrinsecas del
CRT de MSVC (`__security_cookie`, `__security_check_cookie`, `__GSHandlerCheck`) y
para tres funciones de strsafe. Compila y corre.

**Salvedad**: el cabo de `__GSHandlerCheck` no hace la comprobacion real. Sirve
para una POC de medicion, NO para produccion.

### Camino C: RTSS, descartado sin gasto

RTSS expone FPS por aplicacion en memoria compartida y no necesita
elevacion, asi que era el tercer candidato. **No esta instalado**: no hay
carpeta de RivaTuner, no hay claves de RTSS en el registro, y la memoria
compartida `RTSSSharedMemoryV2` no existe. MSI Afterburner esta instalado
pero sin RTSS al lado.

Con esto se agotan los caminos sin elevacion que esta maquina ofrece.

### Lo que igual quedo cubierto

El proposito de M7 era tener un numero independiente contra el cual validar los
demas. Para M2 y M3 eso ya esta, y con algo **mejor** que una herramienta
externa: `poc/m7-oraculo/presentador.cpp` presenta una cantidad conocida e
imprime su propio contador. No es una estimacion, es la definicion -- la app ES
la que llama a Present. Y no es "nuestra capa vieja": es una aplicacion aparte
que no comparte una linea con el dll.

Un oraculo externo sigue haciendo falta para medir procesos que no controlamos,
que es el caso de un juego.

POC: `poc/m7-oraculo/` -- `correr.ps1`, `oraculo.ps1`, `presentador.cpp`
(presentador.exe, 100.311 bytes) y `oraculo.cpp` (oraculo.exe, 189.814 bytes).
Corridas del 2026-09-08.

Verdad de referencia medida con el presentador: **663 presentaciones en 4.004 s =
165.58 fps**, con vsync a 165 Hz.

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

