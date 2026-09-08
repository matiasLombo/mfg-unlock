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

## 6. Que camino sirve

**Sirve, y es el mas barato: arreglar la obtencion del swapchain (opcion d).**
Ya estamos dentro del proceso. No hacen falta ETW ni privilegios para contar
presentaciones: hace falta el swapchain que el juego usa de verdad. Es un
problema acotado y con una prueba clara, que `dp > 0` en Halo.

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
