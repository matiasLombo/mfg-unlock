// hash.h -- XXH64, escrito acá y no linkeado, porque una de las restricciones
// del proyecto es no tener dependencias de runtime.
//
// ENTRA: bytes.
// SALE: hash_bytes / hash_combine / hash_str, deterministas entre corridas y
//   entre maquinas (nada de punteros ni de ASLR adentro).
// DEPENDE DE: types.h.
//
// Por que XXH64 y no algo mas corto: el hash del PSO se saca del bytecode de
// cada etapa, que son cientos de KB, y tiene que ser estable entre corridas
// para poder comparar dos sesiones del mismo juego. Un FNV sobre 300 KB es
// lento y colisiona mas de lo que queremos para una clave que despues agrupa
// pasadas.
#pragma once

#include "types.h"

#include <cstring>

namespace gp {

namespace detail {

inline constexpr u64 kP1 = 0x9E3779B185EBCA87ull;
inline constexpr u64 kP2 = 0xC2B2AE3D27D4EB4Full;
inline constexpr u64 kP3 = 0x165667B19E3779F9ull;
inline constexpr u64 kP4 = 0x85EBCA77C2B2AE63ull;
inline constexpr u64 kP5 = 0x27D4EB2F165667C5ull;

inline u64 rotl64(u64 x, int r) { return (x << r) | (x >> (64 - r)); }

inline u64 read64(const u8 *p) { u64 v; std::memcpy(&v, p, 8); return v; }
inline u32 read32(const u8 *p) { u32 v; std::memcpy(&v, p, 4); return v; }

inline u64 round(u64 acc, u64 val) {
    acc += val * kP2;
    acc = rotl64(acc, 31);
    acc *= kP1;
    return acc;
}

inline u64 merge_round(u64 acc, u64 val) {
    val = round(0, val);
    acc ^= val;
    acc = acc * kP1 + kP4;
    return acc;
}

}  // namespace detail

// XXH64. Little-endian: en x64 y en el CI de Linux es lo mismo, y no corremos
// en otra cosa. Si algun dia corriera en big-endian, los hashes cambiarian y
// las sesiones viejas dejarian de comparar -- por eso el JSONL lleva version.
inline u64 hash_bytes(const void *data, size_t len, u64 seed = 0) {
    const u8 *p = static_cast<const u8 *>(data);
    const u8 *const end = p + len;
    u64 h;

    if (len >= 32) {
        const u8 *const limit = end - 32;
        u64 v1 = seed + detail::kP1 + detail::kP2;
        u64 v2 = seed + detail::kP2;
        u64 v3 = seed;
        u64 v4 = seed - detail::kP1;
        do {
            v1 = detail::round(v1, detail::read64(p)); p += 8;
            v2 = detail::round(v2, detail::read64(p)); p += 8;
            v3 = detail::round(v3, detail::read64(p)); p += 8;
            v4 = detail::round(v4, detail::read64(p)); p += 8;
        } while (p <= limit);
        h = detail::rotl64(v1, 1) + detail::rotl64(v2, 7) +
            detail::rotl64(v3, 12) + detail::rotl64(v4, 18);
        h = detail::merge_round(h, v1);
        h = detail::merge_round(h, v2);
        h = detail::merge_round(h, v3);
        h = detail::merge_round(h, v4);
    } else {
        h = seed + detail::kP5;
    }

    h += static_cast<u64>(len);

    while (p + 8 <= end) {
        h ^= detail::round(0, detail::read64(p));
        h = detail::rotl64(h, 27) * detail::kP1 + detail::kP4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= static_cast<u64>(detail::read32(p)) * detail::kP1;
        h = detail::rotl64(h, 23) * detail::kP2 + detail::kP3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<u64>(*p) * detail::kP5;
        h = detail::rotl64(h, 11) * detail::kP1;
        ++p;
    }

    h ^= h >> 33;
    h *= detail::kP2;
    h ^= h >> 29;
    h *= detail::kP3;
    h ^= h >> 32;
    return h;
}

// Encadenar campos: el hash anterior entra como semilla. El orden importa, y
// eso es deliberado -- dos descriptores con los mismos numeros en distinto
// campo tienen que dar claves distintas.
inline u64 hash_combine(u64 seed, u64 value) {
    return hash_bytes(&value, sizeof value, seed);
}

inline u64 hash_str(const char *s, u64 seed = 0) {
    return hash_bytes(s, s ? std::strlen(s) : 0, seed);
}

}  // namespace gp
