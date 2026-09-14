// test_keys.cpp -- clasificacion de descriptores y estabilidad de las claves.
//
// Lo que se prueba aca es la promesa del proyecto: un perfil escrito en una
// maquina a 1440p tiene que seguir matcheando el mismo RT a 2160p, y un shadow
// map de 4096 tiene que seguir siendo el mismo recurso aunque cambie la
// resolucion de pantalla. Si estos tests se ponen en rojo, el ejecutor deja de
// sobrevivir updates y el proyecto pierde su razon de ser.
#include "../core/keys.h"

#include "harness.h"

using namespace gp;

static ResourceDesc tex2d(Format f, u32 w, u32 h, u32 flags, u32 array = 1,
                          u32 mips = 1) {
    ResourceDesc d;
    d.dim = Dim::Texture2D;
    d.fmt = f;
    d.w = w; d.h = h; d.depth = array; d.mips = mips;
    d.samples = 1; d.flags = flags; d.heap = HeapKind::Default;
    d.bytes = texture_bytes(f, w, h, 1, mips, array);
    return d;
}

int main() {
    const OutputInfo out{2560, 1440};

    // --- clasificacion ---------------------------------------------------
    // Shadow map: depth, cuadrado, sin relacion con la salida.
    CHECK(classify(tex2d(Format::D32_Float, 4096, 4096, kFlagAllowDepthStencil),
                   out) == Category::ShadowMap);
    // Cascadas: array de 4, sigue siendo shadow map.
    CHECK(classify(tex2d(Format::D32_Float, 2048, 2048, kFlagAllowDepthStencil, 4),
                   out) == Category::ShadowMap);
    // Cubo de sombras: array multiplo de 6.
    CHECK(classify(tex2d(Format::D16_Unorm, 1024, 1024, kFlagAllowDepthStencil, 6),
                   out) == Category::ShadowCube);
    // El depth de la escena, a resolucion de salida.
    CHECK(classify(tex2d(Format::D32_Float, 2560, 1440, kFlagAllowDepthStencil),
                   out) == Category::DepthBuffer);
    // Un depth de otra vista, ni cuadrado ni de salida: Unknown a proposito.
    CHECK(classify(tex2d(Format::D32_Float, 800, 450, kFlagAllowDepthStencil),
                   out) == Category::Unknown);

    // RTs por escala.
    CHECK(classify(tex2d(Format::R16G16B16A16_Float, 2560, 1440,
                         kFlagAllowRenderTarget), out) == Category::RtFull);
    CHECK(classify(tex2d(Format::R8G8B8A8_Unorm, 1280, 720,
                         kFlagAllowRenderTarget), out) == Category::RtHalf);
    CHECK(classify(tex2d(Format::R8_Unorm, 640, 360, kFlagAllowRenderTarget),
                   out) == Category::RtQuarter);

    // Volumetrica: Texture3D con UAV.
    ResourceDesc vol = tex2d(Format::R16G16B16A16_Float, 160, 90,
                             kFlagAllowUnorderedAcc);
    vol.dim = Dim::Texture3D;
    vol.depth = 64;
    CHECK(classify(vol, out) == Category::Volume);

    // Un asset con mips no es un RT.
    CHECK(classify(tex2d(Format::BC7_Unorm, 2048, 2048, kFlagNone, 1, 12), out) ==
          Category::Texture);
    // Upload es staging aunque el descriptor diga otra cosa.
    ResourceDesc up = tex2d(Format::R8G8B8A8_Unorm, 2560, 1440, kFlagNone);
    up.heap = HeapKind::Upload;
    CHECK(classify(up, out) == Category::Staging);

    // --- estabilidad de DescKey -----------------------------------------
    // El RT full-res es el mismo recurso a 1440p y a 2160p: el perfil escrito
    // en una resolucion matchea en la otra.
    const OutputInfo out4k{3840, 2160};
    const DescKey rt_1440 = desc_key(
        tex2d(Format::R16G16B16A16_Float, 2560, 1440, kFlagAllowRenderTarget), out);
    const DescKey rt_2160 = desc_key(
        tex2d(Format::R16G16B16A16_Float, 3840, 2160, kFlagAllowRenderTarget), out4k);
    CHECK_EQ_U64(rt_1440.v, rt_2160.v);

    // Con DRS la resolucion interna se mueve unos puntos: sigue siendo Full y
    // sigue siendo la misma clave.
    const DescKey rt_drs = desc_key(
        tex2d(Format::R16G16B16A16_Float, 2432, 1368, kFlagAllowRenderTarget), out);
    CHECK_EQ_U64(rt_1440.v, rt_drs.v);

    // El shadow map de 4096 NO se normaliza: es fijo, y su clave lleva el
    // tamano real. A otra resolucion de pantalla sigue siendo el mismo.
    const DescKey sm_a = desc_key(
        tex2d(Format::D32_Float, 4096, 4096, kFlagAllowDepthStencil), out);
    const DescKey sm_b = desc_key(
        tex2d(Format::D32_Float, 4096, 4096, kFlagAllowDepthStencil), out4k);
    CHECK_EQ_U64(sm_a.v, sm_b.v);
    // Y uno de 2048 es OTRO recurso.
    const DescKey sm_2k = desc_key(
        tex2d(Format::D32_Float, 2048, 2048, kFlagAllowDepthStencil), out);
    CHECK(sm_a != sm_2k);

    // Cambiar formato, flags, mips o array cambia la clave.
    const ResourceDesc base = tex2d(Format::R16G16B16A16_Float, 1280, 720,
                                    kFlagAllowRenderTarget);
    CHECK(desc_key(base, out) !=
          desc_key(tex2d(Format::R8G8B8A8_Unorm, 1280, 720,
                         kFlagAllowRenderTarget), out));
    CHECK(desc_key(base, out) !=
          desc_key(tex2d(Format::R16G16B16A16_Float, 1280, 720,
                         kFlagAllowRenderTarget | kFlagAllowUnorderedAcc), out));
    CHECK(desc_key(base, out) !=
          desc_key(tex2d(Format::R16G16B16A16_Float, 1280, 720,
                         kFlagAllowRenderTarget, 2), out));

    // --- tamanos ---------------------------------------------------------
    // 4096x4096 D32 = 64 MB exactos, un mip.
    CHECK_EQ_U64(texture_bytes(Format::D32_Float, 4096, 4096, 1, 1, 1),
                 4096ull * 4096 * 4);
    // La cadena completa de mips de un 2D suma 4/3 del mip 0.
    const u64 full_chain = texture_bytes(Format::R8_Unorm, 1024, 1024, 1, 0, 1);
    CHECK(full_chain > 1024ull * 1024);
    CHECK(full_chain < 1024ull * 1024 * 4 / 3 + 1024);
    // BC7 es 1 byte por pixel; BC1 medio.
    CHECK_EQ_U64(texture_bytes(Format::BC7_Unorm, 1024, 1024, 1, 1, 1),
                 1024ull * 1024);
    CHECK_EQ_U64(texture_bytes(Format::BC1_Unorm, 1024, 1024, 1, 1, 1),
                 1024ull * 1024 / 2);
    // Un array de 6 cuesta 6 veces.
    CHECK_EQ_U64(texture_bytes(Format::R16_Float, 512, 512, 1, 1, 6),
                 512ull * 512 * 2 * 6);

    // --- PsoKey ----------------------------------------------------------
    const char vs[] = "bytecode-vs-largo-para-cruzar-el-bloque-de-32-bytes";
    const char ps[] = "bytecode-ps";
    StageBlob a[2] = {{vs, sizeof vs}, {ps, sizeof ps}};
    StageBlob swapped[2] = {{ps, sizeof ps}, {vs, sizeof vs}};
    const Format rts[1] = {Format::R16G16B16A16_Float};
    const PsoKey k1 = pso_key(a, 2, rts, 1, Format::D32_Float);
    CHECK_EQ_U64(k1.v, pso_key(a, 2, rts, 1, Format::D32_Float).v);
    CHECK(k1 != pso_key(swapped, 2, rts, 1, Format::D32_Float));
    const Format rts2[1] = {Format::R8G8B8A8_Unorm};
    CHECK(k1 != pso_key(a, 2, rts2, 1, Format::D32_Float));
    CHECK(k1 != pso_key(a, 2, rts, 1, Format::D16_Unorm));

    // --- sesgo de mip sobre una vista -------------------------------------
    // Es la parte del mip_bias que tiene todos los off-by-one.
    {
        u32 first = 0, levels = 12;
        mip_bias_view(12, 1, &first, &levels);
        CHECK_EQ_U64(first, 1);
        CHECK_EQ_U64(levels, 11);

        // Sesgo mas grande que la cadena: queda el ultimo mip, nunca cero.
        first = 0; levels = 4;
        mip_bias_view(4, 10, &first, &levels);
        CHECK_EQ_U64(first, 3);
        CHECK_EQ_U64(levels, 1);

        // "Todos los que queden" se respeta tal cual.
        first = 0; levels = kAllMips;
        mip_bias_view(8, 2, &first, &levels);
        CHECK_EQ_U64(first, 2);
        CHECK_EQ_U64(levels, kAllMips);

        // Una vista que ya empezaba abajo se corre igual.
        first = 3; levels = 5;
        mip_bias_view(10, 2, &first, &levels);
        CHECK_EQ_U64(first, 5);
        CHECK_EQ_U64(levels, 5);

        // Sesgo negativo (mas detalle): no baja de cero.
        first = 1; levels = 3;
        mip_bias_view(10, -4, &first, &levels);
        CHECK_EQ_U64(first, 0);
        CHECK_EQ_U64(levels, 3);

        // Un recurso de un solo mip no se puede sesgar a ningun lado.
        first = 0; levels = 1;
        mip_bias_view(1, 3, &first, &levels);
        CHECK_EQ_U64(first, 0);
        CHECK_EQ_U64(levels, 1);
    }

    return gp_test::report("test_keys");
}
