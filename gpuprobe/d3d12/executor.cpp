// executor.cpp -- ver executor.h.
#define WIN32_LEAN_AND_MEAN
#include "executor.h"

#include <windows.h>

#include <cstdio>
#include <vector>

namespace gp {

namespace {

Executor g_executor;

std::string local_dir(const char *sub) {
    char local[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH)) return {};
    std::string p = std::string(local) + "\\gpuprobe";
    CreateDirectoryA(p.c_str(), nullptr);
    p += "\\";
    p += sub;
    CreateDirectoryA(p.c_str(), nullptr);
    return p;
}

u64 file_mtime(const std::string &path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<u64>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
           fad.ftLastWriteTime.dwLowDateTime;
}

std::string read_file(const std::string &path) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::string out;
    if (size.QuadPart > 0 && size.QuadPart < (4 << 20)) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr))
            out.clear();
        else
            out.resize(read);
    }
    CloseHandle(h);
    return out;
}

}  // namespace

Executor &executor() { return g_executor; }

bool Executor::init(const char *exe, OutputInfo out) {
    core_.set_output(out);
    const std::string profiles = local_dir("profiles");
    const std::string safety = local_dir("safety");
    if (profiles.empty()) return false;
    std::string name = exe && exe[0] ? exe : "desconocido.exe";
    // <exe>.toml, sin la extension del ejecutable: Cyberpunk2077.exe ->
    // Cyberpunk2077.toml. Mas facil de escribir a mano.
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) name.resize(dot);
    profile_path_ = profiles + "\\" + name + ".toml";
    ledger_path_ = safety + "\\" + name + ".txt";
    load_ledger();
    load_profile(true);
    armed_ = true;
    return true;
}

void Executor::shutdown() {
    if (!armed_) return;
    save_ledger();
    armed_ = false;
}

void Executor::load_profile(bool first) {
    const std::string text = read_file(profile_path_);
    if (text.empty()) {
        if (first) {
            // Sin perfil no hay nada que aplicar, y esta bien: gpuprobe
            // arranca en OBSERVE. El archivo lo escribe el analizador.
            last_error_ = "sin perfil: modo observacion";
            Profile empty;
            empty.observe_only = true;
            core_.set_profile(empty);
        }
        return;
    }
    const ParseResult r = parse_profile(text.data(), text.size());
    if (!r.ok) {
        // Un perfil con un error NO se aplica ni a medias: sigue el anterior.
        // Con hot-reload, aplicar la mitad dejaria un estado que despues nadie
        // puede reproducir.
        char buf[256];
        const ParseError &e = r.errors.front();
        std::snprintf(buf, sizeof buf, "perfil con %zu error(es); sigue el "
                      "anterior. Primero: linea %d: %s",
                      r.errors.size(), e.line, e.msg.c_str());
        last_error_ = buf;
        return;
    }
    last_error_.clear();
    core_.set_profile(r.profile);
    ++reloads_;
}

void Executor::load_ledger() {
    // La libreta de seguridad: lo que se observo en sesiones anteriores. Es lo
    // que permite que la SEGUNDA sesion de un juego ya pueda actuar -- la
    // primera es siempre observacion, porque sin haber visto un recurso no hay
    // con que garantizar que escalarlo sea seguro.
    const std::string text = read_file(ledger_path_);
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        unsigned long long key = 0;
        unsigned flags = 0, instances = 0;
        if (std::sscanf(text.c_str() + i, "%llx %u %u", &key, &flags, &instances) >= 2) {
            const DescKey k{key};
            ResourceDesc dummy;
            core_.note_resource(k, dummy);
            if (flags & 0x1) core_.note_copy(k, true);
            if (flags & 0x2) core_.note_copy(k, false);
            if (flags & 0x4) core_.note_placed(k);
            if (flags & 0x8) core_.note_viewport(k);
            if (flags & 0x10) core_.note_reserved(k);
        }
        i = e + 1;
    }
}

void Executor::save_ledger() {
    if (!ledger_dirty_ || ledger_path_.empty()) return;
    FILE *f = fopen(ledger_path_.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "# gpuprobe: lo observado sobre cada clase de recurso.\n"
                    "# dkey flags instancias -- flags: 1 copy_src, 2 copy_dst,\n"
                    "# 4 placed, 8 viewports vistos, 16 tiled.\n"
                    "# Borrar este archivo devuelve el juego a modo observacion.\n");
    for (const auto &kv : core_.observations()) {
        const Observed &o = kv.second;
        u32 flags = 0;
        if (o.copy_source) flags |= 0x1;
        if (o.copy_dest) flags |= 0x2;
        if (o.placed) flags |= 0x4;
        if (o.viewport_tracked) flags |= 0x8;
        if (o.reserved) flags |= 0x10;
        std::fprintf(f, "%llx %u %u\n", static_cast<unsigned long long>(kv.first),
                     flags, o.instances);
    }
    std::fclose(f);
    ledger_dirty_ = false;
}

ResourceOverride Executor::on_create(const ResourceDesc &d, CallsiteId site) {
    ResourceOverride ov;
    const Decision dec = core_.decide_resource(d, site);
    if (!dec.apply()) return ov;
    if (dec.scale < 1.0 && dec.scale > 0.0) {
        ov.w = static_cast<u32>(static_cast<double>(d.w) * dec.scale);
        ov.h = static_cast<u32>(static_cast<double>(d.h) * dec.scale);
        // Nunca por debajo de 64: un RT de 8 pixeles no es una optimizacion,
        // es un recurso roto que el juego va a usar igual.
        if (ov.w < 64) ov.w = 64;
        if (ov.h < 64) ov.h = 64;
        // Multiplo de 8: los juegos asumen alineacion en los shaders de post y
        // un tamano impar produce artefactos de borde.
        ov.w &= ~7u;
        ov.h &= ~7u;
    }
    ov.mip_bias = static_cast<i8>(dec.mip_bias);
    return ov;
}

void Executor::note_resource(DescKey k, const ResourceDesc &d, bool placed,
                             bool reserved) {
    core_.note_resource(k, d);
    if (placed) core_.note_placed(k);
    if (reserved) core_.note_reserved(k);
    ledger_dirty_ = true;
}

void Executor::note_copy(DescKey k, bool as_source) {
    core_.note_copy(k, as_source);
    ledger_dirty_ = true;
}

void Executor::note_viewport(DescKey k) {
    core_.note_viewport(k);
    ledger_dirty_ = true;
}

bool Executor::skip_pass(PassKey p, DescKey rt) {
    return core_.decide_pass(p, rt).apply();
}

bool Executor::drop_barrier(u32 before, u32 after) {
    return core_.decide_barrier(before, after).apply();
}

void Executor::begin_frame(u64 index) {
    // Hot-reload: se mira la fecha del perfil una vez por segundo de juego
    // aproximado (60 frames), no en cada frame. Un stat por frame en el hilo
    // de presentacion es justo el tipo de costo que despues aparece como
    // "gpuprobe agrega 0.2 ms".
    if (index >= last_check_frame_ + 60) {
        last_check_frame_ = index;
        const u64 mt = file_mtime(profile_path_);
        if (mt && mt != last_write_) {
            last_write_ = mt;
            std::lock_guard<std::mutex> lk(mu_);
            load_profile(false);
        }
    }
    // El harness conmuta aca, en el limite de frame. Cada cambio de estado
    // queda en el JSONL: es lo que despues le permite al analizador partir el
    // frametime en dos condiciones y medir la ganancia de verdad. Un toggle
    // sin evento es una medicion que nadie puede reconstruir.
    const bool changed = core_.advance_frame(index);
    if (changed || first_frame_) {
        first_frame_ = false;
        for (const Action &a : core_.profile().actions) {
            const bool on = core_.is_on(a.id);
            auto it = last_state_.find(a.id);
            if (it != last_state_.end() && it->second == on) continue;
            last_state_[a.id] = on;
            collector().note_action(a.id, a.match.dkey,
                                    static_cast<u32>(a.kind), on);
        }
    }
}

const char *Executor::capture_due(u64 index) {
    if (!armed_) return nullptr;
    const Profile &p = core_.profile();
    if (p.observe_only) return nullptr;
    // Se captura cuando el estado de una accion lleva estable el warmup del
    // perfil: antes de eso la imagen puede tener caches a medio llenar o un
    // RT recien recreado, y la comparacion visual no seria justa.
    const u64 settle = p.ab_warmup ? p.ab_warmup : 5;
    for (const Action &a : p.actions) {
        if (!a.ab) continue;
        CapState &c = caps_[a.id];
        const bool on = core_.is_on(a.id);
        if (on != c.on) {
            c.on = on;
            c.changed = index;
            continue;
        }
        if (index < c.changed + settle) continue;
        bool &shot = on ? c.shot_on : c.shot_off;
        if (shot) continue;
        shot = true;
        std::snprintf(capture_suffix_, sizeof capture_suffix_, "a%llu-%s",
                      static_cast<unsigned long long>(a.id), on ? "on" : "off");
        return capture_suffix_;
    }
    return nullptr;
}

void Executor::force(u64 id, bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    core_.force(id, on);
    // Un toggle desde el overlay tambien es un cambio de condicion: si no
    // quedara en el log, el analizador atribuiria esos frames al estado
    // anterior y la medicion saldria mezclada.
    last_state_[id] = on;
    const Action *a = core_.profile().find(id);
    collector().note_action(id, a ? a->match.dkey : DescKey{},
                            a ? static_cast<u32>(a->kind) : 0u, on);
}

}  // namespace gp
