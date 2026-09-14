// jsonl.cpp -- serializacion y parseo del formato de sesion. Ver jsonl.h.
#include "jsonl.h"

namespace gp {

namespace {

const char *heap_name(HeapKind h) {
    switch (h) {
        case HeapKind::Default:  return "default";
        case HeapKind::Upload:   return "upload";
        case HeapKind::Readback: return "readback";
        case HeapKind::Custom:   return "custom";
        case HeapKind::Placed:   return "placed";
        case HeapKind::Reserved: return "reserved";
        case HeapKind::Unknown:  break;
    }
    return "unknown";
}

const char *dim_name(Dim d) {
    switch (d) {
        case Dim::Buffer:    return "buffer";
        case Dim::Texture1D: return "tex1d";
        case Dim::Texture2D: return "tex2d";
        case Dim::Texture3D: return "tex3d";
        case Dim::Unknown:   break;
    }
    return "unknown";
}

// Escapa lo justo: los strings que emitimos son nombres de exe, de adaptador y
// de categoria. Comillas y backslash son lo unico que aparece en la practica
// (una ruta de Windows). Los caracteres de control se reemplazan por '?' en
// vez de emitir una secuencia \u: no aparecen en un nombre de juego, y el
// parser del otro lado no tendria que aprender a deshacerlos.
void put_escaped(char *out, size_t cap, size_t &n, const char *s) {
    for (; s && *s && n + 2 < cap; ++s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = static_cast<char>(c); }
        else if (c < 0x20)         { out[n++] = '?'; }
        else                        { out[n++] = static_cast<char>(c); }
    }
}

}  // namespace

size_t format_header(char *out, size_t cap, const char *exe, const char *adapter,
                     const OutputInfo &res, u64 vram_budget, const char *version) {
    size_t n = 0;
    n += static_cast<size_t>(std::snprintf(out + n, cap - n,
        "{\"t\":\"header\",\"schema\":%d,\"version\":\"", kJsonlSchema));
    put_escaped(out, cap, n, version);
    n += static_cast<size_t>(std::snprintf(out + n, cap - n, "\",\"exe\":\""));
    put_escaped(out, cap, n, exe);
    n += static_cast<size_t>(std::snprintf(out + n, cap - n, "\",\"adapter\":\""));
    put_escaped(out, cap, n, adapter);
    n += static_cast<size_t>(std::snprintf(out + n, cap - n,
        "\",\"out_w\":%u,\"out_h\":%u,\"vram_budget\":%llu}",
        res.w, res.h, static_cast<unsigned long long>(vram_budget)));
    return n;
}

size_t format_event(char *out, size_t cap, const Event &e, const OutputInfo &res) {
    const char *t = event_kind_name(e.kind);
    const unsigned deep = (e.flags & kEvDeepFrame) ? 1u : 0u;
    int n = 0;

    switch (e.kind) {
        case EventKind::None:
            return 0;

        case EventKind::FrameBegin:
            n = std::snprintf(out, cap, "{\"t\":\"%s\",\"f\":%llu,\"deep\":%u}",
                              t, (unsigned long long)e.frame, deep);
            break;

        case EventKind::FrameEnd:
            n = std::snprintf(out, cap,
                "{\"t\":\"%s\",\"f\":%llu,\"cpu_ms\":%.4f,\"present_ms\":%.4f,"
                "\"dropped\":%u,\"queries\":%u,\"deep\":%u}",
                t, (unsigned long long)e.frame,
                static_cast<double>(e.frame_ev.cpu_ns) / 1e6,
                static_cast<double>(e.frame_ev.present_ns) / 1e6,
                e.frame_ev.dropped, e.frame_ev.queries_used, deep);
            break;

        case EventKind::ResourceCreate: {
            const ResourceDesc &d = e.resource.desc;
            n = std::snprintf(out, cap,
                "{\"t\":\"res\",\"f\":%llu,\"key\":\"0x%llx\",\"dkey\":\"0x%llx\","
                "\"cat\":\"%s\",\"dim\":\"%s\",\"fmt\":\"%s\",\"fmt_id\":%u,"
                "\"w\":%u,\"h\":%u,\"d\":%u,\"mips\":%u,\"samples\":%u,"
                "\"flags\":%u,\"heap\":\"%s\",\"bytes\":%llu,\"site\":\"0x%llx\"}",
                (unsigned long long)e.frame,
                (unsigned long long)e.resource.key.v,
                (unsigned long long)e.resource.dkey.v,
                category_name(classify(d, res)), dim_name(d.dim),
                format_name(d.fmt), static_cast<unsigned>(d.fmt),
                d.w, d.h, d.depth, d.mips, d.samples, d.flags, heap_name(d.heap),
                (unsigned long long)d.bytes,
                (unsigned long long)e.resource.site.v);
            break;
        }

        case EventKind::ResourceDestroy:
            n = std::snprintf(out, cap,
                "{\"t\":\"res_free\",\"f\":%llu,\"key\":\"0x%llx\"}",
                (unsigned long long)e.frame,
                (unsigned long long)e.resource.key.v);
            break;

        case EventKind::Pso:
            n = std::snprintf(out, cap,
                "{\"t\":\"pso\",\"f\":%llu,\"key\":\"0x%llx\",\"compile_ms\":%.4f,"
                "\"bytes\":%llu,\"stages\":%u,\"compute\":%u,\"render_thread\":%u}",
                (unsigned long long)e.frame,
                (unsigned long long)e.pso.key.v,
                static_cast<double>(e.pso.compile_ns) / 1e6,
                (unsigned long long)e.pso.bytecode_bytes,
                e.pso.stage_mask, e.pso.is_compute,
                (e.flags & kEvRenderThread) ? 1u : 0u);
            break;

        case EventKind::Draw:
        case EventKind::Dispatch:
            n = std::snprintf(out, cap,
                "{\"t\":\"%s\",\"f\":%llu,\"pass\":\"0x%llx\",\"pso\":\"0x%llx\","
                "\"n\":%u,\"inst\":%u,\"gx\":%u,\"gy\":%u,\"gz\":%u}",
                t, (unsigned long long)e.frame,
                (unsigned long long)e.draw.pass.v,
                (unsigned long long)e.draw.pso.v,
                e.draw.count, e.draw.instances, e.draw.gx, e.draw.gy, e.draw.gz);
            break;

        case EventKind::Barrier:
            n = std::snprintf(out, cap,
                "{\"t\":\"barrier\",\"f\":%llu,\"res\":\"0x%llx\",\"from\":%u,"
                "\"to\":%u,\"kind\":%u,\"dropped\":%u}",
                (unsigned long long)e.frame,
                (unsigned long long)e.barrier.key.v,
                e.barrier.before, e.barrier.after, e.barrier.kind,
                e.barrier.dropped);
            break;

        case EventKind::PassBegin:
        case EventKind::PassEnd:
            n = std::snprintf(out, cap,
                "{\"t\":\"%s\",\"f\":%llu,\"pass\":\"0x%llx\",\"ord\":%u}",
                t, (unsigned long long)e.frame,
                (unsigned long long)e.pass.pass.v, e.pass.ordinal);
            break;

        case EventKind::PassTiming:
            n = std::snprintf(out, cap,
                "{\"t\":\"pass\",\"f\":%llu,\"key\":\"0x%llx\",\"rts\":\"0x%llx\","
                "\"pso\":\"0x%llx\",\"gpu_ms\":%.5f,\"draws\":%u,\"ord\":%u,"
                "\"queue\":%u,\"rt_w\":%u,\"rt_h\":%u,\"rt_bytes\":%llu,"
                "\"begin\":%llu,\"end\":%llu,\"freq\":%llu,\"deep\":%u}",
                (unsigned long long)e.frame,
                (unsigned long long)e.pass.pass.v,
                (unsigned long long)e.pass.rts.v,
                (unsigned long long)e.pass.first_pso.v,
                ticks_to_ms(e.pass.gpu_begin_ticks, e.pass.gpu_end_ticks,
                            e.pass.tick_freq),
                e.pass.draws, e.pass.ordinal, e.pass.queue,
                e.pass.rt_w, e.pass.rt_h,
                (unsigned long long)e.pass.rt_bytes,
                (unsigned long long)e.pass.gpu_begin_ticks,
                (unsigned long long)e.pass.gpu_end_ticks,
                (unsigned long long)e.pass.tick_freq, deep);
            break;

        case EventKind::Vram:
            n = std::snprintf(out, cap,
                "{\"t\":\"vram\",\"f\":%llu,\"budget\":%llu,\"usage\":%llu,"
                "\"committed\":%llu,\"reserved\":%llu}",
                (unsigned long long)e.frame,
                (unsigned long long)e.vram.budget,
                (unsigned long long)e.vram.current_usage,
                (unsigned long long)e.vram.committed,
                (unsigned long long)e.vram.available_res);
            break;

        case EventKind::ActionApplied:
            n = std::snprintf(out, cap,
                "{\"t\":\"action\",\"f\":%llu,\"id\":%llu,\"target\":\"0x%llx\","
                "\"kind\":%u,\"on\":%u}",
                (unsigned long long)e.frame,
                (unsigned long long)e.action.action_id,
                (unsigned long long)e.action.target.v,
                e.action.kind, e.action.enabled);
            break;
    }

    if (n < 0) return 0;
    return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

bool parse_line(const char *line, size_t len, ParsedLine &out) {
    out.n = 0;
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= len || line[i] != '{') return false;
    ++i;

    while (i < len) {
        while (i < len && (line[i] == ' ' || line[i] == ',')) ++i;
        if (i < len && line[i] == '}') return true;
        if (i >= len || line[i] != '"') return false;
        ++i;
        const char *k = line + i;
        while (i < len && line[i] != '"') ++i;
        if (i >= len) return false;
        const size_t kl = static_cast<size_t>(line + i - k);
        ++i;
        while (i < len && (line[i] == ' ' || line[i] == ':')) ++i;
        if (i >= len) return false;

        const char *v;
        size_t vl;
        bool is_str = false;
        if (line[i] == '"') {
            is_str = true;
            ++i;
            v = line + i;
            while (i < len && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < len) ++i;
                ++i;
            }
            if (i >= len) return false;
            vl = static_cast<size_t>(line + i - v);
            ++i;
        } else {
            v = line + i;
            while (i < len && line[i] != ',' && line[i] != '}') ++i;
            if (i >= len) return false;
            vl = static_cast<size_t>(line + i - v);
            while (vl > 0 && v[vl - 1] == ' ') --vl;
        }

        if (out.n < sizeof(out.kv) / sizeof(out.kv[0]))
            out.kv[out.n++] = KV{k, kl, v, vl, is_str};
    }
    return false;  // no cerro la llave: linea cortada por un crash del juego
}

bool event_from_line(const ParsedLine &p, Event &e) {
    size_t tn = 0;
    const char *t = p.s("t", &tn);
    if (!t) return false;
    auto is = [&](const char *k) {
        return tn == std::strlen(k) && std::memcmp(t, k, tn) == 0;
    };

    e = Event{};
    e.frame = p.u("f");
    if (p.u("deep")) e.flags |= kEvDeepFrame;

    if (is("frame_begin")) {
        e.kind = EventKind::FrameBegin;
    } else if (is("frame_end")) {
        e.kind = EventKind::FrameEnd;
        e.frame_ev.cpu_ns = static_cast<u64>(p.f("cpu_ms") * 1e6);
        e.frame_ev.present_ns = static_cast<u64>(p.f("present_ms") * 1e6);
        e.frame_ev.dropped = static_cast<u32>(p.u("dropped"));
        e.frame_ev.queries_used = static_cast<u32>(p.u("queries"));
    } else if (is("res")) {
        e.kind = EventKind::ResourceCreate;
        e.resource.key = ResKey{p.u("key")};
        e.resource.dkey = DescKey{p.u("dkey")};
        e.resource.site = CallsiteId{p.u("site")};
        ResourceDesc &d = e.resource.desc;
        d.fmt = static_cast<Format>(p.u("fmt_id"));
        d.w = static_cast<u32>(p.u("w"));
        d.h = static_cast<u32>(p.u("h"));
        d.depth = static_cast<u32>(p.u("d", 1));
        d.mips = static_cast<u32>(p.u("mips", 1));
        d.samples = static_cast<u32>(p.u("samples", 1));
        d.flags = static_cast<u32>(p.u("flags"));
        d.bytes = p.u("bytes");
        d.dim = p.str_is("dim", "buffer") ? Dim::Buffer
              : p.str_is("dim", "tex1d")  ? Dim::Texture1D
              : p.str_is("dim", "tex3d")  ? Dim::Texture3D
              : p.str_is("dim", "tex2d")  ? Dim::Texture2D
                                          : Dim::Unknown;
        d.heap = p.str_is("heap", "default")  ? HeapKind::Default
               : p.str_is("heap", "upload")   ? HeapKind::Upload
               : p.str_is("heap", "readback") ? HeapKind::Readback
               : p.str_is("heap", "custom")   ? HeapKind::Custom
               : p.str_is("heap", "placed")   ? HeapKind::Placed
               : p.str_is("heap", "reserved") ? HeapKind::Reserved
                                              : HeapKind::Unknown;
    } else if (is("res_free")) {
        e.kind = EventKind::ResourceDestroy;
        e.resource.key = ResKey{p.u("key")};
    } else if (is("pso")) {
        e.kind = EventKind::Pso;
        e.pso.key = PsoKey{p.u("key")};
        e.pso.compile_ns = static_cast<u64>(p.f("compile_ms") * 1e6);
        e.pso.bytecode_bytes = p.u("bytes");
        e.pso.stage_mask = static_cast<u32>(p.u("stages"));
        e.pso.is_compute = static_cast<u32>(p.u("compute"));
        if (p.u("render_thread")) e.flags |= kEvRenderThread;
    } else if (is("draw") || is("dispatch")) {
        e.kind = is("draw") ? EventKind::Draw : EventKind::Dispatch;
        e.draw.pass = PassKey{p.u("pass")};
        e.draw.pso = PsoKey{p.u("pso")};
        e.draw.count = static_cast<u32>(p.u("n"));
        e.draw.instances = static_cast<u32>(p.u("inst"));
        e.draw.gx = static_cast<u32>(p.u("gx"));
        e.draw.gy = static_cast<u32>(p.u("gy"));
        e.draw.gz = static_cast<u32>(p.u("gz"));
    } else if (is("barrier")) {
        e.kind = EventKind::Barrier;
        e.barrier.key = ResKey{p.u("res")};
        e.barrier.before = static_cast<u32>(p.u("from"));
        e.barrier.after = static_cast<u32>(p.u("to"));
        e.barrier.kind = static_cast<u32>(p.u("kind"));
        e.barrier.dropped = static_cast<u32>(p.u("dropped"));
    } else if (is("pass")) {
        e.kind = EventKind::PassTiming;
        e.pass.pass = PassKey{p.u("key")};
        e.pass.rts = RtSetKey{p.u("rts")};
        e.pass.first_pso = PsoKey{p.u("pso")};
        e.pass.gpu_begin_ticks = p.u("begin");
        e.pass.gpu_end_ticks = p.u("end");
        e.pass.tick_freq = p.u("freq");
        e.pass.draws = static_cast<u32>(p.u("draws"));
        e.pass.ordinal = static_cast<u32>(p.u("ord"));
        e.pass.queue = static_cast<u32>(p.u("queue"));
        e.pass.rt_w = static_cast<u32>(p.u("rt_w"));
        e.pass.rt_h = static_cast<u32>(p.u("rt_h"));
        e.pass.rt_bytes = p.u("rt_bytes");
        // Si la linea vino sin ticks (un fixture escrito a mano) se reconstruye
        // una base de 1 GHz desde gpu_ms, para que el modelo de frame no tenga
        // que saber de donde salio el numero.
        if (e.pass.tick_freq == 0 && p.has("gpu_ms")) {
            e.pass.tick_freq = 1000000000ull;
            e.pass.gpu_begin_ticks = 0;
            e.pass.gpu_end_ticks = static_cast<u64>(p.f("gpu_ms") * 1e6);
        }
    } else if (is("vram")) {
        e.kind = EventKind::Vram;
        e.vram.budget = p.u("budget");
        e.vram.current_usage = p.u("usage");
        e.vram.committed = p.u("committed");
        e.vram.available_res = p.u("reserved");
    } else if (is("action")) {
        e.kind = EventKind::ActionApplied;
        e.action.action_id = p.u("id");
        e.action.target = DescKey{p.u("target")};
        e.action.kind = static_cast<u32>(p.u("kind"));
        e.action.enabled = static_cast<u32>(p.u("on"));
    } else {
        return false;  // header u otra linea que no es un evento
    }
    return true;
}

}  // namespace gp
