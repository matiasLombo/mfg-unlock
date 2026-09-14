// profile.cpp -- parser del subconjunto de TOML del perfil, y el matcheo por
// descriptor. Ver profile.h.
#include "profile.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gp {

const char *action_kind_name(ActionKind k) {
    switch (k) {
        case ActionKind::ResourceScale: return "resource_scale";
        case ActionKind::MipBias:       return "mip_bias";
        case ActionKind::SkipPass:      return "skip_pass";
        case ActionKind::BarrierFilter: return "barrier_filter";
        case ActionKind::DrsSettings:   return "drs_settings";
        case ActionKind::None:          break;
    }
    return "none";
}

const char *verify_name(Verify v) {
    switch (v) {
        case Verify::None:             return "none";
        case Verify::NoCopyObserved:   return "no_copy_observed";
        case Verify::NotPlaced:        return "not_placed";
        case Verify::ViewportsTracked: return "viewports_tracked";
        case Verify::Full:             return "full";
    }
    return "full";
}

namespace {

struct Str {
    const char *p = nullptr;
    size_t      n = 0;
    bool eq(const char *s) const {
        return n == std::strlen(s) && std::memcmp(p, s, n) == 0;
    }
    std::string str() const { return std::string(p, n); }
};

bool name_to_category(const Str &s, Category &out) {
    static const struct { const char *n; Category c; } kTable[] = {
        {"unknown", Category::Unknown},        {"buffer", Category::Buffer},
        {"backbuffer", Category::Backbuffer},
        {"shadowmap", Category::ShadowMap},    {"shadowcube", Category::ShadowCube},
        {"depthbuffer", Category::DepthBuffer},{"cubemap", Category::CubeMap},
        {"volume", Category::Volume},          {"rt_full", Category::RtFull},
        {"rt_half", Category::RtHalf},         {"rt_quarter", Category::RtQuarter},
        {"rt_other", Category::RtOther},       {"texture", Category::Texture},
        {"staging", Category::Staging},
    };
    for (const auto &e : kTable)
        if (s.eq(e.n)) { out = e.c; return true; }
    return false;
}

bool name_to_scale(const Str &s, ScaleClass &out) {
    static const struct { const char *n; ScaleClass c; } kTable[] = {
        {"fixed", ScaleClass::Fixed},   {"full", ScaleClass::Full},
        {"half", ScaleClass::Half},     {"quarter", ScaleClass::Quarter},
        {"eighth", ScaleClass::Eighth}, {"larger", ScaleClass::Larger},
    };
    for (const auto &e : kTable)
        if (s.eq(e.n)) { out = e.c; return true; }
    return false;
}

// Formato por nombre: el mismo que escribe el JSONL, para que el usuario pueda
// copiar y pegar del reporte al perfil sin traducir nada.
bool name_to_format(const Str &s, Format &out) {
    for (u32 v = 0; v <= 115; ++v) {
        const Format f = static_cast<Format>(v);
        const char *n = format_name(f);
        if (n[0] == 'f' && n[1] == 'm' && n[2] == 't') continue;  // no esta en la tabla
        if (s.eq(n)) { out = f; return true; }
    }
    return false;
}

bool name_to_state(const Str &s, u32 &out) {
    static const struct { const char *n; u32 v; } kTable[] = {
        {"COMMON", kStateCommon},
        {"VERTEX_AND_CONSTANT_BUFFER", kStateVertexAndConstant},
        {"INDEX_BUFFER", kStateIndex},
        {"RENDER_TARGET", kStateRenderTarget},
        {"UNORDERED_ACCESS", kStateUnorderedAccess},
        {"DEPTH_WRITE", kStateDepthWrite},
        {"DEPTH_READ", kStateDepthRead},
        {"NON_PIXEL_SHADER_RESOURCE", kStateNonPixelShader},
        {"PIXEL_SHADER_RESOURCE", kStatePixelShader},
        {"COPY_DEST", kStateCopyDest},
        {"COPY_SOURCE", kStateCopySource},
        {"RESOLVE_DEST", kStateResolveDest},
        {"RESOLVE_SOURCE", kStateResolveSource},
        {"GENERIC_READ", kStateGenericReadV},
    };
    for (const auto &e : kTable)
        if (s.eq(e.n)) { out = e.v; return true; }
    return false;
}

bool name_to_verify(const Str &s, Verify &out) {
    static const struct { const char *n; Verify v; } kTable[] = {
        {"none", Verify::None},
        {"no_copy_observed", Verify::NoCopyObserved},
        {"not_placed", Verify::NotPlaced},
        {"viewports_tracked", Verify::ViewportsTracked},
        {"full", Verify::Full},
    };
    for (const auto &e : kTable)
        if (s.eq(e.n)) { out = e.v; return true; }
    return false;
}

bool name_to_kind(const Str &s, ActionKind &out) {
    static const struct { const char *n; ActionKind k; } kTable[] = {
        {"resource_scale", ActionKind::ResourceScale},
        {"mip_bias", ActionKind::MipBias},
        {"skip_pass", ActionKind::SkipPass},
        {"barrier_filter", ActionKind::BarrierFilter},
        {"drs_settings", ActionKind::DrsSettings},
    };
    for (const auto &e : kTable)
        if (s.eq(e.n)) { out = e.k; return true; }
    return false;
}

// --- lexer minimo ---------------------------------------------------------

struct Value {
    enum class T { Int, Float, Bool, String, Table } t = T::Int;
    i64         i = 0;
    double      f = 0.0;
    bool        b = false;
    Str         s;
    // Tabla inline: pares sin anidar.
    std::vector<std::pair<Str, Value>> table;
};

class Parser {
public:
    Parser(const char *text, size_t len, ParseResult &res)
        : p_(text), end_(text + len), res_(res) {}

    void run() {
        Action *cur_action = nullptr;
        enum class Sec { Root, Gpuprobe, Action } sec = Sec::Root;

        while (skip_ws_and_comments()) {
            if (peek() == '[') {
                ++p_;
                const bool array = (peek() == '[');
                if (array) ++p_;
                const Str name = bare();
                expect(']');
                if (array) expect(']');
                if (name.eq("action")) {
                    if (!array) {
                        err("[action] tiene que ser [[action]]: son varias");
                        continue;
                    }
                    res_.profile.actions.push_back(Action{});
                    cur_action = &res_.profile.actions.back();
                    cur_action->id = res_.profile.actions.size();
                    sec = Sec::Action;
                } else if (name.eq("gpuprobe")) {
                    sec = Sec::Gpuprobe;
                    cur_action = nullptr;
                } else {
                    err("tabla desconocida: " + name.str());
                    sec = Sec::Root;
                }
                continue;
            }

            const Str key = bare();
            if (key.n == 0) { err("se esperaba una clave"); skip_line(); continue; }
            if (!expect('=')) { skip_line(); continue; }
            Value v;
            if (!value(v)) { skip_line(); continue; }

            switch (sec) {
                case Sec::Gpuprobe: apply_gpuprobe(key, v); break;
                case Sec::Action:
                    if (cur_action) apply_action(*cur_action, key, v);
                    break;
                case Sec::Root:
                    // Claves antes de cualquier tabla: las del perfil.
                    apply_gpuprobe(key, v);
                    break;
            }
        }

        validate();
        res_.ok = res_.errors.empty();
    }

private:
    const char *p_;
    const char *end_;
    ParseResult &res_;
    int line_ = 1;

    void err(const std::string &m) { res_.errors.push_back(ParseError{line_, m}); }

    char peek() const { return p_ < end_ ? *p_ : '\0'; }

    void skip_line() {
        while (p_ < end_ && *p_ != '\n') ++p_;
    }

    // Devuelve false en EOF.
    bool skip_ws_and_comments() {
        for (;;) {
            while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) {
                if (*p_ == '\n') ++line_;
                ++p_;
            }
            if (p_ < end_ && *p_ == '#') { skip_line(); continue; }
            return p_ < end_;
        }
    }

    void skip_inline_ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t')) ++p_;
    }

    bool expect(char c) {
        skip_inline_ws();
        if (p_ < end_ && *p_ == c) { ++p_; return true; }
        err(std::string("se esperaba '") + c + "'");
        return false;
    }

    Str bare() {
        skip_inline_ws();
        Str s;
        s.p = p_;
        while (p_ < end_ && (isalnum(static_cast<unsigned char>(*p_)) || *p_ == '_' ||
                             *p_ == '-' || *p_ == '.'))
            ++p_;
        s.n = static_cast<size_t>(p_ - s.p);
        return s;
    }

    bool value(Value &v) {
        skip_inline_ws();
        if (p_ >= end_) { err("valor vacio"); return false; }

        if (*p_ == '"' || *p_ == '\'') {
            const char q = *p_++;
            v.t = Value::T::String;
            v.s.p = p_;
            while (p_ < end_ && *p_ != q) {
                if (*p_ == '\n') { err("string sin cerrar"); return false; }
                ++p_;
            }
            v.s.n = static_cast<size_t>(p_ - v.s.p);
            if (p_ < end_) ++p_;
            return true;
        }

        if (*p_ == '{') {
            ++p_;
            v.t = Value::T::Table;
            for (;;) {
                skip_ws_and_comments();
                if (peek() == '}') { ++p_; return true; }
                const Str k = bare();
                if (k.n == 0) { err("clave vacia en tabla inline"); return false; }
                if (!expect('=')) return false;
                Value inner;
                if (!value(inner)) return false;
                if (inner.t == Value::T::Table) {
                    err("tablas inline anidadas no: " + k.str());
                    return false;
                }
                v.table.emplace_back(k, inner);
                skip_inline_ws();
                if (peek() == ',') { ++p_; continue; }
                if (peek() == '}') { ++p_; return true; }
                skip_ws_and_comments();
                if (peek() == '}') { ++p_; return true; }
                if (peek() == ',') { ++p_; continue; }
                err("se esperaba ',' o '}' en tabla inline");
                return false;
            }
        }

        // true / false
        if (end_ - p_ >= 4 && std::memcmp(p_, "true", 4) == 0) {
            p_ += 4; v.t = Value::T::Bool; v.b = true; return true;
        }
        if (end_ - p_ >= 5 && std::memcmp(p_, "false", 5) == 0) {
            p_ += 5; v.t = Value::T::Bool; v.b = false; return true;
        }

        // numero: entero, hex o float
        const char *start = p_;
        if (peek() == '-' || peek() == '+') ++p_;
        bool is_float = false, is_hex = false;
        if (end_ - p_ > 2 && p_[0] == '0' && (p_[1] == 'x' || p_[1] == 'X')) {
            is_hex = true;
            p_ += 2;
            while (p_ < end_ && isxdigit(static_cast<unsigned char>(*p_))) ++p_;
        } else {
            while (p_ < end_ && (isdigit(static_cast<unsigned char>(*p_)) || *p_ == '.' ||
                                 *p_ == 'e' || *p_ == 'E' || *p_ == '_')) {
                if (*p_ == '.' || *p_ == 'e' || *p_ == 'E') is_float = true;
                ++p_;
            }
        }
        if (p_ == start) { err("valor no reconocido"); skip_line(); return false; }
        std::string num(start, static_cast<size_t>(p_ - start));
        if (is_float) {
            v.t = Value::T::Float;
            v.f = std::strtod(num.c_str(), nullptr);
            v.i = static_cast<i64>(v.f);
        } else {
            v.t = Value::T::Int;
            v.i = static_cast<i64>(std::strtoll(num.c_str(), nullptr, is_hex ? 16 : 10));
            v.f = static_cast<double>(v.i);
        }
        return true;
    }

    void apply_gpuprobe(const Str &k, const Value &v) {
        Profile &pr = res_.profile;
        if (k.eq("exe"))              pr.exe = v.s.str();
        else if (k.eq("note"))        pr.note = v.s.str();
        else if (k.eq("deep_every"))  pr.deep_every = static_cast<u32>(v.i);
        else if (k.eq("query_budget"))pr.query_budget = static_cast<u32>(v.i);
        else if (k.eq("ab_period"))   pr.ab_period = static_cast<u32>(v.i);
        else if (k.eq("ab_warmup"))   pr.ab_warmup = static_cast<u32>(v.i);
        else if (k.eq("observe_only"))pr.observe_only = v.b;
        else err("clave desconocida en [gpuprobe]: " + k.str());
    }

    void apply_match(Matcher &m, const Value &v) {
        if (v.t != Value::T::Table) { err("match tiene que ser una tabla inline"); return; }
        for (const auto &kv : v.table) {
            const Str &k = kv.first;
            const Value &val = kv.second;
            if (k.eq("category")) {
                if (!name_to_category(val.s, m.category)) err("categoria desconocida: " + val.s.str());
                else m.has_category = true;
            } else if (k.eq("format")) {
                if (!name_to_format(val.s, m.format)) err("formato desconocido: " + val.s.str());
                else m.has_format = true;
            } else if (k.eq("scale")) {
                if (!name_to_scale(val.s, m.scale)) err("escala desconocida: " + val.s.str());
                else m.has_scale = true;
            } else if (k.eq("min_w")) m.min_w = static_cast<u32>(val.i);
            else if (k.eq("max_w"))  m.max_w = static_cast<u32>(val.i);
            else if (k.eq("min_h"))  m.min_h = static_cast<u32>(val.i);
            else if (k.eq("max_h"))  m.max_h = static_cast<u32>(val.i);
            else if (k.eq("min_array")) m.min_array = static_cast<u32>(val.i);
            else if (k.eq("square")) { m.has_square = true; m.square = val.b; }
            else if (k.eq("dkey"))   m.dkey = DescKey{static_cast<u64>(val.i)};
            else if (k.eq("from")) {
                if (!name_to_state(val.s, m.from_state)) err("estado desconocido: " + val.s.str());
                else m.has_from = true;
            } else if (k.eq("to")) {
                if (!name_to_state(val.s, m.to_state)) err("estado desconocido: " + val.s.str());
                else m.has_to = true;
            } else if (k.eq("require_flags")) m.require_flags = static_cast<u32>(val.i);
            else if (k.eq("deny_flags"))      m.deny_flags = static_cast<u32>(val.i);
            else err("clave desconocida en match: " + k.str());
        }
    }

    void apply_action(Action &a, const Str &k, const Value &v) {
        if (k.eq("kind")) {
            if (!name_to_kind(v.s, a.kind)) err("kind desconocido: " + v.s.str());
        } else if (k.eq("name"))    a.name = v.s.str();
        else if (k.eq("match"))     apply_match(a.match, v);
        else if (k.eq("scale"))     a.scale = v.f;
        else if (k.eq("mip_bias"))  a.mip_bias = static_cast<i32>(v.i);
        else if (k.eq("enabled"))   a.enabled = v.b;
        else if (k.eq("ab"))        a.ab = v.b;
        else if (k.eq("drs_id"))    a.drs_id = static_cast<u32>(v.i);
        else if (k.eq("drs_value")) a.drs_value = static_cast<u32>(v.i);
        else if (k.eq("verify")) {
            if (!name_to_verify(v.s, a.verify)) err("verify desconocido: " + v.s.str());
        } else err("clave desconocida en [[action]]: " + k.str());
    }

    void validate() {
        for (const Action &a : res_.profile.actions) {
            const std::string who = a.name.empty()
                ? ("accion #" + std::to_string(a.id)) : ("'" + a.name + "'");
            if (a.kind == ActionKind::None) {
                err(who + ": falta kind");
                continue;
            }
            if (a.kind == ActionKind::ResourceScale) {
                // Un scale fuera de (0,1] no es una optimizacion: agrandar un
                // RT no baja el frametime, y 0 seria un recurso de tamano cero.
                if (!(a.scale > 0.0 && a.scale <= 1.0))
                    err(who + ": scale tiene que estar en (0, 1], no " +
                        std::to_string(a.scale));
                // Un resource_scale con matcher vacio escalaria TODO el juego.
                const Matcher &m = a.match;
                const bool any = m.has_category || m.has_format || m.has_scale ||
                                 m.min_w || m.max_w || m.min_h || m.max_h ||
                                 m.min_array || m.has_square || m.dkey.v ||
                                 m.require_flags || m.deny_flags;
                if (!any) err(who + ": resource_scale sin matcher escalaria todos "
                                    "los recursos del juego");
            }
            if (a.kind == ActionKind::BarrierFilter && !a.match.has_from &&
                !a.match.has_to)
                err(who + ": barrier_filter necesita from o to");
            if (a.kind == ActionKind::MipBias && a.mip_bias == 0)
                err(who + ": mip_bias 0 no hace nada");
        }
        if (res_.profile.deep_every == 0)
            err("deep_every 0: seria instrumentacion completa en todos los frames");
        if (res_.profile.query_budget < 8)
            err("query_budget menor a 8 no alcanza ni para las pasadas principales");
    }
};

}  // namespace

ParseResult parse_profile(const char *text, size_t len) {
    ParseResult res;
    Parser(text, len, res).run();
    if (!res.ok) res.profile = Profile{};  // o todo o nada
    return res;
}

bool matches(const Matcher &m, const ResourceDesc &d, const OutputInfo &out) {
    if (m.dkey.v && desc_key(d, out) != m.dkey) return false;
    if (m.has_category && classify(d, out) != m.category) return false;
    if (m.has_format && d.fmt != m.format) return false;
    if (m.has_scale && scale_of(d.w, out.w) != m.scale) return false;
    if (m.min_w && d.w < m.min_w) return false;
    if (m.max_w && d.w > m.max_w) return false;
    if (m.min_h && d.h < m.min_h) return false;
    if (m.max_h && d.h > m.max_h) return false;
    if (m.min_array && d.depth < m.min_array) return false;
    if (m.has_square && (d.w == d.h) != m.square) return false;
    if (m.require_flags && (d.flags & m.require_flags) != m.require_flags) return false;
    if (m.deny_flags && (d.flags & m.deny_flags) != 0) return false;
    return true;
}

std::string write_profile(const Profile &p) {
    std::string s;
    s += "# Perfil de gpuprobe. Generado; se puede editar a mano.\n";
    s += "# Hot-reload: guardar este archivo aplica los cambios sin reiniciar el\n";
    s += "# juego. Si tiene un error, no se aplica NADA y el perfil anterior sigue.\n\n";
    s += "[gpuprobe]\n";
    if (!p.exe.empty())  s += "exe = \"" + p.exe + "\"\n";
    if (!p.note.empty()) s += "note = \"" + p.note + "\"\n";
    s += "observe_only = " + std::string(p.observe_only ? "true" : "false") + "\n";
    s += "deep_every = " + std::to_string(p.deep_every) + "\n";
    s += "query_budget = " + std::to_string(p.query_budget) + "\n";
    s += "ab_period = " + std::to_string(p.ab_period) + "\n";
    s += "ab_warmup = " + std::to_string(p.ab_warmup) + "\n";

    for (const Action &a : p.actions) {
        s += "\n[[action]]\n";
        if (!a.name.empty()) s += "name = \"" + a.name + "\"\n";
        s += std::string("kind = \"") + action_kind_name(a.kind) + "\"\n";
        std::string m;
        auto add = [&](const std::string &kv) {
            if (!m.empty()) m += ", ";
            m += kv;
        };
        if (a.match.has_category)
            add(std::string("category = \"") + category_name(a.match.category) + "\"");
        if (a.match.has_format)
            add(std::string("format = \"") + format_name(a.match.format) + "\"");
        if (a.match.has_scale)
            add(std::string("scale = \"") + scale_name(a.match.scale) + "\"");
        if (a.match.min_w) add("min_w = " + std::to_string(a.match.min_w));
        if (a.match.max_w) add("max_w = " + std::to_string(a.match.max_w));
        if (a.match.min_h) add("min_h = " + std::to_string(a.match.min_h));
        if (a.match.max_h) add("max_h = " + std::to_string(a.match.max_h));
        if (a.match.min_array) add("min_array = " + std::to_string(a.match.min_array));
        if (a.match.has_square)
            add(std::string("square = ") + (a.match.square ? "true" : "false"));
        if (!m.empty()) s += "match = { " + m + " }\n";
        if (a.kind == ActionKind::ResourceScale) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.3f", a.scale);
            s += std::string("scale = ") + buf + "\n";
        }
        if (a.kind == ActionKind::MipBias)
            s += "mip_bias = " + std::to_string(a.mip_bias) + "\n";
        s += std::string("verify = \"") + verify_name(a.verify) + "\"\n";
        s += std::string("enabled = ") + (a.enabled ? "true" : "false") + "\n";
        if (a.ab) s += "ab = true\n";
    }
    return s;
}

}  // namespace gp
