// types.cpp -- la unica parte de types.h que no es inline: la tabla de nombres
// de formato. Esta aparte para que el header no meta un arreglo estatico en
// cada translation unit que lo incluya.
#include "types.h"

#include <cstdio>

namespace gp {

const char *format_name(Format f) {
    switch (f) {
        case Format::Unknown:               return "UNKNOWN";
        case Format::R32G32B32A32_Float:    return "R32G32B32A32_FLOAT";
        case Format::R32G32B32A32_Uint:     return "R32G32B32A32_UINT";
        case Format::R32G32B32_Float:       return "R32G32B32_FLOAT";
        case Format::R16G16B16A16_Float:    return "R16G16B16A16_FLOAT";
        case Format::R16G16B16A16_Unorm:    return "R16G16B16A16_UNORM";
        case Format::R32G32_Float:          return "R32G32_FLOAT";
        case Format::R32G8X24_Typeless:     return "R32G8X24_TYPELESS";
        case Format::D32_Float_S8X24_Uint:  return "D32_FLOAT_S8X24_UINT";
        case Format::R10G10B10A2_Unorm:     return "R10G10B10A2_UNORM";
        case Format::R11G11B10_Float:       return "R11G11B10_FLOAT";
        case Format::R8G8B8A8_Typeless:     return "R8G8B8A8_TYPELESS";
        case Format::R8G8B8A8_Unorm:        return "R8G8B8A8_UNORM";
        case Format::R8G8B8A8_Unorm_Srgb:   return "R8G8B8A8_UNORM_SRGB";
        case Format::R16G16_Float:          return "R16G16_FLOAT";
        case Format::R16G16_Unorm:          return "R16G16_UNORM";
        case Format::R32_Typeless:          return "R32_TYPELESS";
        case Format::D32_Float:             return "D32_FLOAT";
        case Format::R32_Float:             return "R32_FLOAT";
        case Format::R32_Uint:              return "R32_UINT";
        case Format::R24G8_Typeless:        return "R24G8_TYPELESS";
        case Format::D24_Unorm_S8_Uint:     return "D24_UNORM_S8_UINT";
        case Format::R24_Unorm_X8_Typeless: return "R24_UNORM_X8_TYPELESS";
        case Format::R8G8_Unorm:            return "R8G8_UNORM";
        case Format::R16_Typeless:          return "R16_TYPELESS";
        case Format::R16_Float:             return "R16_FLOAT";
        case Format::D16_Unorm:             return "D16_UNORM";
        case Format::R16_Unorm:             return "R16_UNORM";
        case Format::R16_Uint:              return "R16_UINT";
        case Format::R8_Unorm:              return "R8_UNORM";
        case Format::R8_Uint:               return "R8_UINT";
        case Format::A8_Unorm:              return "A8_UNORM";
        case Format::R9G9B9E5_Sharedexp:    return "R9G9B9E5_SHAREDEXP";
        case Format::BC1_Unorm:             return "BC1_UNORM";
        case Format::BC1_Unorm_Srgb:        return "BC1_UNORM_SRGB";
        case Format::BC3_Unorm:             return "BC3_UNORM";
        case Format::BC3_Unorm_Srgb:        return "BC3_UNORM_SRGB";
        case Format::BC4_Unorm:             return "BC4_UNORM";
        case Format::BC5_Unorm:             return "BC5_UNORM";
        case Format::B8G8R8A8_Unorm:        return "B8G8R8A8_UNORM";
        case Format::B8G8R8A8_Unorm_Srgb:   return "B8G8R8A8_UNORM_SRGB";
        case Format::BC6H_Uf16:             return "BC6H_UF16";
        case Format::BC7_Unorm:             return "BC7_UNORM";
        case Format::BC7_Unorm_Srgb:        return "BC7_UNORM_SRGB";
    }
    // Un formato que no esta en la tabla no es un error: sale con su numero
    // para poder agregarlo despues. El buffer estatico por hilo evita alocar
    // en el camino del colector.
    static thread_local char scratch[24];
    std::snprintf(scratch, sizeof scratch, "fmt%u", static_cast<unsigned>(f));
    return scratch;
}

}  // namespace gp

// Los nombres de keys.h viven aca por la misma razon que los de formato: una
// sola copia de la tabla en todo el binario.
#include "keys.h"

namespace gp {

const char *scale_name(ScaleClass s) {
    switch (s) {
        case ScaleClass::Fixed:   return "fixed";
        case ScaleClass::Full:    return "full";
        case ScaleClass::Half:    return "half";
        case ScaleClass::Quarter: return "quarter";
        case ScaleClass::Eighth:  return "eighth";
        case ScaleClass::Larger:  return "larger";
    }
    return "?";
}

const char *category_name(Category c) {
    switch (c) {
        case Category::Unknown:     return "unknown";
        case Category::Buffer:      return "buffer";
        case Category::ShadowMap:   return "shadowmap";
        case Category::ShadowCube:  return "shadowcube";
        case Category::DepthBuffer: return "depthbuffer";
        case Category::CubeMap:     return "cubemap";
        case Category::Volume:      return "volume";
        case Category::RtFull:      return "rt_full";
        case Category::RtHalf:      return "rt_half";
        case Category::RtQuarter:   return "rt_quarter";
        case Category::RtOther:     return "rt_other";
        case Category::Texture:     return "texture";
        case Category::Staging:     return "staging";
    }
    return "?";
}

}  // namespace gp

#include "events.h"

namespace gp {

const char *event_kind_name(EventKind k) {
    switch (k) {
        case EventKind::None:            return "none";
        case EventKind::FrameBegin:      return "frame_begin";
        case EventKind::FrameEnd:        return "frame_end";
        case EventKind::ResourceCreate:  return "res";
        case EventKind::ResourceDestroy: return "res_free";
        case EventKind::Pso:             return "pso";
        case EventKind::Draw:            return "draw";
        case EventKind::Dispatch:        return "dispatch";
        case EventKind::Barrier:         return "barrier";
        case EventKind::PassBegin:       return "pass_begin";
        case EventKind::PassEnd:         return "pass_end";
        case EventKind::PassTiming:      return "pass";
        case EventKind::Vram:            return "vram";
        case EventKind::ActionApplied:   return "action";
    }
    return "?";
}

}  // namespace gp
