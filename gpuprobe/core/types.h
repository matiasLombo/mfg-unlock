// types.h -- los tipos que cruzan todas las capas. Puro: no incluye d3d12.h ni
// windows.h, compila con g++ en Linux, y por eso el modelo de frame se puede
// testear en CI sin GPU.
//
// ENTRA: nada.
// SALE: los enteros de ancho fijo, Format (los valores SON los de DXGI_FORMAT,
//   asi que la capa de Windows castea sin traducir), ResFlags, HeapKind, y las
//   funciones puras sobre formato (bits por pixel, es depth, nombre, bytes de
//   una textura con sus mips).
// DEPENDE DE: <cstdint>, <cstddef>.
//
// Regla: si un valor numerico de este header coincide con uno de la API de
// Windows, el comentario lo dice. Que coincida es deliberado -- no queremos una
// tabla de traduccion que haya que mantener en dos lados.
#pragma once

#include <cstdint>
#include <cstddef>

namespace gp {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// Los valores son los de DXGI_FORMAT tal cual. Solo estan los que aparecen en
// recursos que nos interesan; cualquier otro llega como numero crudo y las
// funciones de abajo lo tratan como desconocido (bpp 0), que es visible en el
// JSONL en vez de silencioso.
enum class Format : u32 {
    Unknown              = 0,
    R32G32B32A32_Float   = 2,
    R32G32B32A32_Uint    = 3,
    R32G32B32_Float      = 6,
    R16G16B16A16_Float   = 10,
    R16G16B16A16_Unorm   = 11,
    R32G32_Float         = 16,
    R32G8X24_Typeless    = 19,
    D32_Float_S8X24_Uint = 20,
    R10G10B10A2_Unorm    = 24,
    R11G11B10_Float      = 26,
    R8G8B8A8_Typeless    = 27,
    R8G8B8A8_Unorm       = 28,
    R8G8B8A8_Unorm_Srgb  = 29,
    R16G16_Float         = 34,
    R16G16_Unorm         = 35,
    R32_Typeless         = 39,
    D32_Float            = 40,
    R32_Float            = 41,
    R32_Uint             = 42,
    R24G8_Typeless       = 44,
    D24_Unorm_S8_Uint    = 45,
    R24_Unorm_X8_Typeless= 46,
    R8G8_Unorm           = 49,
    R16_Typeless         = 53,
    R16_Float            = 54,
    D16_Unorm            = 55,
    R16_Unorm            = 56,
    R16_Uint             = 57,
    R8_Unorm             = 61,
    R8_Uint              = 62,
    A8_Unorm             = 65,
    R9G9B9E5_Sharedexp   = 67,
    BC1_Unorm            = 71,
    BC1_Unorm_Srgb       = 72,
    BC3_Unorm            = 77,
    BC3_Unorm_Srgb       = 78,
    BC4_Unorm            = 80,
    BC5_Unorm            = 83,
    B8G8R8A8_Unorm       = 87,
    B8G8R8A8_Unorm_Srgb  = 91,
    BC6H_Uf16            = 95,
    BC7_Unorm            = 98,
    BC7_Unorm_Srgb       = 99,
};

// Los bits son los de D3D12_RESOURCE_FLAGS. NoFlags = 0.
enum ResFlags : u32 {
    kFlagNone              = 0,
    kFlagAllowRenderTarget = 0x1,
    kFlagAllowDepthStencil = 0x2,
    kFlagAllowUnorderedAcc = 0x4,
    kFlagDenyShaderResource= 0x8,
    kFlagAllowCrossAdapter = 0x10,
    kFlagAllowSimultaneous = 0x20,
};

// D3D12_HEAP_TYPE, con Placed/Reserved anadidos arriba del rango de la API
// porque nos importa distinguir como se creo el recurso, no solo donde vive.
enum class HeapKind : u32 {
    Unknown  = 0,
    Default  = 1,   // D3D12_HEAP_TYPE_DEFAULT
    Upload   = 2,   // D3D12_HEAP_TYPE_UPLOAD
    Readback = 3,   // D3D12_HEAP_TYPE_READBACK
    Custom   = 4,   // D3D12_HEAP_TYPE_CUSTOM
    Placed   = 16,  // CreatePlacedResource: comparte heap con otros
    Reserved = 17,  // CreateReservedResource: tiled
};

// D3D12_RESOURCE_DIMENSION.
enum class Dim : u32 {
    Unknown   = 0,
    Buffer    = 1,
    Texture1D = 2,
    Texture2D = 3,
    Texture3D = 4,
};

// Los estados que nos importan para detectar barriers redundantes. Los valores
// son los de D3D12_RESOURCE_STATES; COMMON es 0 y por eso es indistinguible de
// "sin estado" en la API -- el colector nunca infiere COMMON, solo lo registra
// cuando la llamada lo dice.
enum ResState : u32 {
    kStateCommon            = 0,
    kStateVertexAndConstant = 0x1,
    kStateIndex             = 0x2,
    kStateRenderTarget      = 0x4,
    kStateUnorderedAccess   = 0x8,
    kStateDepthWrite        = 0x10,
    kStateDepthRead         = 0x20,
    kStateNonPixelShader    = 0x40,
    kStatePixelShader       = 0x80,
    kStateCopyDest          = 0x400,
    kStateCopySource        = 0x800,
    kStateResolveDest       = 0x1000,
    kStateResolveSource     = 0x2000,
};

// GENERIC_READ es una mascara, no un bit: 0x1|0x2|0x40|0x80|0x200|0x800.
inline constexpr u32 kStateGenericReadV = 0x1 | 0x2 | 0x40 | 0x80 | 0x200 | 0x800;

// ---- funciones puras sobre formato -------------------------------------

inline bool format_is_block_compressed(Format f) {
    const u32 v = static_cast<u32>(f);
    return (v >= 70 && v <= 84) || (v >= 94 && v <= 99);
}

inline bool format_is_depth(Format f) {
    switch (f) {
        case Format::D16_Unorm:
        case Format::D32_Float:
        case Format::D24_Unorm_S8_Uint:
        case Format::D32_Float_S8X24_Uint:
        // Los typeless que solo se usan como depth cuentan: un shadow map se
        // crea R32_TYPELESS y se ve como DSV + SRV. Sin esto no lo detectamos.
        case Format::R32_Typeless:
        case Format::R24G8_Typeless:
        case Format::R32G8X24_Typeless:
        case Format::R16_Typeless:
            return true;
        default:
            return false;
    }
}

// Bits por pixel. Para los comprimidos por bloque devuelve los bits por pixel
// equivalentes (BC1 = 4, BC7 = 8), que es lo que hace falta para el tamano.
inline u32 format_bits_per_pixel(Format f) {
    switch (f) {
        case Format::R32G32B32A32_Float:
        case Format::R32G32B32A32_Uint:   return 128;
        case Format::R32G32B32_Float:     return 96;
        case Format::R16G16B16A16_Float:
        case Format::R16G16B16A16_Unorm:
        case Format::R32G32_Float:
        case Format::R32G8X24_Typeless:
        case Format::D32_Float_S8X24_Uint: return 64;
        case Format::R10G10B10A2_Unorm:
        case Format::R11G11B10_Float:
        case Format::R8G8B8A8_Typeless:
        case Format::R8G8B8A8_Unorm:
        case Format::R8G8B8A8_Unorm_Srgb:
        case Format::R16G16_Float:
        case Format::R16G16_Unorm:
        case Format::R32_Typeless:
        case Format::D32_Float:
        case Format::R32_Float:
        case Format::R32_Uint:
        case Format::R24G8_Typeless:
        case Format::D24_Unorm_S8_Uint:
        case Format::R24_Unorm_X8_Typeless:
        case Format::R9G9B9E5_Sharedexp:
        case Format::B8G8R8A8_Unorm:
        case Format::B8G8R8A8_Unorm_Srgb: return 32;
        case Format::R8G8_Unorm:
        case Format::R16_Typeless:
        case Format::R16_Float:
        case Format::D16_Unorm:
        case Format::R16_Unorm:
        case Format::R16_Uint:            return 16;
        case Format::R8_Unorm:
        case Format::R8_Uint:
        case Format::A8_Unorm:            return 8;
        // Comprimidos: 8 bytes por bloque de 4x4 (BC1, BC4) son 4 bpp; 16
        // bytes por bloque (el resto) son 8 bpp.
        case Format::BC1_Unorm:
        case Format::BC1_Unorm_Srgb:
        case Format::BC4_Unorm:           return 4;
        case Format::BC3_Unorm:
        case Format::BC3_Unorm_Srgb:
        case Format::BC5_Unorm:
        case Format::BC6H_Uf16:
        case Format::BC7_Unorm:
        case Format::BC7_Unorm_Srgb:      return 8;
        default:                          return 0;  // desconocido: visible
    }
}

// Bytes de una subresource (un mip) contando el padding de bloque de los
// comprimidos. No cuenta el alineado de 512/64 KB que pide D3D12 al colocar
// un recurso: eso lo sabe el device (GetResourceAllocationInfo) y la capa de
// Windows lo pisa con el valor real cuando lo tiene. Este es el piso.
inline u64 mip_bytes(Format f, u32 w, u32 h, u32 depth) {
    const u32 bpp = format_bits_per_pixel(f);
    if (bpp == 0) return 0;
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    if (depth == 0) depth = 1;
    if (format_is_block_compressed(f)) {
        const u64 bw = (w + 3u) / 4u, bh = (h + 3u) / 4u;
        const u64 block_bytes = (bpp == 4) ? 8u : 16u;
        return bw * bh * block_bytes * depth;
    }
    return (static_cast<u64>(w) * h * depth * bpp) / 8u;
}

// Bytes de la textura entera: todos los mips, todas las slices del array.
// mips == 0 significa "la cadena completa", igual que en D3D12.
inline u64 texture_bytes(Format f, u32 w, u32 h, u32 depth, u32 mips, u32 array) {
    if (array == 0) array = 1;
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    if (depth == 0) depth = 1;
    u32 full = 1;
    for (u32 d = (w > h ? w : h); d > 1; d >>= 1) ++full;
    if (mips == 0 || mips > full) mips = full;
    u64 total = 0;
    for (u32 m = 0; m < mips; ++m) {
        const u32 mw = w >> m ? w >> m : 1;
        const u32 mh = h >> m ? h >> m : 1;
        const u32 md = depth >> m ? depth >> m : 1;
        total += mip_bytes(f, mw, mh, md);
    }
    return total * array;
}

// Nombre corto para el JSONL y el overlay. Desconocido sale como "fmt<N>" con
// el numero crudo, que es lo que despues permite agregarlo a la tabla.
const char *format_name(Format f);

inline bool flags_has(u32 flags, u32 bit) { return (flags & bit) != 0; }

}  // namespace gp
