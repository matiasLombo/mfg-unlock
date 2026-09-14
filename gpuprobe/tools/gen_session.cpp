// gen_session.cpp -- genera una sesion JSONL sintetica con los mismos patrones
// que el testbed D3D12 emite de verdad.
//
//   g++ -std=c++20 -I gpuprobe gpuprobe/tools/gen_session.cpp
//       gpuprobe/core/types.cpp gpuprobe/core/jsonl.cpp -o gen_session
//   ./gen_session /tmp/sesion.jsonl [thrash]
//
// Para que existe: el analizador tiene que poder desarrollarse y testearse sin
// GPU y sin Windows. Esta sesion la escribe el MISMO writer que usa el DLL
// (core/jsonl.cpp), asi que el contrato entre C++ y Python queda ejercitado de
// punta a punta en el CI de Linux; el testbed sobre WARP produce la sesion
// real y el analizador corre igual sobre las dos.
//
// Los defectos que mete son deliberados y son los que el analizador tiene que
// encontrar:
//   1. sombras de 4096 en array de 4, caras y sobredimensionadas
//   2. SSAO y SSR a resolucion completa
//   3. un RT creado, escrito y nunca leido
//   4. barriers redundantes (ida y vuelta a COMMON todos los frames)
//   5. un PSO compilado en el hilo de render a mitad del gameplay
//
// Con "thrash" ademas pone el uso de VRAM por encima del budget, que es el
// caso en el que el analizador tiene que dejar de proponer pasadas y decir que
// la sesion mide thrashing.
#include "core/jsonl.h"
#include "core/stats.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace gp;

namespace {

const OutputInfo kOut{2560, 1440};
constexpr u64 kFreq = 1000000000ull;  // ticks en ns: las cuentas dan redondas
constexpr u64 kFrames = 900;
constexpr u64 kDeepEvery = 120;

struct Res {
    const char  *name;
    ResourceDesc desc;
    ResKey       key;
    DescKey      dkey;
};

Res make(const char *name, u64 id, Dim dim, Format f, u32 w, u32 h, u32 depth,
         u32 mips, u32 flags, HeapKind heap = HeapKind::Default) {
    Res r;
    r.name = name;
    r.desc.dim = dim;
    r.desc.fmt = f;
    r.desc.w = w;
    r.desc.h = h;
    r.desc.depth = depth;
    r.desc.mips = mips;
    r.desc.samples = 1;
    r.desc.flags = flags;
    r.desc.heap = heap;
    const bool vol = (dim == Dim::Texture3D);
    r.desc.bytes = texture_bytes(f, w, h, vol ? depth : 1, mips, vol ? 1 : depth);
    r.key = ResKey{0x1000 + id};
    r.dkey = desc_key(r.desc, kOut);
    return r;
}

struct Pass {
    const char *name;
    int         res;        // indice en el vector de recursos: el RT principal
    double      base_ms;
    u32         draws;
    u32         queue;
};

}  // namespace

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "sesion.jsonl";
    const bool thrash = argc > 2 && std::strcmp(argv[2], "thrash") == 0;
    // Modo ab: una accion que se prende y apaga cada 60 frames y que, cuando
    // esta prendida, saca 0.9 ms de la pasada de SSR. Es lo que el harness
    // deja en el JSONL y lo que el analizador tiene que poder medir.
    const bool ab = argc > 2 && std::strcmp(argv[2], "ab") == 0;
    const u64 kAbPeriod = 60;
    const u64 kAbAction = 3;

    std::FILE *out = std::fopen(path, "wb");
    if (!out) {
        std::fprintf(stderr, "no pude abrir %s\n", path);
        return 1;
    }

    char line[kMaxLine];
    auto write_line = [&](size_t n) {
        if (n) { std::fwrite(line, 1, n, out); std::fputc('\n', out); }
    };
    auto emit = [&](const Event &e) {
        write_line(format_event(line, sizeof line, e, kOut));
    };

    const u64 budget = 11ull * 1024 * 1024 * 1024;
    write_line(format_header(line, sizeof line, "gpuprobe-testbed.exe",
                             "NVIDIA GeForce RTX 4070 Ti", kOut, budget,
                             "gpuprobe 0.1.0-fixture"));

    // --- los recursos de la escena ---------------------------------------
    std::vector<Res> res;
    res.push_back(make("shadow_atlas", 0, Dim::Texture2D, Format::D32_Float,
                       4096, 4096, 4, 1, kFlagAllowDepthStencil));
    res.push_back(make("scene_depth", 1, Dim::Texture2D, Format::D32_Float,
                       2560, 1440, 1, 1, kFlagAllowDepthStencil));
    res.push_back(make("gbuffer_albedo", 2, Dim::Texture2D, Format::R8G8B8A8_Unorm,
                       2560, 1440, 1, 1, kFlagAllowRenderTarget));
    res.push_back(make("gbuffer_normal", 3, Dim::Texture2D, Format::R16G16_Float,
                       2560, 1440, 1, 1, kFlagAllowRenderTarget));
    res.push_back(make("ssao", 4, Dim::Texture2D, Format::R8_Unorm,
                       2560, 1440, 1, 1, kFlagAllowRenderTarget));
    res.push_back(make("ssr", 5, Dim::Texture2D, Format::R16G16B16A16_Float,
                       2560, 1440, 1, 1, kFlagAllowRenderTarget));
    res.push_back(make("volumetric", 6, Dim::Texture3D, Format::R16G16B16A16_Float,
                       160, 90, 64, 1, kFlagAllowUnorderedAcc));
    res.push_back(make("bloom_half", 7, Dim::Texture2D, Format::R11G11B10_Float,
                       1280, 720, 1, 1, kFlagAllowRenderTarget));
    res.push_back(make("hdr_color", 8, Dim::Texture2D, Format::R16G16B16A16_Float,
                       2560, 1440, 1, 1, kFlagAllowRenderTarget));
    res.push_back(make("ui", 9, Dim::Texture2D, Format::R8G8B8A8_Unorm,
                       2560, 1440, 1, 1, kFlagAllowRenderTarget));
    // El huerfano: se crea, se limpia todos los frames y nunca se lee.
    res.push_back(make("orphan_rt", 10, Dim::Texture2D, Format::R16G16B16A16_Float,
                       2560, 1440, 1, 1, kFlagAllowRenderTarget));
    // Y un par de assets, para que el reporte tenga con que comparar la VRAM.
    res.push_back(make("albedo_atlas", 11, Dim::Texture2D, Format::BC7_Unorm,
                       4096, 4096, 1, 13, kFlagNone));
    res.push_back(make("normal_atlas", 12, Dim::Texture2D, Format::BC5_Unorm,
                       4096, 4096, 1, 13, kFlagNone));

    for (size_t i = 0; i < res.size(); ++i) {
        Event e;
        e.kind = EventKind::ResourceCreate;
        e.frame = 0;
        e.resource.key = res[i].key;
        e.resource.dkey = res[i].dkey;
        e.resource.site = CallsiteId{0xC0DE0000ull + i};
        e.resource.desc = res[i].desc;
        emit(e);
    }

    // --- las pasadas del frame -------------------------------------------
    // Los numeros son plausibles para una 4070 Ti a 1440p. Lo que importa no
    // es que sean exactos sino el ORDEN: las sombras y el SSR arriba, la UI
    // abajo, y una pasada de compute solapada con el gbuffer.
    const Pass passes[] = {
        {"shadows",    0, 2.40, 900, 0},
        {"gbuffer",    2, 3.10, 1400, 0},
        {"ssao",       4, 1.60, 1, 0},
        {"ssr",        5, 2.20, 1, 0},
        {"volumetric", 6, 0.80, 1, 1},   // queue 1: async compute
        {"lighting",   8, 2.90, 12, 0},
        {"bloom",      7, 0.45, 6, 0},
        {"tonemap",    8, 0.30, 1, 0},
        {"ui",         9, 0.22, 40, 0},
        {"orphan",    10, 0.05, 1, 0},
    };
    const size_t npasses = sizeof passes / sizeof passes[0];

    Rng rng(0xC0FFEE);
    auto jitter = [&](double base) {
        // +-4%: el jitter real de una GPU con clocks estables.
        const double u = static_cast<double>(rng.next() % 10000) / 10000.0;
        return base * (0.96 + 0.08 * u);
    };

    for (u64 f = 1; f <= kFrames; ++f) {
        const bool deep = (f % kDeepEvery) == 0;
        const u16 fl = deep ? kEvDeepFrame : kEvNone;

        Event fb;
        fb.kind = EventKind::FrameBegin;
        fb.frame = f;
        fb.flags = fl;
        emit(fb);

        u64 cursor[2] = {0, 300000};  // ns; la queue 1 arranca un poco despues
        double busy = 0.0;
        const bool ab_on = ab && ((f / kAbPeriod) % 2) == 1;
        if (ab && (f % kAbPeriod) == 0) {
            Event ae;
            ae.kind = EventKind::ActionApplied;
            ae.frame = f;
            ae.action.action_id = kAbAction;
            ae.action.target = res[5].dkey;
            ae.action.kind = 1;  // resource_scale
            ae.action.enabled = ((f / kAbPeriod) % 2) ? 1u : 0u;
            emit(ae);
        }

        for (size_t i = 0; i < npasses; ++i) {
            const Pass &p = passes[i];
            // Con la accion prendida, el SSR a media resolucion cuesta 0.9 ms
            // menos. Es la ganancia que el analizador tiene que recuperar.
            const double base = (ab_on && std::strcmp(p.name, "ssr") == 0)
                                    ? p.base_ms - 0.9 : p.base_ms;
            const double ms = jitter(base);
            const u64 dur = static_cast<u64>(ms * 1e6);
            const u64 begin = cursor[p.queue];
            const u64 end = begin + dur;
            cursor[p.queue] = end;
            if (p.queue == 0) busy += ms;

            Event e;
            e.kind = EventKind::PassTiming;
            e.frame = f;
            e.flags = fl;
            const PsoKey pso{0x5000 + i};
            e.pass.pass = pass_key(RtSetKey{res[p.res].dkey.v}, pso,
                                   static_cast<u32>(i));
            e.pass.rts = RtSetKey{res[p.res].dkey.v};
            e.pass.first_pso = pso;
            e.pass.gpu_begin_ticks = begin;
            e.pass.gpu_end_ticks = end;
            e.pass.tick_freq = kFreq;
            e.pass.draws = p.draws;
            e.pass.ordinal = static_cast<u32>(i);
            e.pass.queue = p.queue;
            e.pass.rt_w = res[p.res].desc.w;
            e.pass.rt_h = res[p.res].desc.h;
            e.pass.rt_bytes = res[p.res].desc.bytes;
            e.pass.rt_key = res[p.res].key;
            e.pass.rt_dkey = res[p.res].dkey;
            emit(e);

            // En los frames deep sale ademas un evento por draw. El colector
            // hace lo mismo: en light solo la pasada, en deep todo. Aca se
            // recortan a 8 por pasada para que el fixture no pese 40 MB; el
            // testbed real emite todos.
            if (deep) {
                const u32 shown = p.draws < 8 ? p.draws : 8;
                for (u32 d = 0; d < shown; ++d) {
                    Event dv;
                    dv.kind = (p.queue == 1) ? EventKind::Dispatch : EventKind::Draw;
                    dv.frame = f;
                    dv.flags = fl;
                    dv.draw.pass = e.pass.pass;
                    dv.draw.pso = pso;
                    dv.draw.count = 3 * (d + 1);
                    dv.draw.instances = 1;
                    if (p.queue == 1) { dv.draw.gx = 20; dv.draw.gy = 12; dv.draw.gz = 8; }
                    emit(dv);
                }
            }
        }

        // --- barriers ---------------------------------------------------
        // Los legitimos y los redundantes, mezclados como en un juego real.
        auto barrier = [&](const Res &r, u32 from, u32 to) {
            Event b;
            b.kind = EventKind::Barrier;
            b.frame = f;
            b.flags = fl;
            b.barrier.key = r.key;
            b.barrier.before = from;
            b.barrier.after = to;
            b.barrier.kind = 0;
            emit(b);
        };
        // Sombras: escribir, leer, volver a escribir. Necesario.
        barrier(res[0], kStateDepthWrite, kStatePixelShader);
        barrier(res[0], kStatePixelShader, kStateDepthWrite);
        // Gbuffer: escribir, leer. Necesario.
        barrier(res[2], kStateRenderTarget, kStatePixelShader);
        // Y la vuelta por COMMON que no hace falta: leer -> COMMON -> escribir,
        // cuando leer -> escribir alcanzaba. Dos barriers donde va uno, todos
        // los frames.
        barrier(res[2], kStatePixelShader, kStateCommon);
        barrier(res[2], kStateCommon, kStateRenderTarget);
        // El normal se transiciona al estado en el que YA esta.
        barrier(res[3], kStatePixelShader, kStatePixelShader);
        // El huerfano se prepara para escribir y nunca se lee.
        barrier(res[10], kStateCommon, kStateRenderTarget);

        // --- VRAM ---------------------------------------------------------
        if ((f % 30) == 0) {
            Event v;
            v.kind = EventKind::Vram;
            v.frame = f;
            v.vram.budget = budget;
            // En modo thrash el juego pide 1.4 GB mas de lo que el adaptador
            // le presupuesta: a partir de ahi todos los ms miden paginacion.
            v.vram.current_usage = thrash ? budget + (1400ull << 20)
                                          : budget - (1800ull << 20);
            v.vram.committed = v.vram.current_usage + (200ull << 20);
            v.vram.available_res = 0;
            emit(v);
        }

        // --- el PSO que compila en gameplay -------------------------------
        // Frame 600, en el hilo de render, 14 ms: el hitch clasico al entrar a
        // una zona nueva. Y otro en el 601 para que no parezca un outlier.
        if (f == 600 || f == 601) {
            Event pe;
            pe.kind = EventKind::Pso;
            pe.frame = f;
            pe.flags = kEvRenderThread;
            pe.pso.key = PsoKey{0x9000 + f};
            pe.pso.compile_ns = (f == 600) ? 14000000ull : 9000000ull;
            pe.pso.bytecode_bytes = 38000;
            pe.pso.stage_mask = 0x3;
            pe.pso.is_compute = 0;
            emit(pe);
        }

        Event fe;
        fe.kind = EventKind::FrameEnd;
        fe.frame = f;
        fe.flags = fl;
        const double hitch = (f == 600) ? 14.0 : (f == 601 ? 9.0 : 0.0);
        fe.frame_ev.cpu_ns = static_cast<u64>((busy + 1.2 + hitch) * 1e6);
        fe.frame_ev.present_ns = 250000;
        fe.frame_ev.queries_used = static_cast<u32>(npasses) * 2;
        emit(fe);
    }

    std::fclose(out);
    std::printf("%s escrito (%llu frames%s)\n", path,
                (unsigned long long)kFrames, thrash ? ", thrashing" : "");
    return 0;
}
