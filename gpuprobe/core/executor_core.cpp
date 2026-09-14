// executor_core.cpp -- ver executor_core.h.
#include "executor_core.h"

namespace gp {

const char *gate_name(GateResult g) {
    switch (g) {
        case GateResult::Ok:               return "ok";
        case GateResult::Disabled:         return "apagada en el perfil";
        case GateResult::NoMatch:          return "no matchea";
        case GateResult::NotObserved:      return "nunca vimos un recurso de esa clase";
        case GateResult::CopyObserved:     return "lo copian con extents fijos";
        case GateResult::Placed:           return "comparte heap con otro recurso";
        case GateResult::Reserved:         return "recurso tiled";
        case GateResult::ViewportsUnknown: return "no podemos escalar sus viewports";
        case GateResult::Ambiguous:        return "varios recursos comparten el descriptor";
        case GateResult::ObserveOnly:      return "el perfil esta en modo observacion";
        case GateResult::Backbuffer:       return "es el backbuffer: no se toca";
    }
    return "?";
}

void ExecutorCore::set_profile(const Profile &p) {
    profile_ = p;
    ab_ids_.clear();
    for (const Action &a : profile_.actions) {
        if (a.ab) ab_ids_.push_back(a.id);
        // Las acciones que el usuario toco a mano en el overlay mantienen su
        // estado; el resto arranca como diga el perfil.
        if (forced_.find(a.id) == forced_.end()) state_[a.id] = a.enabled;
    }
    ab_index_ = 0;
    ab_current_ = ab_ids_.empty() ? 0 : ab_ids_[0];
}

void ExecutorCore::note_resource(DescKey k, const ResourceDesc &d) {
    (void)d;
    Observed &o = obs_[k.v];
    o.seen = true;
    ++o.instances;
}

void ExecutorCore::note_copy(DescKey k, bool as_source) {
    Observed &o = obs_[k.v];
    if (as_source) o.copy_source = true;
    else o.copy_dest = true;
}

void ExecutorCore::note_placed(DescKey k)   { obs_[k.v].placed = true; }
void ExecutorCore::note_reserved(DescKey k) { obs_[k.v].reserved = true; }
void ExecutorCore::note_viewport(DescKey k) { obs_[k.v].viewport_tracked = true; }

const Observed *ExecutorCore::observed(DescKey k) const {
    auto it = obs_.find(k.v);
    return it == obs_.end() ? nullptr : &it->second;
}

GateResult ExecutorCore::gate(const Action &a, const ResourceDesc &d,
                              DescKey key) const {
    // El backbuffer no se escala ni con verify = "none": no es una
    // optimizacion, es romper la presentacion. Esta antes que el gate del
    // usuario a proposito -- no hay perfil que lo habilite.
    if (d.swapchain && (a.kind == ActionKind::ResourceScale ||
                        a.kind == ActionKind::MipBias))
        return GateResult::Backbuffer;
    if (a.verify == Verify::None) return GateResult::Ok;

    const Observed *o = observed(key);
    // Nunca vimos un recurso de esta clase: no hay nada que garantice que sea
    // seguro escalarlo. La primera sesion de un juego es siempre OBSERVE.
    if (!o || !o->seen) return GateResult::NotObserved;

    const bool need_copy = a.verify == Verify::NoCopyObserved || a.verify == Verify::Full;
    const bool need_placed = a.verify == Verify::NotPlaced || a.verify == Verify::Full;
    const bool need_vp = a.verify == Verify::ViewportsTracked || a.verify == Verify::Full;

    if (need_copy && (o->copy_source || o->copy_dest)) return GateResult::CopyObserved;
    if (need_placed && o->placed) return GateResult::Placed;
    if (need_placed && o->reserved) return GateResult::Reserved;
    if (need_vp && !o->viewport_tracked) return GateResult::ViewportsUnknown;
    // Varios recursos con el mismo descriptor: la accion los tocaria a todos.
    // Para un scale eso no es necesariamente malo (si son todos SSR), pero no
    // lo podemos saber, asi que con verify = full no se aplica.
    if (a.verify == Verify::Full && o->instances > 1 &&
        a.kind == ActionKind::SkipPass)
        return GateResult::Ambiguous;
    (void)d;
    return GateResult::Ok;
}

Decision ExecutorCore::decide_resource(const ResourceDesc &d, CallsiteId site,
                                       ActionKind want) const {
    (void)site;
    Decision dec;
    const DescKey key = desc_key(d, out_);
    for (const Action &a : profile_.actions) {
        if (a.kind != ActionKind::ResourceScale && a.kind != ActionKind::MipBias)
            continue;
        if (want != ActionKind::None && a.kind != want) continue;
        if (!matches(a.match, d, out_)) continue;
        dec.action_id = a.id;
        if (profile_.observe_only) { dec.why = GateResult::ObserveOnly; return dec; }
        if (!is_on(a.id))          { dec.why = GateResult::Disabled; return dec; }
        const GateResult g = gate(a, d, key);
        if (g != GateResult::Ok)   { dec.why = g; return dec; }
        dec.why = GateResult::Ok;
        if (a.kind == ActionKind::ResourceScale) dec.scale = a.scale;
        else                                     dec.mip_bias = a.mip_bias;
        return dec;
    }
    return dec;
}

Decision ExecutorCore::decide_pass(PassKey pass, DescKey rt) const {
    (void)pass;
    Decision dec;
    for (const Action &a : profile_.actions) {
        if (a.kind != ActionKind::SkipPass) continue;
        // Una pasada se matchea por el DESCRIPTOR de su RT principal. Si el
        // perfil trae una dkey explicita, tiene que ser esa.
        if (a.match.dkey.v && a.match.dkey != rt) continue;
        dec.action_id = a.id;
        if (profile_.observe_only) { dec.why = GateResult::ObserveOnly; return dec; }
        if (!is_on(a.id))          { dec.why = GateResult::Disabled; return dec; }
        const Observed *o = observed(rt);
        if (a.verify != Verify::None && (!o || !o->seen)) {
            dec.why = GateResult::NotObserved;
            return dec;
        }
        if (a.verify == Verify::Full && o && o->instances > 1) {
            dec.why = GateResult::Ambiguous;
            return dec;
        }
        dec.why = GateResult::Ok;
        return dec;
    }
    return dec;
}

Decision ExecutorCore::decide_barrier(u32 before, u32 after) const {
    Decision dec;
    for (const Action &a : profile_.actions) {
        if (a.kind != ActionKind::BarrierFilter) continue;
        if (a.match.has_from && a.match.from_state != before) continue;
        if (a.match.has_to && a.match.to_state != after) continue;
        dec.action_id = a.id;
        if (profile_.observe_only) { dec.why = GateResult::ObserveOnly; return dec; }
        if (!is_on(a.id))          { dec.why = GateResult::Disabled; return dec; }
        dec.why = GateResult::Ok;
        return dec;
    }
    return dec;
}

bool ExecutorCore::is_on(u64 id) const {
    auto f = forced_.find(id);
    if (f != forced_.end()) return f->second;
    auto it = state_.find(id);
    return it != state_.end() && it->second;
}

void ExecutorCore::force(u64 id, bool on) {
    forced_[id] = on;
    state_[id] = on;
}

bool ExecutorCore::advance_frame(u64 frame) {
    if (ab_ids_.empty() || profile_.ab_period == 0) return false;
    if (frame < ab_last_switch_ + profile_.ab_period) return false;
    ab_last_switch_ = frame;

    // El ciclo: para cada accion del harness, un periodo prendida y uno
    // apagada, y recien ahi pasa a la siguiente. Nunca hay dos prendidas: con
    // dos prendidas no se sabe cual gano, y el reporte no serviria para
    // decidir.
    const u64 id = ab_ids_[ab_index_];
    const bool was_on = is_on(id);
    if (was_on) {
        state_[id] = false;
        forced_.erase(id);
        ab_index_ = (ab_index_ + 1) % ab_ids_.size();
        ab_current_ = ab_ids_[ab_index_];
        state_[ab_current_] = true;
        forced_.erase(ab_current_);
    } else {
        for (u64 other : ab_ids_) if (other != id) state_[other] = false;
        state_[id] = true;
        forced_.erase(id);
        ab_current_ = id;
    }
    ++applied_;
    return true;
}

}  // namespace gp
