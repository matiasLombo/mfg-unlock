// frame.cpp -- ver frame.h.
#include "frame.h"

#include "jsonl.h"

#include <algorithm>
#include <map>

namespace gp {

double union_ms(std::vector<PassStat> passes) {
    // Union por queue: los ticks de dos queues distintas no son comparables
    // salvo que el colector los haya normalizado a un reloj comun (lo hace con
    // GetClockCalibration cuando el driver lo soporta). Si no lo estan, unir
    // entre queues daria un numero inventado; unir por queue y quedarse con el
    // maximo es conservador y no miente.
    std::map<u32, std::vector<const PassStat *>> by_queue;
    for (const PassStat &p : passes)
        if (p.timed) by_queue[p.queue].push_back(&p);

    double best = 0.0;
    for (auto &kv : by_queue) {
        std::vector<const PassStat *> &v = kv.second;
        std::sort(v.begin(), v.end(), [](const PassStat *a, const PassStat *b) {
            return a->begin_ms < b->begin_ms;
        });
        double total = 0.0, cur_b = 0.0, cur_e = 0.0;
        bool open = false;
        for (const PassStat *p : v) {
            if (!open) { cur_b = p->begin_ms; cur_e = p->end_ms; open = true; continue; }
            if (p->begin_ms <= cur_e) {
                if (p->end_ms > cur_e) cur_e = p->end_ms;
            } else {
                total += cur_e - cur_b;
                cur_b = p->begin_ms;
                cur_e = p->end_ms;
            }
        }
        if (open) total += cur_e - cur_b;
        best = std::max(best, total);
    }
    return best;
}

FrameModel build_frame(const Event *events, size_t n, u64 frame_index) {
    FrameModel f;
    f.index = frame_index;

    // Primer tick visto por queue: las pasadas se posicionan relativas a eso,
    // porque el tick absoluto de la GPU no significa nada fuera de la corrida.
    std::map<u32, u64> first_tick;
    for (size_t i = 0; i < n; ++i) {
        const Event &e = events[i];
        if (e.frame != frame_index) continue;
        if (e.kind == EventKind::PassTiming && e.pass.tick_freq) {
            auto it = first_tick.find(e.pass.queue);
            if (it == first_tick.end() || e.pass.gpu_begin_ticks < it->second)
                first_tick[e.pass.queue] = e.pass.gpu_begin_ticks;
        }
    }

    // Draws sin timing: se cuentan igual, porque "esta pasada existe y tiene
    // 900 draws" ya es un dato aunque no sepamos cuanto costo.
    std::map<u64, size_t> pass_index;

    for (size_t i = 0; i < n; ++i) {
        const Event &e = events[i];
        if (e.frame != frame_index) continue;
        if (e.flags & kEvDeepFrame) f.deep = true;

        switch (e.kind) {
            case EventKind::FrameEnd:
                f.cpu_ms = static_cast<double>(e.frame_ev.cpu_ns) / 1e6;
                f.present_ms = static_cast<double>(e.frame_ev.present_ns) / 1e6;
                f.dropped += e.frame_ev.dropped;
                f.complete = true;
                break;

            case EventKind::PassTiming: {
                const u64 base = first_tick.count(e.pass.queue)
                                     ? first_tick[e.pass.queue] : e.pass.gpu_begin_ticks;
                PassStat p;
                p.key = e.pass.pass;
                p.rts = e.pass.rts;
                p.first_pso = e.pass.first_pso;
                p.gpu_ms = ticks_to_ms(e.pass.gpu_begin_ticks, e.pass.gpu_end_ticks,
                                       e.pass.tick_freq);
                p.begin_ms = ticks_to_ms(base, e.pass.gpu_begin_ticks, e.pass.tick_freq);
                p.end_ms = p.begin_ms + p.gpu_ms;
                p.draws = e.pass.draws;
                p.ordinal = e.pass.ordinal;
                p.queue = e.pass.queue;
                p.rt_w = e.pass.rt_w;
                p.rt_h = e.pass.rt_h;
                p.rt_bytes = e.pass.rt_bytes;
                p.timed = e.pass.tick_freq != 0;

                auto it = pass_index.find(p.key.v);
                if (it == pass_index.end()) {
                    pass_index[p.key.v] = f.passes.size();
                    f.passes.push_back(p);
                } else {
                    // Un PassTiming para una pasada que ya vimos por sus draws:
                    // completa el timing en vez de duplicar la fila.
                    PassStat &dst = f.passes[it->second];
                    const u32 draws = dst.draws ? dst.draws : p.draws;
                    dst = p;
                    dst.draws = draws;
                }
                break;
            }

            case EventKind::Draw:
            case EventKind::Dispatch: {
                auto it = pass_index.find(e.draw.pass.v);
                if (it == pass_index.end()) {
                    PassStat p;
                    p.key = e.draw.pass;
                    p.first_pso = e.draw.pso;
                    p.draws = 1;
                    pass_index[p.key.v] = f.passes.size();
                    f.passes.push_back(p);
                } else {
                    ++f.passes[it->second].draws;
                }
                break;
            }

            case EventKind::Pso:
                // Un PSO creado DENTRO de un frame de gameplay es una compilacion
                // en caliente. El colector marca si paso en el hilo de render;
                // el analizador usa las dos cosas.
                ++f.psos_compiled;
                f.pso_compile_ms += static_cast<double>(e.pso.compile_ns) / 1e6;
                break;

            case EventKind::Vram:
                f.vram.budget = e.vram.budget;
                f.vram.usage = e.vram.current_usage;
                f.vram.committed = e.vram.committed;
                f.vram.valid = true;
                break;

            default:
                break;
        }
    }

    for (const PassStat &p : f.passes) f.gpu_sum_ms += p.gpu_ms;
    f.gpu_busy_ms = union_ms(f.passes);
    f.overlap_ms = f.gpu_sum_ms - f.gpu_busy_ms;
    if (f.overlap_ms < 0.0) f.overlap_ms = 0.0;
    return f;
}

std::vector<FrameModel> build_frames(const Event *events, size_t n) {
    std::vector<u64> order;
    std::map<u64, bool> seen;
    for (size_t i = 0; i < n; ++i) {
        const u64 idx = events[i].frame;
        if (!seen.count(idx)) { seen[idx] = true; order.push_back(idx); }
    }
    std::vector<FrameModel> out;
    out.reserve(order.size());
    for (u64 idx : order) out.push_back(build_frame(events, n, idx));
    return out;
}

}  // namespace gp
