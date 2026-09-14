# Arquitectura de gpuprobe

## El principio rector

**Todo lo que pueda ser puro, es puro.** La capa que toca D3D12 no analiza
nada: emite eventos y aplica acciones ya decididas. El modelo de frame, los
matchers del perfil, el parser de TOML, los gates del ejecutor y la estadistica
compilan con `g++` en Linux y corren en CI sin GPU.

No es una preferencia de estilo. Es lo que hace que el proyecto sea
verificable: si el modelo de frame viviera adentro del hook, la unica forma de
probarlo seria lanzar un juego.

## Las capas

```
L0  proxy/      version.dll: forwarding de exports, gate, descubrimiento
L1  d3d12/      enganche por vtable. No decide nada
L2  d3d12/      colector: recursos, PSOs, pasadas, timestamps -> ring -> JSONL
L3  core/       modelo de frame PURO: pasadas, solapamiento, VRAM
L4  core/+d3d12 ejecutor: politica pura + envoltorio de Windows
L5  d3d12/      overlay ImGui (opcional)
L6  analyzer/   analizador y harness A/B en Python
```

Regla de dependencias: **`core/` no incluye ni un header de D3D12.** `d3d12/`
conoce `core/`; `core/` no conoce a nadie. El analizador solo conoce el formato
JSONL.

## Las interfaces que importan

Entre la capa que toca la API y todo lo demas hay exactamente dos superficies.

**Hacia afuera, eventos** (`core/events.h`): un POD de 104 bytes por evento,
sin punteros a memoria que pueda morir. Esa es toda la salida del colector.

**Hacia adentro, acciones** (`d3d12/collector.h`):

```cpp
struct Actions {
    ResourceOverride on_create(const ResourceDesc &, CallsiteId);
    void note_resource(DescKey, const ResourceDesc &, bool placed, bool reserved);
    void note_copy(DescKey, bool as_source);
    void note_viewport(DescKey);
    bool skip_pass(PassKey, DescKey rt);
    bool drop_barrier(u32 before, u32 after);
    void begin_frame(u64 index);
    const char *capture_due(u64 index);
};
```

Un `Actions` que devuelve todo neutro es passthrough exacto, y es el default.
Las `note_*` no son decorativas: son los hechos que despues habilitan (o no)
una accion. Un ejecutor que no se entera de que a un recurso lo copian con
extents fijos es un ejecutor que lo va a escalar.

**Para D3D11 (prioridad 2)** no hace falta tocar nada de esto: alcanza con una
capa `d3d11/` que emita los mismos eventos y consulte las mismas acciones. Los
eventos no mencionan D3D12 en ningun lado; los conceptos (recurso, PSO,
pasada, barrier) existen en las dos APIs o se pueden derivar.

## Identidad: tres claves y por que son tres

| clave | de que sale | para que |
| --- | --- | --- |
| `ResKey` | el puntero al recurso | seguir una instancia viva |
| `DescKey` | el descriptor normalizado | **matchear en el perfil** |
| `PsoKey` | el bytecode de cada etapa | agrupar y comparar sesiones |

`DescKey` es la decision de fondo del proyecto. Se normaliza segun si el
recurso escala con la salida: un RT full-res da la misma clave a 1440p, a 2160p
y con DRS; un shadow map de 4096 entra con sus numeros reales porque no escala
con nada. Normalizar un shadow map contra la salida daria dos claves distintas
para el mismo recurso.

La contracara, que esta escrita en el reporte y no escondida: `DescKey`
identifica una **clase**. Tres RTs RGBA16F full-res comparten clave, asi que
toda accion cuyo matcher sea ambiguo lo dice.

`PsoKey` sale del bytecode y no del puntero, asi que es estable entre corridas
-- pero se usa para **medir**, no para actuar: un parche del juego lo mueve.

## El hot path

El hilo de render **no hashea, no aloca, no escribe a disco y no toma locks**.
Copia un POD a un ring SPSC y sigue. Todo lo caro pasa en el hilo que drena.

- Un ring por hilo que graba (los juegos graban en 8 o 16).
- Cuando el ring se llena, **descarta y cuenta**. Nunca frena al juego. Los
  descartes salen en el JSONL: un frame con eventos perdidos no se puede
  comparar con uno completo, y el analizador lo excluye.
- La tabla de vistas (handle de RTV -> recurso) es open addressing con
  publicacion release y lectura acquire.
- El estado por command list se cachea en `thread_local`; el mutex solo se toca
  en `Reset` y `Close`.
- El bytecode de un PSO es la unica excepcion: los punteros del descriptor solo
  valen durante la llamada, asi que se copia ahi. Un memcpy al lado de un
  `CreateGraphicsPipelineState` -- que ya cuesta milisegundos -- no mueve la
  aguja.

## Timing sin stalls

Un `ID3D12QueryHeap` de timestamps por slot, cuatro slots en vuelo. Cada
command list reserva bloques de queries, marca el principio y el fin de cada
pasada, y resuelve a un readback en su `Close`. Los ticks se cobran **tres
frames despues**, cuando la fence de esa ejecucion ya paso. Nunca se espera.

El presupuesto de queries es fijo por slot: cuando se agota se deja de medir y
se cuenta. Nunca se crece un heap adentro de un frame.

La unica excepcion en todo gpuprobe es la **captura PNG**, que copia el
backbuffer y espera la fence. Pasa dos veces por candidato del harness, no por
frame, y el frame en el que pasa no se usa como muestra.

## Los tres riesgos que se identificaron al principio, y donde quedaron

### 1. `resource_scale` rompe el render, y el modo de romperse es silencioso

Bajar un RT de 2048 a 1024 no es cambiar un numero: hay viewports con
constantes, SRVs con mips fijos, copias con extents literales y aliasing en
heaps placed.

Quedo en tres escalones:

- **Gate de elegibilidad** (`core/executor_core.h`): una accion NO se aplica si
  el hecho que la haria segura no fue OBSERVADO. Eso implica que la primera
  sesion de un juego es siempre observacion. Los motivos de rechazo estan
  enumerados y se escriben en el log.
- **Viewports**: `RSSetViewports` se intercepta y se escala junto con el
  recurso. Si los viewports de un recurso no pasan por ahi, el gate lo rechaza.
- **El backbuffer no se toca nunca**, ni con `verify = "none"`.

Y el testbed trae los dos casos hostiles a proposito -- un RT que ademas se
copia, y uno colocado en un heap compartido -- para que los gates se prueben
rechazando, no solo aceptando.

### 2. Convivencia con otras capas y con la grabacion multihilo

En una maquina real ya hay enganchados Streamline, el overlay de GeForce,
Steam, a veces ReShade. Y los juegos graban en muchos hilos.

- **Una tabla de vtables, no "la" vtable.** Es la leccion cara de este
  repositorio (`docs/present-por-vtable.diff`): un juego puede tener mas de una
  vtable viva para la misma interfaz, y guardar "el original" en una global
  hace que la segunda llame al original de la primera. Cada metodo guarda su
  original por vtable, y si no lo encuentra se apaga en vez de saltar a ciegas.
- **Guarda de reentrada por hilo**: si un hook nuestro termina llamando a algo
  que vuelve a entrar, la segunda vuelta pasa derecho.
- **Sin locks en el hot path** (arriba).
- **Un solo camino de degradado**, y es definitivo: reintentar algo que ya
  fallo una vez adentro de un juego es como se fabrica un crash intermitente.
- **Un testigo de excepciones** (VEH) que mira si la falla cayo dentro de
  nuestro modulo: si fue nuestra, lo deja escrito y apaga gpuprobe; si no, no
  toca nada y devuelve `CONTINUE_SEARCH`.

### 3. La medicion miente

- El instrumentado cambia lo que mide: por eso hay **dos niveles**, uno liviano
  con presupuesto fijo de queries y uno completo cada N frames, marcados
  distinto en el JSONL para que el analizador no los mezcle.
- Con async compute la suma de las pasadas no es el frame: el modelo **no arma
  un arbol que sume**. Reporta la suma, la union de intervalos y el
  solapamiento por separado, y la ganancia de un candidato se limita con su
  tiempo exclusivo.
- Los clocks de la GPU derivan: por eso la cifra final sale del harness A/B
  alternando ON/OFF cada pocos frames, y no de comparar dos corridas.
- El techo de VRAM se evalua **antes** que todo lo demas.

## Lo que el testbed sobre WARP encontro

Cuatro cosas que los tests unitarios daban por verdes y estaban rotas contra un
device real. Es el argumento entero a favor de tener un testbed propio:

1. Las pasadas de compute no se median: el corte entre pasadas es el cambio de
   render targets, y una command list de COMPUTE nunca llama
   `OMSetRenderTargets`.
2. El recurso huerfano no se detectaba: el analizador daba por escrito solo lo
   que veia transicionar por un barrier, y un RT creado ya en `RENDER_TARGET`
   no emite ninguno.
3. El PSO compilado en gameplay se perdia por un warmup que se aplicaba dos
   veces.
4. El **backbuffer** salia propuesto como recurso huerfano -- un falso positivo
   que en un juego real aparece siempre.
