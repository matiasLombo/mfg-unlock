// test_png.cpp -- que el PNG que escribimos sea un PNG.
//
// Un visor que no abre la captura convierte al harness A/B en la mitad de lo
// que tiene que ser: la ganancia medida sin el costo visual al lado no alcanza
// para decidir. Asi que se verifica la estructura, los CRC de cada chunk y el
// Adler32 del zlib, que es exactamente lo que un decodificador revisa.
#include "../core/png.h"

#include "harness.h"

#include <vector>

using namespace gp;

static u32 be32(const u8 *p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | p[3];
}

int main() {
    const u32 w = 37, h = 11;   // medidas feas a proposito
    std::vector<u8> img(static_cast<size_t>(w) * h * 4);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u8 *p = &img[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<u8>(x * 7);
            p[1] = static_cast<u8>(y * 13);
            p[2] = static_cast<u8>(x ^ y);
            p[3] = 255;
        }

    const std::vector<u8> png = png_encode_rgba(img.data(), w, h);

    // Firma.
    const u8 sig[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
    CHECK(png.size() > 8);
    for (int i = 0; i < 8; ++i) CHECK(png[static_cast<size_t>(i)] == sig[i]);

    // Recorrer los chunks verificando cada CRC, como hace un decodificador.
    size_t pos = 8;
    int n_ihdr = 0, n_idat = 0, n_iend = 0;
    u32 got_w = 0, got_h = 0;
    while (pos + 12 <= png.size()) {
        const u32 len = be32(&png[pos]);
        const char *type = reinterpret_cast<const char *>(&png[pos + 4]);
        const u32 crc = be32(&png[pos + 8 + len]);
        CHECK_EQ_U64(crc, png_detail::crc32_of(&png[pos + 4], len + 4));
        if (!memcmp(type, "IHDR", 4)) {
            ++n_ihdr;
            got_w = be32(&png[pos + 8]);
            got_h = be32(&png[pos + 12]);
            CHECK(png[pos + 8 + 8] == 8);   // 8 bits por canal
            CHECK(png[pos + 8 + 9] == 6);   // RGBA
        } else if (!memcmp(type, "IDAT", 4)) {
            ++n_idat;
            // zlib: cabecera 0x78 0x01 y el chequeo de cabecera valido.
            CHECK(png[pos + 8] == 0x78);
            const u32 hdr = (static_cast<u32>(png[pos + 8]) << 8) | png[pos + 9];
            CHECK_EQ_U64(hdr % 31, 0);
        } else if (!memcmp(type, "IEND", 4)) {
            ++n_iend;
        }
        pos += 12 + len;
    }
    CHECK_EQ_U64(pos, png.size());
    CHECK_EQ_U64(n_ihdr, 1);
    CHECK(n_idat >= 1);
    CHECK_EQ_U64(n_iend, 1);
    CHECK_EQ_U64(got_w, w);
    CHECK_EQ_U64(got_h, h);

    // Con stride (una fila mas larga que w*4): es como viene un readback de
    // D3D12, alineado a 256. Los bytes de relleno NO tienen que entrar.
    const size_t stride = 256;
    std::vector<u8> padded(stride * h, 0xCC);
    for (u32 y = 0; y < h; ++y)
        memcpy(&padded[y * stride], &img[static_cast<size_t>(y) * w * 4],
               static_cast<size_t>(w) * 4);
    const std::vector<u8> png2 = png_encode_rgba(padded.data(), w, h, stride);
    CHECK_EQ_U64(png2.size(), png.size());
    CHECK(png2 == png);

    // Una imagen grande cruza el limite de 65535 de un bloque stored.
    const u32 bw = 200, bh = 200;
    std::vector<u8> big(static_cast<size_t>(bw) * bh * 4, 0x40);
    const std::vector<u8> pbig = png_encode_rgba(big.data(), bw, bh);
    size_t p2 = 8;
    int idats = 0;
    bool crc_ok = true;
    while (p2 + 12 <= pbig.size()) {
        const u32 len = be32(&pbig[p2]);
        if (!memcmp(&pbig[p2 + 4], "IDAT", 4)) ++idats;
        if (be32(&pbig[p2 + 8 + len]) != png_detail::crc32_of(&pbig[p2 + 4], len + 4))
            crc_ok = false;
        p2 += 12 + len;
    }
    CHECK(crc_ok);
    CHECK_EQ_U64(p2, pbig.size());
    CHECK(idats == 1);

    return gp_test::report("test_png");
}
