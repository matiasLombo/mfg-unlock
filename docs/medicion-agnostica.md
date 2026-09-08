# Como miden los demas, y que nos sirve

Investigacion para el entregable I del objetivo "medir y controlar sin depender
del juego". Cada mecanismo lleva su fuente. Lo que no se pudo verificar dice
NO VERIFICADO con esas palabras.

Fecha: 2026-09-08. Esto es lectura de fuentes publicas, no medicion propia; lo
que se mida despues va aparte.

## 1. Como cuenta PresentMon las presentaciones sin inyectarse

No se inyecta. Se suscribe a eventos ETW del kernel grafico y correlaciona las
presentaciones desde afuera del proceso del juego. Consume eventos de
`Microsoft-Windows-DxgKrnl` -- entre otros `Flip::Info`, `Present::Info`,
`PresentHistory::Start`, `PresentHistory::Info`, los `QueuePacket` y
`VSyncDPC::Info` -- y los empareja por secuencia de envio y por handle de
contexto de GPU, para saber cuando una presentacion se envio y cuando se
completo.

Fuente: GameTechDev/PresentMon, `PresentData/PresentMonTraceConsumer.cpp` y
`PresentData/ETW/Microsoft_Windows_DxgKrnl.h`.

Consecuencia: el conteo de presentaciones NO necesita ver el swapchain del
juego. Es exactamente el numero que en Halo no tenemos.

## 2. Que hace falta para consumir ETW en tiempo real

Aca esta el problema. Solo pueden CONTROLAR sesiones de traza y CONSUMIR eventos
en tiempo real los procesos elevados, los usuarios del grupo "Performance Log
Users", y los servicios que corren como LocalSystem, LocalService o
NetworkService.

Fuente: Microsoft Learn, "Controlling Event Tracing Sessions" y "About Event
Tracing".

Por eso PresentMon no es una biblioteca suelta sino un SERVICIO: corre con
privilegios, agrega los datos ETW con telemetria del hardware y se los expone a
las aplicaciones cliente por una API. Desde la 2.3.1 instala `PresentMonAPI2.dll`
junto con el servicio, y los clientes hablan con el por el SDK.

Fuente: GameTechDev/PresentMon, `README-Service.md`.

**Esto choca de frente con "se entrega como un solo DLL".** Un juego corre sin
elevar, asi que un consumidor ETW dentro de nuestro `version.dll` no puede abrir
la sesion. Las salidas, ninguna gratis:

  a. Depender del servicio de PresentMon instalado y hablarle por su SDK.
     Deja de ser un solo archivo.
  b. Un servicio propio. Lo mismo, y ademas hay que mantenerlo.
  c. Pedir que el usuario este en "Performance Log Users". Paso manual, que este
     proyecto ya decidio que no cuenta como solucion.
  d. No usar ETW y arreglar la obtencion del swapchain, que es lo que realmente
     falla en Halo.

VERIFICADO, y la respuesta es NO. Una sesion privada
(`EVENT_TRACE_PRIVATE_LOGGER_MODE`) corre en modo usuario dentro del proceso, y
la sesion y los proveedores que habilita **tienen que estar todos en el mismo
proceso**. `DxgKrnl` es un proveedor de modo kernel, fuera del proceso, asi que
una sesion privada no puede consumirlo. (Hay ademas un limite de 8 loggers
privados por proceso, irrelevante aca.)

Fuente: Microsoft Learn, "Configuring and Starting a Private Logger Session".

O sea que la salida (c) del listado de arriba no existe por la via privada: sin
privilegios no hay ETW de presentaciones dentro del juego. Queda (a), (b) o (d).

## 3. Como sabe el overlay de NVIDIA el multiplicador de frame generation

Porque **esta adentro de la tuberia**, no porque lo consulte. El indicador se
enciende con una opcion de depuracion de NGX (`DLSSGIndicator`), la misma que en
dxvk-nvapi se expone como
`DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS=DLSSIndicator=1024,DLSSGIndicator=2`: dibuja
arriba para frame generation y abajo para super resolution.

Fuente: jp7677/dxvk-nvapi, documentacion de variables de entorno.

NO VERIFICADO: que exista una consulta publica de NVAPI que devuelva a un tercero
el multiplicador de frame generation en curso. La busqueda no encontro ninguna, y
dxvk-nvapi habla de soporte "limitado e incompleto" de los entrypoints de Dynamic
Multi Frame Generation. Hay que mirar los headers de NVAPI antes de cerrarlo.

Consecuencia: si no hay consulta publica, el multiplicador lo tenemos que medir
nosotros (presentadas sobre base), y eso vuelve al punto 1.

## 4. De donde sale la latencia cuando el juego no expone Reflex

De correlacionar eventos de entrada de ETW con el momento de presentacion.
PresentMon 2.2 separo dos metricas: "click-to-photon", que solo mira botones del
mouse, y "all-input-to-photon". Tambien mejoro la atribucion: la entrada que cae
en un frame descartado se asigna al siguiente frame mostrado en vez de perderse.

Fuente: notas de la version 2.2 de PresentMon y el issue 366 del repositorio.

Limite dicho por Intel: **no es click-to-photon de verdad**, porque no hay captura
externa del foton. Va desde el evento USB hasta la presentacion.

Y `MsPCLatency` esta en BETA, incluido en el esquema pero DESHABILITADO hasta que
haya soporte de eventos por debajo.

Fuente: GameTechDev/PresentMon, notas de version.

Consecuencia: sin Reflex no hay latencia de verdad, hay una aproximacion
entrada-a-presentacion que tambien necesita ETW. En Halo, donde Reflex da 0
informes en 316 ventanas, mostrar "-- ms" es lo correcto y no hay atajo.

## 5. Como conviven varios overlays

Los hooks de `Present` **se encadenan**: uno lo engancha para su menu, otro para
su limitador, cada uno llamando al anterior. Special K intercepta en su
`PresentCallback`, hace lo suyo y despacha al Present original; incluso carga el
`ReShade64.dll` de la carpeta del juego como plug-in y le arma la configuracion.
Su limitador ademas predice el sobrecosto que meten los overlays de terceros, o
sea que da por sentado que estan ahi.

Fuentes: wiki de Special K (SwapChain, ReShade) y DeepWiki de SpecialKO/SpecialK.

Consecuencia: nuestra regla de "no apilarnos sobre un Present ya detourado" es
una decision conservadora, no una imposibilidad tecnica.

Pero OJO, y esto lo midio este proyecto, no una fuente: **ese no es el motivo por
el que Halo falla**. GTA V tiene ReShade instalado y funciona; el mensaje
"already detoured" aparece en Cyberpunk, que tambien funciona. Lo que falla en
Halo es que nunca interceptamos la creacion del swapchain.

NO VERIFICADO: por que nuestra intercepcion de la creacion del swapchain no toma
en Halo. Esa carpeta tiene ReShade como `dxgi.dll`, DLSS Enabler, renodx y UE4SS
encadenados.

## 5b. Dos alternativas que no estaban en la lista

Esta seccion existe porque la primera version de este documento contestaba solo
las cinco preguntas del objetivo y saltaba a "arreglemos el swapchain", que es lo
mas barato. Buscar alternativas dio dos caminos mejores, y los dos EVITAN el
problema en vez de pelearlo.

### A. El contador de presentaciones lo da Streamline, no el swapchain

`slDLSSGGetState` devuelve un `DLSSGState` con:

    uint32_t numFramesActuallyPresented;   // desde la ultima llamada
    uint32_t numFramesToGenerateMax;       // techo del sistema

Fuente: `corpus/slheaders/sl_dlss_g_v2.12.0.h`. El layout esta confirmado porque
`numFramesToGenerateMax` (offset 52) es un campo que este proyecto YA lee bien;
`numFramesActuallyPresented` esta en el 48, justo antes.

Es API publica de Streamline: in-process, sin enganchar el swapchain, sin ETW,
sin privilegios y sin parche de bytes. Existe en todo juego que use DLSS-G, y en
el log de Halo ya aparece `state: slDLSSGGetState captured`.

Hoy lo llamamos UNA sola vez, con una guarda `g_asked_state`, y solo para leer el
techo. El contador nunca se leyo.

Dos complicaciones, dichas antes de entusiasmarse:

  - El contador se reinicia en cada llamada, asi que hay que llamarlo periodico y
    acumular.
  - El plugin exige que se llame desde el hilo de present. En Halo el log dice
    "the plugin requires the present thread for this call" y salteamos la
    llamada, porque el hilo del token no es el que configuro DLSS-G. Hay que
    resolver desde donde llamarlo.

#### Probado, y NO alcanza tal como esta: -94%

Se implemento la consulta y se midio en Cyberpunk contra la capa vieja, que ahi
si funciona. 77 pares comparables:

    capa NUEVA (slDLSSGGetState)      mediana   7.7 fps   (p10 4.1, p90 20.6)
    capa VIEJA (PresentCount runtime)  mediana 147.3 fps   (p10 134.6, p90 157.0)
    diferencia mediana -94.1%; dentro del 10%: 0 de 77

No es "cuenta solo las generadas": eso daria -25%, no -94%.

Causa que encaja con el campo: dice "frames presentados desde la ULTIMA LLAMADA a
slDLSSGGetState", y no somos el unico llamador. Cyberpunk tambien lo llama -- su
propio sl.log avisa sobre esas llamadas -- asi que cada llamada del juego
reinicia el contador y nosotros solo vemos el pedazo entre su ultima llamada y la
nuestra.

**Sin verificar todavia**: la salida seria OBSERVAR en vez de CONSULTAR --
enganchar `slDLSSGGetState` y sumar lo que el plugin le devuelve a cualquier
llamador, con lo cual ningun reinicio se pierde. No esta probado; hay que hacerlo
en una POC aislada y no dentro del dll, que es el error que produjo este -94%.

El codigo de la consulta se revirtio: no queda en el arbol.

### B. La vtable del swapchain se toma con un swapchain propio

La tecnica documentada para enganchar sin depender de la factory del juego es
crear un dispositivo y un swapchain PROPIOS, descartables, leer su vtable y
enganchar la ranura. La vtable la comparten todas las instancias creadas por la
misma implementacion de DXGI, asi que no importa quien envolvio la factory del
juego. Es lo que hace kiero, y las guias lo llaman el metodo universal
justamente porque no depende de encontrar el puntero del juego.

Fuentes: kiero (Rebzzel) y las guias del metodo de dispositivo descartable en
guidedhacking.

Contra lo que hacemos hoy: enganchamos `CreateSwapChainForHwnd` en la factory, y
si la factory del juego esta envuelta por otro overlay podemos no verla nunca.
Eso encaja con Halo, donde no hay una sola linea de swapchain en el log.

## 6. Que camino sirve

**Primero A**, el contador de Streamline. No necesita swapchain en absoluto, es
API publica, y el campo de al lado ya lo leemos bien. Si anda, resuelve el conteo
de presentaciones en cualquier juego con DLSS-G sin tocar el sistema de hooks.

**Segundo B**, tomar la vtable con un swapchain propio. Resuelve el caso de Halo
sin pelear con quien envolvio la factory, y de paso da el Present para el HUD.

**Arreglar la intercepcion actual de la factory queda TERCERO**, no primero como
decia la primera version de este documento. Es el camino que ya sabemos que falla
en presencia de otros overlays.

**Sirve para el multiplicador, si se resuelve lo anterior.** Presentadas sobre
base es medicion propia y no depende de que NVIDIA exponga nada.

**NO SIRVE, y esta cerrado: ETW dentro del DLL.** Los privilegios del punto 2 lo
impiden y la sesion privada no es una salida, porque solo alcanza proveedores del
mismo proceso. No hay que volver a intentarlo.

**No sirve: esperar una consulta de NVAPI** que nos diga el multiplicador. No se
encontro, y depender de ella nos ata otra vez a NVIDIA.

**Queda abierto: la latencia sin Reflex.** Todos los caminos pasan por ETW, asi
que hereda el problema de privilegios. Mostrar "-- ms" es la respuesta honesta
mientras tanto.
