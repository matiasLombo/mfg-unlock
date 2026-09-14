// events.h -- el registro que cruza del hilo de render al hilo de gpuprobe.
//
// ENTRA: los hooks (capa d3d12) llaman push().
// SALE: un Event de tamano fijo, POD, sin punteros a memoria que pueda morir.
// DEPENDE DE: types.h, keys.h.
//
// Regla del hot path: el hilo de render NO hashea, NO aloca y NO escribe a
// disco. Copia un POD a un ring y sigue. Todo lo caro (hashear bytecode,
// resolver callstacks, serializar) pasa en el hilo que drena.
//
// El bytecode de un PSO es la excepcion y vale explicarla: los punteros del
// D3D12_GRAPHICS_PIPELINE_STATE_DESC solo son validos durante la llamada, asi
// que hay que copiarlo AHI. Se copia crudo a una arena y el evento lleva el
// offset; el hash lo saca el que drena. Un memcpy de unos cientos de KB al
// lado de un CreateGraphicsPipelineState -- que ya cuesta milisegundos de
// compilacion -- no mueve la aguja, y es la unica forma de tener el hash.
#pragma once

#include "keys.h"
#include "types.h"

namespace gp {

enum class EventKind : u16 {
    None = 0,
    FrameBegin,
    FrameEnd,
    ResourceCreate,
    ResourceDestroy,
    Pso,             // un PSO nuevo (con el costo de compilarlo)
    Draw,
    Dispatch,
    Barrier,
    PassBegin,
    PassEnd,
    PassTiming,      // resuelto N frames despues, con los ticks de la GPU
    Vram,
    ActionApplied,   // el ejecutor toco algo: queda en el log igual que lo medido
};

const char *event_kind_name(EventKind);

// Banderas por evento. is_deep marca los frames de instrumentacion completa:
// el analizador no puede mezclar un frame deep con uno light, porque miden
// cosas distintas.
enum EventFlags : u16 {
    kEvNone       = 0,
    kEvDeepFrame  = 0x1,
    kEvRenderThread = 0x2,  // paso en el hilo que presenta (importa en Pso)
    kEvEstimated  = 0x4,    // el numero no es medido sino derivado
};

struct EvFrame {
    u64 cpu_ns;        // duracion del frame en el CPU
    u64 present_ns;    // lo que tardo Present
    u32 dropped;       // pasadas que no pudimos medir por falta de queries
    u32 queries_used;
};

struct EvResource {
    ResKey     key;
    DescKey    dkey;
    CallsiteId site;
    ResourceDesc desc;
};

struct EvPso {
    PsoKey key;
    u64    bytecode_bytes;
    u64    compile_ns;     // lo que tardo Create*PipelineState
    u32    stage_mask;     // bit por etapa presente: VS PS DS HS GS CS
    u32    is_compute;
};

struct EvDraw {
    PassKey pass;
    PsoKey  pso;
    u32     count;      // vertices o indices
    u32     instances;
    u32     gx, gy, gz; // dispatch: tamano de grilla
};

struct EvBarrier {
    ResKey key;
    u32    before;
    u32    after;
    u32    kind;       // 0 transition, 1 aliasing, 2 uav
    u32    dropped;    // 1 si el ejecutor lo filtro
};

struct EvPass {
    PassKey  pass;
    RtSetKey rts;
    PsoKey   first_pso;
    u64      gpu_begin_ticks;
    u64      gpu_end_ticks;
    u64      tick_freq;    // por queue: ticks por segundo
    u32      draws;
    u32      ordinal;
    u32      queue;        // indice de command queue
    u32      rt_w, rt_h;   // tamano del RT principal, para la regla de escala
    u64      rt_bytes;
};

struct EvVram {
    u64 budget;
    u64 current_usage;
    u64 committed;       // suma de lo que creamos nosotros ver
    u64 available_res;   // CurrentReservation
};

struct EvAction {
    u64 action_id;
    DescKey target;
    u32 kind;
    u32 enabled;
};

struct Event {
    EventKind kind = EventKind::None;
    u16       flags = kEvNone;
    u32       thread = 0;
    u64       frame = 0;
    u64       cpu_ns = 0;   // reloj monotono del proceso al emitirlo
    union {
        EvFrame    frame_ev;
        EvResource resource;
        EvPso      pso;
        EvDraw     draw;
        EvBarrier  barrier;
        EvPass     pass;
        EvVram     vram;
        EvAction   action;
    };

    Event() : frame_ev{} {}
};

// Si este static_assert se rompe, algo crecio de mas y el ring pasa a ocupar
// el doble de cache por evento. Crecer no esta prohibido; pasar por aca si.
static_assert(sizeof(Event) <= 128, "Event crecio: revisar el ring");

}  // namespace gp
