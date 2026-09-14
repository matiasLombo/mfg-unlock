// test_executor.cpp -- los gates de seguridad y el calendario del A/B.
//
// Este es el test que evita el peor modo de falla del proyecto: aplicar un
// resource_scale a un recurso que no se puede escalar. Los dos casos hostiles
// del testbed (un RT que se copia con extents fijos, uno colocado en un heap
// compartido) tienen que ser RECHAZADOS, y el motivo tiene que quedar dicho.
#include "../core/executor_core.h"
#include "../core/profile.h"

#include "harness.h"

#include <string>

using namespace gp;

static Profile parse(const std::string &s) {
    const ParseResult r = parse_profile(s.data(), s.size());
    for (const ParseError &e : r.errors)
        std::fprintf(stderr, "  (linea %d) %s\n", e.line, e.msg.c_str());
    return r.profile;
}

static ResourceDesc shadow4k() {
    ResourceDesc d;
    d.dim = Dim::Texture2D;
    d.fmt = Format::D32_Float;
    d.w = d.h = 4096;
    d.depth = 4;
    d.mips = 1;
    d.samples = 1;
    d.flags = kFlagAllowDepthStencil;
    d.heap = HeapKind::Default;
    return d;
}

static ResourceDesc rt_full() {
    ResourceDesc d;
    d.dim = Dim::Texture2D;
    d.fmt = Format::R16G16B16A16_Float;
    d.w = 2560; d.h = 1440;
    d.depth = 1; d.mips = 1; d.samples = 1;
    d.flags = kFlagAllowRenderTarget;
    d.heap = HeapKind::Default;
    return d;
}

int main() {
    const OutputInfo out{2560, 1440};
    const std::string toml =
        "[gpuprobe]\n"
        "observe_only = false\n"
        "ab_period = 60\n"
        "[[action]]\n"
        "name = \"sombras\"\n"
        "kind = \"resource_scale\"\n"
        "match = { category = \"shadowmap\", min_w = 2048 }\n"
        "scale = 0.5\n"
        "verify = \"full\"\n"
        "enabled = true\n"
        "[[action]]\n"
        "name = \"ssr\"\n"
        "kind = \"resource_scale\"\n"
        "match = { category = \"rt_full\", format = \"R16G16B16A16_FLOAT\" }\n"
        "scale = 0.5\n"
        "verify = \"no_copy_observed\"\n"
        "enabled = true\n";

    // --- sin observacion no se aplica NADA --------------------------------
    {
        ExecutorCore ex;
        ex.set_output(out);
        ex.set_profile(parse(toml));
        const Decision d = ex.decide_resource(shadow4k(), CallsiteId{});
        CHECK(!d.apply());
        CHECK(d.why == GateResult::NotObserved);
    }

    // --- observado y limpio: se aplica ------------------------------------
    {
        ExecutorCore ex;
        ex.set_output(out);
        ex.set_profile(parse(toml));
        const ResourceDesc sm = shadow4k();
        const DescKey k = desc_key(sm, out);
        ex.note_resource(k, sm);
        ex.note_viewport(k);
        const Decision d = ex.decide_resource(sm, CallsiteId{});
        CHECK(d.apply());
        CHECK_NEAR(d.scale, 0.5, 1e-9);
    }

    // --- hostil A: lo copian con extents fijos ----------------------------
    {
        ExecutorCore ex;
        ex.set_output(out);
        ex.set_profile(parse(toml));
        const ResourceDesc rt = rt_full();
        const DescKey k = desc_key(rt, out);
        ex.note_resource(k, rt);
        ex.note_viewport(k);
        ex.note_copy(k, true);
        const Decision d = ex.decide_resource(rt, CallsiteId{});
        CHECK(!d.apply());
        CHECK(d.why == GateResult::CopyObserved);
    }

    // --- hostil B: vive en un heap compartido -----------------------------
    {
        ExecutorCore ex;
        ex.set_output(out);
        ex.set_profile(parse(toml));
        const ResourceDesc sm = shadow4k();
        const DescKey k = desc_key(sm, out);
        ex.note_resource(k, sm);
        ex.note_viewport(k);
        ex.note_placed(k);
        const Decision d = ex.decide_resource(sm, CallsiteId{});
        CHECK(!d.apply());
        CHECK(d.why == GateResult::Placed);
    }

    // --- sin viewports rastreados tampoco ---------------------------------
    {
        ExecutorCore ex;
        ex.set_output(out);
        ex.set_profile(parse(toml));
        const ResourceDesc sm = shadow4k();
        const DescKey k = desc_key(sm, out);
        ex.note_resource(k, sm);   // sin note_viewport
        const Decision d = ex.decide_resource(sm, CallsiteId{});
        CHECK(!d.apply());
        CHECK(d.why == GateResult::ViewportsUnknown);
    }

    // --- verify = "none": el usuario se hace cargo ------------------------
    {
        ExecutorCore ex;
        ex.set_output(out);
        ex.set_profile(parse(
            "[gpuprobe]\nobserve_only = false\n"
            "[[action]]\nkind = \"resource_scale\"\n"
            "match = { category = \"shadowmap\", min_w = 2048 }\n"
            "scale = 0.5\nverify = \"none\"\nenabled = true\n"));
        const Decision d = ex.decide_resource(shadow4k(), CallsiteId{});
        CHECK(d.apply());
    }

    // --- observe_only manda sobre todo ------------------------------------
    {
        ExecutorCore ex;
        ex.set_output(out);
        ex.set_profile(parse(
            "[gpuprobe]\nobserve_only = true\n"
            "[[action]]\nkind = \"resource_scale\"\n"
            "match = { category = \"shadowmap\", min_w = 2048 }\n"
            "scale = 0.5\nverify = \"none\"\nenabled = true\n"));
        const Decision d = ex.decide_resource(shadow4k(), CallsiteId{});
        CHECK(!d.apply());
        CHECK(d.why == GateResult::ObserveOnly);
    }

    // --- una accion apagada no se aplica aunque matchee -------------------
    {
        ExecutorCore ex;
        ex.set_output(out);
        ex.set_profile(parse(
            "[gpuprobe]\nobserve_only = false\n"
            "[[action]]\nkind = \"resource_scale\"\n"
            "match = { category = \"shadowmap\", min_w = 2048 }\n"
            "scale = 0.5\nverify = \"none\"\n"));
        CHECK(ex.decide_resource(shadow4k(), CallsiteId{}).why == GateResult::Disabled);
    }

    // --- scale y mip_bias sobre el MISMO recurso --------------------------
    // Una textura puede matchear las dos acciones. Preguntar por una no tiene
    // que devolver la otra, ni taparla porque vino antes en el perfil.
    {
        ExecutorCore ex;
        ex.set_output(out);
        ex.set_profile(parse(
            "[gpuprobe]\nobserve_only = false\n"
            "[[action]]\nkind = \"resource_scale\"\n"
            "match = { category = \"texture\" }\nscale = 0.5\n"
            "verify = \"none\"\nenabled = true\n"
            "[[action]]\nkind = \"mip_bias\"\n"
            "match = { category = \"texture\" }\nmip_bias = 2\n"
            "verify = \"none\"\nenabled = true\n"));
        ResourceDesc tex;
        tex.dim = Dim::Texture2D;
        tex.fmt = Format::BC7_Unorm;
        tex.w = tex.h = 2048;
        tex.depth = 1; tex.mips = 12; tex.samples = 1;
        tex.flags = kFlagNone; tex.heap = HeapKind::Default;
        CHECK(classify(tex, out) == Category::Texture);

        const Decision scale = ex.decide_resource(tex, CallsiteId{},
                                                  ActionKind::ResourceScale);
        CHECK(scale.apply());
        CHECK_NEAR(scale.scale, 0.5, 1e-9);
        CHECK_EQ_U64(scale.mip_bias, 0);

        const Decision bias = ex.decide_resource(tex, CallsiteId{},
                                                 ActionKind::MipBias);
        CHECK(bias.apply());
        CHECK_EQ_U64(bias.mip_bias, 2);
        CHECK_NEAR(bias.scale, 1.0, 1e-9);
    }

    // --- el backbuffer no se toca ni con verify = none --------------------
    {
        ExecutorCore ex;
        ex.set_output(out);
        ex.set_profile(parse(
            "[gpuprobe]\nobserve_only = false\n"
            "[[action]]\nkind = \"resource_scale\"\n"
            "match = { category = \"backbuffer\" }\n"
            "scale = 0.5\nverify = \"none\"\nenabled = true\n"));
        ResourceDesc bb = rt_full();
        bb.swapchain = true;
        const DescKey k = desc_key(bb, out);
        ex.note_resource(k, bb);
        ex.note_viewport(k);
        const Decision d = ex.decide_resource(bb, CallsiteId{});
        CHECK(!d.apply());
        CHECK(d.why == GateResult::Backbuffer);
        // Y clasifica como backbuffer, no como RT full-res.
        CHECK(classify(bb, out) == Category::Backbuffer);
    }

    // --- barriers ---------------------------------------------------------
    {
        ExecutorCore ex;
        ex.set_output(out);
        ex.set_profile(parse(
            "[gpuprobe]\nobserve_only = false\n"
            "[[action]]\nkind = \"barrier_filter\"\n"
            "match = { from = \"PIXEL_SHADER_RESOURCE\", to = \"COMMON\" }\n"
            "enabled = true\n"));
        CHECK(ex.decide_barrier(kStatePixelShader, kStateCommon).apply());
        CHECK(!ex.decide_barrier(kStateRenderTarget, kStateCommon).apply());
        CHECK(!ex.decide_barrier(kStatePixelShader, kStateRenderTarget).apply());
    }

    // --- el calendario del A/B -------------------------------------------
    // Nunca dos prendidas a la vez, y cada accion pasa por prendida y apagada.
    {
        ExecutorCore ex;
        ex.set_output(out);
        ex.set_profile(parse(
            "[gpuprobe]\nobserve_only = false\nab_period = 10\n"
            "[[action]]\nkind = \"mip_bias\"\nmip_bias = 1\n"
            "match = { category = \"texture\" }\nab = true\n"
            "[[action]]\nkind = \"mip_bias\"\nmip_bias = 2\n"
            "match = { category = \"texture\" }\nab = true\n"));
        int on_1 = 0, on_2 = 0, both = 0;
        for (u64 f = 0; f < 200; ++f) {
            ex.advance_frame(f);
            const bool a = ex.is_on(1), b = ex.is_on(2);
            if (a && b) ++both;
            if (a) ++on_1;
            if (b) ++on_2;
        }
        CHECK_EQ_U64(both, 0);
        CHECK(on_1 > 20);
        CHECK(on_2 > 20);
        // Y las dos tienen que haber estado apagadas parte del tiempo.
        CHECK(on_1 < 190);
        CHECK(on_2 < 190);
    }

    // --- el overlay pisa al harness --------------------------------------
    {
        ExecutorCore ex;
        ex.set_output(out);
        ex.set_profile(parse(
            "[gpuprobe]\nobserve_only = false\nab_period = 10\n"
            "[[action]]\nkind = \"mip_bias\"\nmip_bias = 1\n"
            "match = { category = \"texture\" }\nab = true\n"));
        ex.force(1, true);
        CHECK(ex.is_on(1));
        ex.force(1, false);
        CHECK(!ex.is_on(1));
    }

    return gp_test::report("test_executor");
}
