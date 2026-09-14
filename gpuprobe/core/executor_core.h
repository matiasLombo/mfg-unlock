// executor_core.h -- la POLITICA del ejecutor, sin Windows adentro.
//
// ENTRA: el perfil ya parseado, y los hechos que se van observando sobre cada
//   recurso (lo copiaron, esta en un heap compartido, sus viewports pasan por
//   RSSetViewports).
// SALE: decisiones -- escalar o no este recurso, saltear o no esta pasada,
//   filtrar o no este barrier -- y el calendario del harness A/B.
// DEPENDE DE: profile.h, keys.h.
//
// Esta separado de d3d12/executor.cpp a proposito: la parte que decide es la
// que puede equivocarse feo (escalar un recurso que se copia con extents fijos
// corrompe el render), asi que tiene que poder testearse exhaustivamente en el
// host. La parte de Windows solo lee archivos y llama aca.
//
// La regla de oro esta en gate(): una accion NO se aplica si el hecho que la
// haria segura no fue OBSERVADO. No se asume seguro por defecto. Eso implica
// que la primera sesion de un juego es siempre OBSERVE: primero se mira, se
// escribe lo aprendido, y recien despues se puede actuar.
#pragma once

#include "keys.h"
#include "profile.h"

#include <unordered_map>
#include <vector>

namespace gp {

// Lo observado sobre una clase de recurso (por DescKey). Todo empieza en
// "no observado", que NO es lo mismo que "no pasa".
struct Observed {
    bool seen = false;           // vimos crear al menos uno
    bool copy_source = false;    // fue origen de una copia con extents fijos
    bool copy_dest = false;
    bool placed = false;         // vive en un heap compartido con otros
    bool viewport_tracked = false;  // sus viewports pasaron por RSSetViewports
    bool reserved = false;       // recurso tiled
    u32  instances = 0;          // cuantos recursos comparten este descriptor
};

// Por que una accion no se aplico. Va al log y al overlay: un candidato que no
// se aplica sin decir por que es peor que uno que no existe.
enum class GateResult : u32 {
    Ok = 0,
    Disabled,            // el perfil la tiene apagada
    NoMatch,
    NotObserved,         // nunca vimos un recurso de esa clase: falta una sesion
    CopyObserved,        // lo copian con extents fijos
    Placed,              // comparte heap: achicarlo mueve al de al lado
    Reserved,            // tiled
    ViewportsUnknown,    // no podemos escalar sus viewports
    Ambiguous,           // varios recursos comparten el descriptor
    ObserveOnly,         // el perfil esta en modo observacion
    Backbuffer,          // es el backbuffer del swapchain: no se toca nunca
};

const char *gate_name(GateResult);

struct Decision {
    GateResult why = GateResult::NoMatch;
    u64        action_id = 0;
    double     scale = 1.0;
    i32        mip_bias = 0;
    bool       apply() const { return why == GateResult::Ok; }
};

class ExecutorCore {
public:
    void set_profile(const Profile &p);
    const Profile &profile() const { return profile_; }
    void set_output(OutputInfo o) { out_ = o; }

    // --- observacion ------------------------------------------------------
    void note_resource(DescKey, const ResourceDesc &);
    void note_copy(DescKey, bool as_source);
    void note_placed(DescKey);
    void note_reserved(DescKey);
    void note_viewport(DescKey);
    const Observed *observed(DescKey) const;
    // Para poder guardar la libreta entre sesiones. Es de solo lectura: quien
    // la escribe es note_*.
    const std::unordered_map<u64, Observed> &observations() const { return obs_; }

    // --- decisiones -------------------------------------------------------
    // Que hacer con un recurso que esta por crearse. No aplica nada: devuelve
    // la decision y el motivo, y quien llama decide que hacer con eso.
    Decision decide_resource(const ResourceDesc &, CallsiteId) const;
    Decision decide_pass(PassKey, DescKey rt) const;
    Decision decide_barrier(u32 before, u32 after) const;

    // --- harness A/B ------------------------------------------------------
    // Devuelve true si el estado de alguna accion cambio en este frame. Las
    // acciones con ab = true se prenden y apagan de a una, nunca dos a la vez:
    // con dos prendidas no se sabe cual gano.
    bool advance_frame(u64 frame);
    bool is_on(u64 action_id) const;
    // Para el overlay: prender/apagar a mano saca a la accion del ciclo A/B.
    void force(u64 action_id, bool on);
    u64  ab_current() const { return ab_current_; }

    // Contadores para el overlay y el log.
    u64 applied_count() const { return applied_; }

private:
    Profile profile_;
    OutputInfo out_{};
    std::unordered_map<u64, Observed> obs_;
    std::unordered_map<u64, bool>     state_;   // action id -> prendida
    std::unordered_map<u64, bool>     forced_;
    std::vector<u64> ab_ids_;
    size_t ab_index_ = 0;
    u64    ab_current_ = 0;
    u64    ab_last_switch_ = 0;
    u64    applied_ = 0;

    GateResult gate(const Action &, const ResourceDesc &, DescKey) const;
};

}  // namespace gp
