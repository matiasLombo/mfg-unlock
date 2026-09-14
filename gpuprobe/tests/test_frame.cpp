// test_frame.cpp -- el modelo de frame, incluido el caso que hace que un
// profiler mienta: async compute.
//
// Si la suma de las pasadas se reportara como "el costo del frame", una pasada
// de compute que corre en paralelo con el gbuffer apareceria como 3 ms de
// ganancia posible cuando la ganancia real es cero. Este test fija que la suma
// y la union son numeros distintos y que el solapamiento se ve.
#include "../core/frame.h"
#include "../core/jsonl.h"

#include "harness.h"

#include <string>
#include <vector>

using namespace gp;

static Event pass_ev(u64 frame, u64 key, u32 queue, u64 begin_ns, u64 end_ns,
                     u32 draws, u32 ordinal = 0) {
    Event e;
    e.kind = EventKind::PassTiming;
    e.frame = frame;
    e.pass.pass = PassKey{key};
    e.pass.rts = RtSetKey{key ^ 0xAAull};
    e.pass.first_pso = PsoKey{key ^ 0xBBull};
    e.pass.gpu_begin_ticks = begin_ns;
    e.pass.gpu_end_ticks = end_ns;
    e.pass.tick_freq = 1000000000ull;  // 1 GHz: ticks en ns, cuentas redondas
    e.pass.draws = draws;
    e.pass.ordinal = ordinal;
    e.pass.queue = queue;
    e.pass.rt_w = 2560;
    e.pass.rt_h = 1440;
    return e;
}

static Event frame_end(u64 frame, double cpu_ms, u32 dropped = 0) {
    Event e;
    e.kind = EventKind::FrameEnd;
    e.frame = frame;
    e.frame_ev.cpu_ns = static_cast<u64>(cpu_ms * 1e6);
    e.frame_ev.present_ns = 200000;
    e.frame_ev.dropped = dropped;
    return e;
}

int main() {
    // --- frame simple: tres pasadas en serie en la queue 0 ---------------
    {
        std::vector<Event> ev = {
            pass_ev(1, 0x100, 0, 0,       2'000'000, 800),   // 2.0 ms
            pass_ev(1, 0x200, 0, 2'000'000, 3'500'000, 40),  // 1.5 ms
            pass_ev(1, 0x300, 0, 3'500'000, 4'000'000, 1),   // 0.5 ms
            frame_end(1, 8.3),
        };
        const FrameModel f = build_frame(ev.data(), ev.size(), 1);
        CHECK(f.complete);
        CHECK(f.usable());
        CHECK_NEAR(f.cpu_ms, 8.3, 1e-3);
        CHECK_EQ_U64(f.passes.size(), 3);
        CHECK_NEAR(f.gpu_sum_ms, 4.0, 1e-6);
        CHECK_NEAR(f.gpu_busy_ms, 4.0, 1e-6);
        CHECK_NEAR(f.overlap_ms, 0.0, 1e-6);
        CHECK(!f.overlapped());
    }

    // --- async compute: la suma miente, la union no ----------------------
    {
        std::vector<Event> ev = {
            pass_ev(2, 0x100, 0, 0, 4'000'000, 900),          // gbuffer, 4 ms
            pass_ev(2, 0x900, 1, 500'000, 3'500'000, 1),      // compute, 3 ms
            frame_end(2, 9.0),
        };
        const FrameModel f = build_frame(ev.data(), ev.size(), 2);
        CHECK_NEAR(f.gpu_sum_ms, 7.0, 1e-6);
        // La queue 1 corre entera adentro de la 0: la GPU estuvo ocupada 4 ms.
        CHECK_NEAR(f.gpu_busy_ms, 4.0, 1e-6);
        CHECK_NEAR(f.overlap_ms, 3.0, 1e-6);
        CHECK(f.overlapped());
    }

    // --- pasadas que se tocan en la misma queue se unen ------------------
    {
        std::vector<PassStat> ps(2);
        ps[0].timed = true; ps[0].begin_ms = 0.0; ps[0].end_ms = 2.0; ps[0].queue = 0;
        ps[1].timed = true; ps[1].begin_ms = 1.0; ps[1].end_ms = 3.0; ps[1].queue = 0;
        CHECK_NEAR(union_ms(ps), 3.0, 1e-9);
        // Y un hueco entre pasadas NO se cuenta como ocupado.
        ps[1].begin_ms = 5.0; ps[1].end_ms = 6.0;
        CHECK_NEAR(union_ms(ps), 3.0, 1e-9);
    }

    // --- draws sin timing: la pasada existe igual ------------------------
    {
        std::vector<Event> ev;
        for (int i = 0; i < 7; ++i) {
            Event d;
            d.kind = EventKind::Draw;
            d.frame = 3;
            d.draw.pass = PassKey{0x777};
            d.draw.pso = PsoKey{0x1};
            ev.push_back(d);
        }
        ev.push_back(frame_end(3, 5.0));
        const FrameModel f = build_frame(ev.data(), ev.size(), 3);
        CHECK_EQ_U64(f.passes.size(), 1);
        CHECK_EQ_U64(f.passes[0].draws, 7);
        CHECK(!f.passes[0].timed);
        CHECK_NEAR(f.gpu_sum_ms, 0.0, 1e-9);
    }

    // --- un frame con eventos perdidos no es comparable -------------------
    {
        std::vector<Event> ev = {pass_ev(4, 1, 0, 0, 1'000'000, 10), frame_end(4, 6.0, 12)};
        const FrameModel f = build_frame(ev.data(), ev.size(), 4);
        CHECK(f.complete);
        CHECK(!f.usable());
        CHECK_EQ_U64(f.dropped, 12);
    }

    // --- VRAM por encima del budget --------------------------------------
    {
        Event v;
        v.kind = EventKind::Vram;
        v.frame = 5;
        v.vram.budget = 11ull << 30;
        v.vram.current_usage = 12ull << 30;
        v.vram.committed = 13ull << 30;
        std::vector<Event> ev = {v, frame_end(5, 30.0)};
        const FrameModel f = build_frame(ev.data(), ev.size(), 5);
        CHECK(f.vram.valid);
        CHECK(f.vram.over_budget());
        CHECK(f.vram.pressure() > 1.0);
    }

    // --- PSO compilado en gameplay ---------------------------------------
    {
        Event p;
        p.kind = EventKind::Pso;
        p.frame = 6;
        p.flags = kEvRenderThread;
        p.pso.key = PsoKey{0xABC};
        p.pso.compile_ns = 14'000'000;  // 14 ms: un hitch de libro
        std::vector<Event> ev = {p, frame_end(6, 22.0)};
        const FrameModel f = build_frame(ev.data(), ev.size(), 6);
        CHECK_EQ_U64(f.psos_compiled, 1);
        CHECK_NEAR(f.pso_compile_ms, 14.0, 1e-3);
    }

    // --- ida y vuelta por JSONL ------------------------------------------
    // El formato es el contrato con el analizador: lo que el modelo ve despues
    // de pasar por texto tiene que ser lo mismo que antes.
    {
        const OutputInfo out{2560, 1440};
        std::vector<Event> ev = {
            pass_ev(7, 0x1234, 0, 1'000'000, 3'500'000, 142, 3),
            frame_end(7, 7.7),
        };
        std::vector<Event> back;
        for (const Event &e : ev) {
            char line[kMaxLine];
            const size_t n = format_event(line, sizeof line, e, out);
            CHECK(n > 0);
            ParsedLine pl;
            CHECK(parse_line(line, n, pl));
            Event r;
            CHECK(event_from_line(pl, r));
            back.push_back(r);
        }
        const FrameModel f = build_frame(back.data(), back.size(), 7);
        CHECK_EQ_U64(f.passes.size(), 1);
        CHECK_NEAR(f.passes[0].gpu_ms, 2.5, 1e-4);
        CHECK_EQ_U64(f.passes[0].draws, 142);
        CHECK_EQ_U64(f.passes[0].ordinal, 3);
        CHECK_NEAR(f.cpu_ms, 7.7, 1e-3);
        CHECK(f.complete);
    }

    // --- una linea cortada por un crash no tira el parser -----------------
    {
        const char bad[] = "{\"t\":\"pass\",\"f\":9,\"gpu_ms\":1.2";
        ParsedLine pl;
        CHECK(!parse_line(bad, sizeof bad - 1, pl));
        const char empty[] = "";
        CHECK(!parse_line(empty, 0, pl));
        const char garbage[] = "no soy json";
        CHECK(!parse_line(garbage, sizeof garbage - 1, pl));
    }

    // --- build_frames parte por indice de frame ---------------------------
    {
        std::vector<Event> ev = {
            pass_ev(10, 1, 0, 0, 1'000'000, 5), frame_end(10, 5.0),
            pass_ev(11, 1, 0, 0, 2'000'000, 5), frame_end(11, 6.0),
        };
        const std::vector<FrameModel> fs = build_frames(ev.data(), ev.size());
        CHECK_EQ_U64(fs.size(), 2);
        CHECK_EQ_U64(fs[0].index, 10);
        CHECK_NEAR(fs[1].gpu_sum_ms, 2.0, 1e-6);
    }

    return gp_test::report("test_frame");
}
