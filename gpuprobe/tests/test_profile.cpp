// test_profile.cpp -- el parser del perfil y el matcheo por descriptor.
//
// El perfil es lo unico que el usuario escribe a mano, asi que el parser tiene
// dos obligaciones: entender TOML real, y RECHAZAR entero un archivo con un
// error en vez de aplicar la mitad. Lo segundo importa mas: con hot-reload, un
// perfil a medio aplicar deja el juego en un estado que despues nadie puede
// reproducir.
#include "../core/profile.h"

#include "harness.h"

#include <string>

using namespace gp;

static ParseResult parse(const std::string &s) {
    return parse_profile(s.data(), s.size());
}

static ResourceDesc tex(Format f, u32 w, u32 h, u32 flags, u32 array = 1) {
    ResourceDesc d;
    d.dim = Dim::Texture2D; d.fmt = f; d.w = w; d.h = h; d.depth = array;
    d.mips = 1; d.samples = 1; d.flags = flags; d.heap = HeapKind::Default;
    return d;
}

int main() {
    const OutputInfo out{2560, 1440};

    // --- perfil completo, tal como se documenta --------------------------
    {
        const ParseResult r = parse(R"(
# Cyberpunk 2077 -- medido en la corrida del 2026-09-14
[gpuprobe]
exe = "Cyberpunk2077.exe"
note = "sombras y SSR"
observe_only = false
deep_every = 90
query_budget = 96

[[action]]
name = "sombras 4k a la mitad"
kind = "resource_scale"
match = { category = "shadowmap", min_w = 2048, format = "D32_FLOAT" }
scale = 0.5
verify = "full"
enabled = true
ab = true

[[action]]
name = "SSR a media resolucion"
kind = "resource_scale"
match = { category = "rt_full", format = "R16G16B16A16_FLOAT" }
scale = 0.5
verify = "no_copy_observed"

[[action]]
kind = "barrier_filter"
match = { from = "PIXEL_SHADER_RESOURCE", to = "COMMON" }
enabled = true
)");
        for (const ParseError &e : r.errors)
            std::fprintf(stderr, "  (linea %d) %s\n", e.line, e.msg.c_str());
        CHECK(r.ok);
        CHECK(r.profile.exe == "Cyberpunk2077.exe");
        CHECK(!r.profile.observe_only);
        CHECK_EQ_U64(r.profile.deep_every, 90);
        CHECK_EQ_U64(r.profile.query_budget, 96);
        CHECK_EQ_U64(r.profile.actions.size(), 3);

        const Action &a0 = r.profile.actions[0];
        CHECK(a0.kind == ActionKind::ResourceScale);
        CHECK(a0.name == "sombras 4k a la mitad");
        CHECK(a0.enabled);
        CHECK(a0.ab);
        CHECK(a0.verify == Verify::Full);
        CHECK_NEAR(a0.scale, 0.5, 1e-9);
        CHECK(a0.match.has_category && a0.match.category == Category::ShadowMap);
        CHECK_EQ_U64(a0.match.min_w, 2048);
        CHECK(a0.match.has_format && a0.match.format == Format::D32_Float);

        // Sin enabled explicito, apagada.
        CHECK(!r.profile.actions[1].enabled);
        CHECK(r.profile.actions[1].verify == Verify::NoCopyObserved);

        const Action &a2 = r.profile.actions[2];
        CHECK(a2.kind == ActionKind::BarrierFilter);
        CHECK(a2.match.has_from && a2.match.from_state == kStatePixelShader);
        CHECK(a2.match.has_to && a2.match.to_state == kStateCommon);

        // --- el matcheo ---
        CHECK(matches(a0.match, tex(Format::D32_Float, 4096, 4096,
                                    kFlagAllowDepthStencil), out));
        // Un shadow map de 1024 no llega al min_w.
        CHECK(!matches(a0.match, tex(Format::D32_Float, 1024, 1024,
                                     kFlagAllowDepthStencil), out));
        // El depth de la escena NO es shadowmap: la categoria lo salva.
        CHECK(!matches(a0.match, tex(Format::D32_Float, 2560, 1440,
                                     kFlagAllowDepthStencil), out));
        // Otro formato de depth tampoco.
        CHECK(!matches(a0.match, tex(Format::D16_Unorm, 4096, 4096,
                                     kFlagAllowDepthStencil), out));

        // El RT full-res matchea a 1440p y a 2160p con la MISMA regla.
        const OutputInfo out4k{3840, 2160};
        CHECK(matches(r.profile.actions[1].match,
                      tex(Format::R16G16B16A16_Float, 2560, 1440,
                          kFlagAllowRenderTarget), out));
        CHECK(matches(r.profile.actions[1].match,
                      tex(Format::R16G16B16A16_Float, 3840, 2160,
                          kFlagAllowRenderTarget), out4k));
    }

    // --- un error tira TODO el perfil ------------------------------------
    {
        const ParseResult r = parse(R"(
[[action]]
kind = "resource_scale"
match = { category = "sombras" }
scale = 0.5
)");
        CHECK(!r.ok);
        CHECK(r.profile.actions.empty());   // no queda nada aplicado
        CHECK(r.errors.size() >= 1);
    }

    // --- validaciones que atajan el pie en la escopeta --------------------
    {
        // scale fuera de rango
        CHECK(!parse("[[action]]\nkind=\"resource_scale\"\nmatch={category=\"rt_full\"}\nscale=1.5\n").ok);
        CHECK(!parse("[[action]]\nkind=\"resource_scale\"\nmatch={category=\"rt_full\"}\nscale=0.0\n").ok);
        // resource_scale sin matcher: escalaria todo el juego
        CHECK(!parse("[[action]]\nkind=\"resource_scale\"\nscale=0.5\n").ok);
        // barrier_filter sin from ni to
        CHECK(!parse("[[action]]\nkind=\"barrier_filter\"\nenabled=true\n").ok);
        // mip_bias 0 no hace nada
        CHECK(!parse("[[action]]\nkind=\"mip_bias\"\nmip_bias=0\n").ok);
        // kind faltante
        CHECK(!parse("[[action]]\nname=\"x\"\n").ok);
        // deep_every 0 seria instrumentar todos los frames
        CHECK(!parse("[gpuprobe]\ndeep_every = 0\n").ok);
        // clave desconocida: mejor rechazar que ignorar en silencio
        CHECK(!parse("[gpuprobe]\nquery_budge = 64\n").ok);
        // Y una accion valida sigue siendo valida.
        CHECK(parse("[[action]]\nkind=\"mip_bias\"\nmip_bias=1\nmatch={category=\"texture\"}\n").ok);
    }

    // --- comentarios, espacios raros, comillas simples --------------------
    {
        const ParseResult r = parse(
            "# comentario\n\n  [gpuprobe]   # al final de la linea\n"
            "exe = 'juego.exe'\n\n\n[[action]]\n"
            "kind='skip_pass'   \nmatch = { category = 'rt_quarter' }\n"
            "enabled=true\n");
        for (const ParseError &e : r.errors)
            std::fprintf(stderr, "  (linea %d) %s\n", e.line, e.msg.c_str());
        CHECK(r.ok);
        CHECK(r.profile.exe == "juego.exe");
        CHECK(r.profile.actions.size() == 1);
        CHECK(r.profile.actions[0].kind == ActionKind::SkipPass);
    }

    // --- ida y vuelta: escribir y volver a parsear ------------------------
    {
        const std::string src =
            "[gpuprobe]\nexe = \"x.exe\"\n"
            "[[action]]\nname = \"sombras\"\nkind = \"resource_scale\"\n"
            "match = { category = \"shadowmap\", min_w = 2048 }\n"
            "scale = 0.5\nenabled = true\n";
        const ParseResult a = parse(src);
        CHECK(a.ok);
        const std::string txt = write_profile(a.profile);
        const ParseResult b = parse(txt);
        for (const ParseError &e : b.errors)
            std::fprintf(stderr, "  (linea %d) %s\n", e.line, e.msg.c_str());
        CHECK(b.ok);
        CHECK_EQ_U64(b.profile.actions.size(), a.profile.actions.size());
        CHECK(b.profile.actions[0].name == "sombras");
        CHECK_NEAR(b.profile.actions[0].scale, 0.5, 1e-6);
        CHECK(b.profile.actions[0].enabled);
        CHECK(b.profile.actions[0].match.category == Category::ShadowMap);
        CHECK_EQ_U64(b.profile.actions[0].match.min_w, 2048);
    }

    // --- un matcher vacio matchea todo (regla global a proposito) ---------
    {
        Matcher m;
        CHECK(matches(m, tex(Format::R8G8B8A8_Unorm, 64, 64, kFlagNone), out));
    }

    return gp_test::report("test_profile");
}
