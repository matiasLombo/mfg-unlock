// frame.h -- el modelo de frame: de una lista de eventos a "que costo cuanto".
//
// ENTRA: los eventos de un frame (ya drenados y ordenados por llegada).
// SALE: un FrameModel con las pasadas, sus ms de GPU, el solapamiento, y el
//   estado de VRAM.
// DEPENDE DE: events.h, keys.h. NADA de D3D12: por eso se puede testear en CI
//   sin GPU, que es la unica razon por la que este modulo existe separado del
//   colector.
//
// Dos decisiones que valen mas que el codigo:
//
// 1. NO se arma un arbol que sume al frame. Con async compute la suma de las
//    pasadas es mayor que el frame, y un arbol que "cierra" solo cierra porque
//    alguien escondio el solapamiento. Aca la suma y la union se reportan por
//    separado, y overlap_ms es la diferencia. Una pasada que solapa con otra
//    puede costar 3 ms y no valer nada optimizarla: eso tiene que verse.
//
// 2. El techo de VRAM se evalua ANTES que nada. Si el juego esta pidiendo mas
//    memoria de la que el adaptador le da, los ms por pasada son consecuencia
//    del thrashing y optimizar una pasada es perder el tiempo. El modelo marca
//    la sesion y el analizador corta ahi.
#pragma once

#include "events.h"
#include "keys.h"

#include <vector>

namespace gp {

struct PassStat {
    PassKey  key;
    RtSetKey rts;
    PsoKey   first_pso;
    double   gpu_ms = 0.0;
    double   begin_ms = 0.0;   // relativo al primer tick del frame, por queue
    double   end_ms = 0.0;
    u32      draws = 0;
    u32      ordinal = 0;
    u32      queue = 0;
    u32      rt_w = 0, rt_h = 0;
    u64      rt_bytes = 0;
    bool     timed = false;    // false: contamos draws pero no hubo query
};

struct VramState {
    u64  budget = 0;
    u64  usage = 0;
    u64  committed = 0;
    bool valid = false;
    // El juego pidio mas de lo que el adaptador le presupuesta. A partir de
    // aca el driver empieza a paginar por PCIe y TODO mide mal.
    bool over_budget() const { return valid && budget > 0 && usage > budget; }
    double pressure() const {
        return (valid && budget > 0) ? static_cast<double>(usage) /
                                       static_cast<double>(budget) : 0.0;
    }
};

struct FrameModel {
    u64    index = 0;
    double cpu_ms = 0.0;
    double present_ms = 0.0;
    double gpu_sum_ms = 0.0;    // suma de las pasadas: cuenta el solapamiento dos veces
    double gpu_busy_ms = 0.0;   // union de los intervalos: lo que la GPU estuvo ocupada
    double overlap_ms = 0.0;    // sum - busy. > 0 significa async compute
    u32    dropped = 0;         // eventos que el ring descarto en este frame
    u32    psos_compiled = 0;   // PSOs creados DURANTE el frame (stutter)
    double pso_compile_ms = 0.0;
    bool   deep = false;
    bool   complete = false;    // vimos frame_end
    std::vector<PassStat> passes;
    VramState vram;

    bool overlapped() const { return overlap_ms > 0.05; }
    // Un frame con eventos perdidos no se puede comparar contra uno completo.
    bool usable() const { return complete && dropped == 0; }
};

// Construye el modelo de UN frame. Los eventos de otros frames se ignoran
// (el llamador filtra por index, o pasa el frame entero tal como salio del
// ring; ambas cosas funcionan).
FrameModel build_frame(const Event *events, size_t n, u64 frame_index);

// Parte una corrida de eventos en frames, usando frame_end como corte.
std::vector<FrameModel> build_frames(const Event *events, size_t n);

// La union de intervalos, expuesta porque el test la mira directo: sin esto
// el solapamiento se mide "a ojo" y es justo el numero que hace que una
// optimizacion parezca valer 3 ms cuando vale cero.
double union_ms(std::vector<PassStat> passes);

}  // namespace gp
