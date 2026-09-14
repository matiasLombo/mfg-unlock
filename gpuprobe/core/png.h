// png.h -- escritor de PNG minimo, sin dependencias.
//
// ENTRA: pixeles RGBA de 8 bits por canal.
// SALE: un PNG valido en memoria.
// DEPENDE DE: nada (ni zlib, ni libpng, ni stb).
//
// Existe porque el harness A/B tiene que dejar la MISMA escena con y sin la
// optimizacion para poder evaluar el costo visual mirando, y porque una de las
// restricciones del proyecto es no tener dependencias de runtime. Un BMP
// hubiera sido mas corto, pero una captura de 4K en BMP son 33 MB y nadie abre
// eso dos veces.
//
// El deflate es de bloques "stored" (sin comprimir, tipo 0): un PNG valido y
// legible por cualquier visor, a cambio de un archivo mas grande. Comprimir de
// verdad seria meter un deflate completo acá para ahorrar disco en una captura
// que se mira una vez -- no vale. Lo que si importa es que el CRC y el Adler
// esten bien, porque un PNG con checksum malo no lo abre nadie.
#pragma once

#include "types.h"

#include <cstdio>
#include <vector>

namespace gp {

namespace png_detail {

inline u32 crc32_of(const u8 *data, size_t len, u32 crc = 0) {
    static u32 table[256];
    static bool ready = false;
    if (!ready) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    crc = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

inline void put32(std::vector<u8> &out, u32 v) {
    out.push_back(static_cast<u8>(v >> 24));
    out.push_back(static_cast<u8>(v >> 16));
    out.push_back(static_cast<u8>(v >> 8));
    out.push_back(static_cast<u8>(v));
}

inline void chunk(std::vector<u8> &out, const char type[4], const u8 *data,
                  size_t len) {
    put32(out, static_cast<u32>(len));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (len) out.insert(out.end(), data, data + len);
    put32(out, crc32_of(out.data() + start, out.size() - start));
}

}  // namespace png_detail

// rgba: w*h*4 bytes. stride es el largo de una fila en bytes (0 = w*4), que es
// lo que hace falta para volcar directo un readback de D3D12, donde las filas
// vienen alineadas a 256.
inline std::vector<u8> png_encode_rgba(const u8 *rgba, u32 w, u32 h,
                                       size_t stride = 0) {
    using namespace png_detail;
    if (stride == 0) stride = static_cast<size_t>(w) * 4;

    std::vector<u8> out;
    const u8 sig[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
    out.insert(out.end(), sig, sig + 8);

    u8 ihdr[13];
    ihdr[0] = static_cast<u8>(w >> 24); ihdr[1] = static_cast<u8>(w >> 16);
    ihdr[2] = static_cast<u8>(w >> 8);  ihdr[3] = static_cast<u8>(w);
    ihdr[4] = static_cast<u8>(h >> 24); ihdr[5] = static_cast<u8>(h >> 16);
    ihdr[6] = static_cast<u8>(h >> 8);  ihdr[7] = static_cast<u8>(h);
    ihdr[8] = 8;   // bits por canal
    ihdr[9] = 6;   // RGBA
    ihdr[10] = 0;  // deflate
    ihdr[11] = 0;  // filtro adaptativo
    ihdr[12] = 0;  // sin entrelazado
    chunk(out, "IHDR", ihdr, sizeof ihdr);

    // Los datos crudos: cada fila lleva adelante un byte de filtro (0 = none).
    std::vector<u8> raw;
    raw.reserve(static_cast<size_t>(h) * (static_cast<size_t>(w) * 4 + 1));
    for (u32 y = 0; y < h; ++y) {
        raw.push_back(0);
        const u8 *row = rgba + static_cast<size_t>(y) * stride;
        raw.insert(raw.end(), row, row + static_cast<size_t>(w) * 4);
    }

    // zlib: cabecera, bloques stored de hasta 65535, y el Adler32 al final.
    std::vector<u8> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t n = raw.size() - pos > 65535 ? 65535 : raw.size() - pos;
        const bool last = (pos + n == raw.size());
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<u8>(n));
        z.push_back(static_cast<u8>(n >> 8));
        z.push_back(static_cast<u8>(~n));
        z.push_back(static_cast<u8>((~n) >> 8));
        z.insert(z.end(), raw.begin() + static_cast<long>(pos),
                 raw.begin() + static_cast<long>(pos + n));
        pos += n;
    }
    u32 a = 1, b = 0;
    for (u8 byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    const u32 adler = (b << 16) | a;
    z.push_back(static_cast<u8>(adler >> 24));
    z.push_back(static_cast<u8>(adler >> 16));
    z.push_back(static_cast<u8>(adler >> 8));
    z.push_back(static_cast<u8>(adler));

    chunk(out, "IDAT", z.data(), z.size());
    chunk(out, "IEND", nullptr, 0);
    return out;
}

// Volcado a disco. Devuelve false si no se pudo escribir; no lanza.
inline bool png_write(const char *path, const u8 *rgba, u32 w, u32 h,
                      size_t stride = 0) {
    const std::vector<u8> data = png_encode_rgba(rgba, w, h, stride);
    std::FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    const size_t n = std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return n == data.size();
}

}  // namespace gp
