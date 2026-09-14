// keys.h -- las claves con las que se habla de un recurso, un PSO y una pasada,
// y la clasificacion de un descriptor en una categoria.
//
// ENTRA: descriptores (ResourceDesc) y la resolucion de salida.
// SALE: DescKey (clave estable por descriptor normalizado), PsoKey, PassKey,
//   RtSetKey, Category y ScaleClass.
// DEPENDE DE: types.h, hash.h.
//
// La decision de fondo del proyecto esta en este archivo: el ejecutor matchea
// por DESCRIPTOR, no por hash de shader. Un parche del juego recompila
// shaders (y mueve todos los hashes) mucho mas seguido de lo que cambia "el
// shadow map es un D32 de 4096 en array de 4". Por eso la clave que sobrevive
// updates se arma de los numeros del descriptor, normalizados contra la
// resolucion de salida, y el hash del bytecode queda solo para MEDIR.
#pragma once

#include "hash.h"
#include "types.h"

namespace gp {

struct ResourceDesc {
    Dim      dim    = Dim::Unknown;
    Format   fmt    = Format::Unknown;
    u32      w      = 0;
    u32      h      = 0;
    u32      depth  = 1;   // Texture3D: profundidad. 2D array: slices.
    u32      mips   = 1;
    u32      samples= 1;
    u32      flags  = kFlagNone;
    HeapKind heap   = HeapKind::Unknown;
    u64      bytes  = 0;   // el que dijo el device si lo sabemos; si no, el piso
    // Buffer del swapchain. Importa por dos cosas que serian errores caros:
    // el backbuffer se escribe y NUNCA se transiciona a un estado de lectura
    // (lo lee el compositor, no un shader), asi que la regla del recurso
    // huerfano lo marcaria a el en todos los juegos; y escalarlo no es una
    // optimizacion, es romper la presentacion.
    bool     swapchain = false;
};

// La resolucion a la que el juego presenta. Todo lo demas se mide contra esto:
// sin ella no se puede distinguir "RT full-res de post" de "textura de 1920
// que casualmente mide lo mismo".
struct OutputInfo {
    u32 w = 0;
    u32 h = 0;
};

// Escala respecto de la salida. Los cortes son anchos a proposito: los juegos
// redondean a multiplos de 8 y con DRS la salida se mueve sola.
enum class ScaleClass : u8 {
    Fixed = 0,   // no guarda relacion con la salida (sombras, LUTs, atlas)
    Full,        // >= 90% de la salida
    Half,        // ~50%
    Quarter,     // ~25%
    Eighth,      // ~12.5%
    Larger,      // mas grande que la salida (supersampling, atlas gigante)
};

inline ScaleClass scale_of(u32 dim_px, u32 out_px) {
    if (out_px == 0 || dim_px == 0) return ScaleClass::Fixed;
    const double r = static_cast<double>(dim_px) / static_cast<double>(out_px);
    if (r > 1.10) return ScaleClass::Larger;
    if (r >= 0.90) return ScaleClass::Full;
    if (r >= 0.45 && r <= 0.55) return ScaleClass::Half;
    if (r >= 0.22 && r <= 0.28) return ScaleClass::Quarter;
    if (r >= 0.11 && r <= 0.14) return ScaleClass::Eighth;
    return ScaleClass::Fixed;
}

const char *scale_name(ScaleClass);

// Categoria inferida. No hay campo en D3D12 que diga "esto es un shadow map":
// se infiere del descriptor, y por eso cada regla esta escrita para fallar
// hacia Unknown en vez de hacia una categoria equivocada. Un Unknown se ve en
// el reporte; un shadow map mal etiquetado se convierte en una accion que
// rompe el render.
enum class Category : u8 {
    Unknown = 0,
    Buffer,
    Backbuffer,     // buffer del swapchain: ni se escala ni se le pide lectura
    ShadowMap,      // depth, cuadrado, sin relacion con la salida
    ShadowCube,     // depth, cuadrado, array multiplo de 6
    DepthBuffer,    // depth a resolucion de salida
    CubeMap,        // color, cuadrado, array multiplo de 6
    Volume,         // Texture3D con RT/UAV: niebla volumetrica, LUT de cielo
    RtFull,
    RtHalf,
    RtQuarter,
    RtOther,
    Texture,        // asset: sin RT/DS, con mips, subido desde Upload
    Staging,        // Upload/Readback
};

const char *category_name(Category);

inline bool desc_is_render_target(const ResourceDesc &d) {
    return flags_has(d.flags, kFlagAllowRenderTarget);
}
inline bool desc_is_depth(const ResourceDesc &d) {
    return flags_has(d.flags, kFlagAllowDepthStencil);
}

inline Category classify(const ResourceDesc &d, const OutputInfo &out) {
    if (d.swapchain) return Category::Backbuffer;
    if (d.dim == Dim::Buffer) return Category::Buffer;
    if (d.heap == HeapKind::Upload || d.heap == HeapKind::Readback)
        return Category::Staging;

    const bool square = (d.w == d.h && d.w > 0);
    const ScaleClass sw = scale_of(d.w, out.w);
    const ScaleClass sh = scale_of(d.h, out.h);
    // Cubo: D3D12 no marca cubemaps en el recurso -- eso vive en la vista. Lo
    // unico que se puede ver desde aca es "array multiplo de 6 y cuadrado", y
    // con eso alcanza para las dos categorias que nos importan.
    const bool cubeish = square && d.depth >= 6 && (d.depth % 6) == 0;

    if (desc_is_depth(d)) {
        if (cubeish) return Category::ShadowCube;
        if (sw == ScaleClass::Full && sh == ScaleClass::Full)
            return Category::DepthBuffer;
        if (square) return Category::ShadowMap;
        // Depth que no es cuadrado ni de salida: un depth de otra vista (agua,
        // retrovisor). No es un shadow map y no queremos tratarlo como tal.
        return Category::Unknown;
    }

    if (d.dim == Dim::Texture3D &&
        (desc_is_render_target(d) || flags_has(d.flags, kFlagAllowUnorderedAcc)))
        return Category::Volume;

    if (desc_is_render_target(d) || flags_has(d.flags, kFlagAllowUnorderedAcc)) {
        if (cubeish) return Category::CubeMap;
        if (sw == ScaleClass::Full && sh == ScaleClass::Full)    return Category::RtFull;
        if (sw == ScaleClass::Half && sh == ScaleClass::Half)    return Category::RtHalf;
        if (sw == ScaleClass::Quarter && sh == ScaleClass::Quarter)
            return Category::RtQuarter;
        return Category::RtOther;
    }

    if (d.mips > 1) return Category::Texture;
    return Category::Unknown;
}

// --- claves ---------------------------------------------------------------
//
// Todas son u64 envueltos en un struct para que el compilador no deje pasar
// una clave de PSO donde va una de pasada. El costo es cero y el error que
// evita es de los que no se ven en el log.

#define GP_DEFINE_KEY(Name)                                                   \
    struct Name {                                                             \
        u64 v = 0;                                                            \
        constexpr bool operator==(const Name &o) const { return v == o.v; }   \
        constexpr bool operator!=(const Name &o) const { return v != o.v; }   \
        constexpr bool operator<(const Name &o) const { return v < o.v; }     \
        constexpr explicit operator bool() const { return v != 0; }           \
    }

GP_DEFINE_KEY(DescKey);   // descriptor normalizado: sobrevive updates
GP_DEFINE_KEY(ResKey);    // instancia viva: puntero + generacion
GP_DEFINE_KEY(PsoKey);    // bytecode de las etapas
GP_DEFINE_KEY(RtSetKey);  // conjunto de RTs bindeados
GP_DEFINE_KEY(PassKey);   // (RtSetKey, PsoKey del primer draw, ordinal)
GP_DEFINE_KEY(CallsiteId);

#undef GP_DEFINE_KEY

// Un recurso escala con la salida o no, y eso decide como entra en la clave.
// Los RTs y el depth de la escena escalan: su clave lleva la CLASE de escala,
// asi el perfil escrito a 1440p matchea a 2160p y no se rompe con DRS. Un
// shadow map, un cubemap o un atlas no escalan -- un D32 de 4096 es 4096 en
// toda pantalla -- y su clave lleva los numeros reales, que es justo lo que lo
// identifica. Normalizar un shadow map contra la salida seria peor que no
// normalizar nada: a 1440p daria "mas grande que la salida" y a 2160p
// "igual a la salida", o sea dos claves para el mismo recurso.
inline bool category_scales_with_output(Category c) {
    switch (c) {
        case Category::DepthBuffer:
        case Category::RtFull:
        case Category::RtHalf:
        case Category::RtQuarter:
        case Category::RtOther:
            return true;
        default:
            return false;
    }
}

inline DescKey desc_key(const ResourceDesc &d, const OutputInfo &out) {
    const Category cat = classify(d, out);
    const ScaleClass sw = scale_of(d.w, out.w);
    const ScaleClass sh = scale_of(d.h, out.h);
    // Escala solo si la categoria escala Y las dos dimensiones cayeron en una
    // clase reconocida: un RT de 800x450 en una salida de 2560x1440 no guarda
    // relacion con nada, y entra con sus numeros.
    const bool by_scale = category_scales_with_output(cat) &&
                          sw != ScaleClass::Fixed && sh != ScaleClass::Fixed;

    u64 h = hash_combine(0x6770726F6265ull /*"gprobe"*/, static_cast<u64>(d.dim));
    h = hash_combine(h, static_cast<u64>(d.fmt));
    h = hash_combine(h, static_cast<u64>(cat));
    if (by_scale) {
        h = hash_combine(h, static_cast<u64>(sw));
        h = hash_combine(h, static_cast<u64>(sh));
    } else {
        h = hash_combine(h, d.w);
        h = hash_combine(h, d.h);
    }
    h = hash_combine(h, d.depth);
    h = hash_combine(h, d.mips);
    h = hash_combine(h, d.samples);
    h = hash_combine(h, d.flags);
    h = hash_combine(h, static_cast<u64>(d.heap));
    return DescKey{h};
}

// Hash del PSO: el bytecode de cada etapa en orden, mas los formatos de salida.
// No entra ningun puntero. Dos corridas del mismo juego dan la misma clave;
// dos juegos con el mismo shader tambien, y eso esta bien.
struct StageBlob {
    const void *data = nullptr;
    size_t      size = 0;
};

inline PsoKey pso_key(const StageBlob *stages, size_t nstages,
                      const Format *rt_formats, size_t nrt, Format dsv) {
    u64 h = 0x70736F6B6579ull;  // "psokey"
    for (size_t i = 0; i < nstages; ++i) {
        h = hash_combine(h, i);
        if (stages[i].data && stages[i].size)
            h = hash_bytes(stages[i].data, stages[i].size, h);
        else
            h = hash_combine(h, 0);
    }
    for (size_t i = 0; i < nrt; ++i)
        h = hash_combine(h, static_cast<u64>(rt_formats[i]));
    h = hash_combine(h, static_cast<u64>(dsv));
    return PsoKey{h};
}

inline RtSetKey rt_set_key(const DescKey *rts, size_t nrt, DescKey dsv) {
    u64 h = 0x727473657400ull;  // "rtset"
    for (size_t i = 0; i < nrt; ++i) h = hash_combine(h, rts[i].v);
    h = hash_combine(h, dsv.v);
    return RtSetKey{h};
}

// El ancla de una pasada es (RTs, PSO del primer draw). El ordinal solo
// desempata cuando el mismo par aparece dos veces en el frame (dos cascadas de
// sombra son literalmente la misma pasada repetida), y por eso entra ultimo y
// se registra aparte: un cambio de ordinal entre dos frames no es un cambio de
// pasada, es que el juego salteo una.
inline PassKey pass_key(RtSetKey rts, PsoKey first_pso, u32 ordinal) {
    u64 h = hash_combine(0x7061737300ull /*"pass"*/, rts.v);
    h = hash_combine(h, first_pso.v);
    h = hash_combine(h, ordinal);
    return PassKey{h};
}

}  // namespace gp
