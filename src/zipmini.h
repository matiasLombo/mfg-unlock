// Lector de ZIP minimo: directorio central + inflate (RFC 1951).
//
// Existe porque el toolchain no trae zlib y el dll no puede depender de nada
// que el usuario tenga que instalar. Solo se necesita LEER: extraer unos pocos
// .dll de bin/x64 del SDK de Streamline, una vez, a nuestra carpeta.
//
// Se valida contra Python antes de usarse en el dll (tools/test_zipmini.cpp):
// mismo tamano y mismo SHA-256 en las 16 entradas de bin/x64. Un inflate mal
// hecho no falla ruidosamente, entrega bytes plausibles -- por eso la
// comparacion es byte a byte y no "parece un PE".
//
// No soporta: zip64, cifrado, ni metodos que no sean 0 (store) y 8 (deflate).
// El SDK usa 8 en todas las entradas y 0 en ninguna, pero store se implementa
// igual porque es trivial y evita un caso raro.
#pragma once

#include <cstdint>
#include <cstring>

namespace zipmini {

// ---------------------------------------------------------------- inflate

struct BitReader {
    const uint8_t *p, *fin;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool malo = false;

    BitReader(const uint8_t *d, size_t n) : p(d), fin(d + n) {}

    int bits(int n) {
        while (bitcnt < n) {
            if (p >= fin) { malo = true; return 0; }
            bitbuf |= (uint32_t)(*p++) << bitcnt;
            bitcnt += 8;
        }
        const int v = (int)(bitbuf & ((1u << n) - 1));
        bitbuf >>= n;
        bitcnt -= n;
        return v;
    }
    void align() { bitbuf = 0; bitcnt = 0; }
};

// Arbol de Huffman canonico, en la forma "conteos + simbolos" del RFC.
struct Huff {
    uint16_t count[16];
    uint16_t simbolo[288];

    void build(const uint8_t *lengths, int n) {
        for (int i = 0; i < 16; ++i) count[i] = 0;
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;
        uint16_t offs[16];
        offs[0] = 0; offs[1] = 0;
        for (int i = 1; i < 15; ++i) offs[i + 1] = (uint16_t)(offs[i] + count[i]);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) simbolo[offs[lengths[i]]++] = (uint16_t)i;
    }

    int decode(BitReader &br) const {
        int codigo = 0, first = 0, indice = 0;
        for (int len = 1; len <= 15; ++len) {
            codigo |= br.bits(1);
            const int c = count[len];
            if (codigo - c < first) return simbolo[indice + (codigo - first)];
            indice += c;
            first = (first + c) << 1;
            codigo <<= 1;
        }
        return -1;
    }
};

// Escribe en `salida` hasta `cap` bytes. Devuelve cuantos escribio, o -1.
inline long inflate(const uint8_t *src, size_t nsrc, uint8_t *output, size_t cap) {
    static const uint16_t klen[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const uint8_t kelen[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const uint16_t kdist[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
        4097,6145,8193,12289,16385,24577 };
    static const uint8_t kedist[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

    BitReader br(src, nsrc);
    size_t out = 0;
    for (;;) {
        const int last = br.bits(1);
        const int tipo = br.bits(2);
        if (br.malo) return -1;

        if (tipo == 0) {                       // sin comprimir
            br.align();
            if (br.p + 4 > br.fin) return -1;
            const unsigned n = (unsigned)br.p[0] | ((unsigned)br.p[1] << 8);
            br.p += 4;                          // LEN + NLEN
            if (br.p + n > br.fin || out + n > cap) return -1;
            memcpy(output + out, br.p, n);
            br.p += n; out += n;
        } else if (tipo == 1 || tipo == 2) {
            Huff hl, hd;
            if (tipo == 1) {                    // arboles fijos
                uint8_t l[288];
                int i = 0;
                for (; i < 144; ++i) l[i] = 8;
                for (; i < 256; ++i) l[i] = 9;
                for (; i < 280; ++i) l[i] = 7;
                for (; i < 288; ++i) l[i] = 8;
                hl.build(l, 288);
                uint8_t d[30];
                for (i = 0; i < 30; ++i) d[i] = 5;
                hd.build(d, 30);
            } else {                            // arboles dinamicos
                static const uint8_t orden[19] = {
                    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
                const int hlit = br.bits(5) + 257;
                const int hdist = br.bits(5) + 1;
                const int hclen = br.bits(4) + 4;
                if (br.malo || hlit > 286 || hdist > 30) return -1;
                uint8_t lc[19];
                for (int i = 0; i < 19; ++i) lc[i] = 0;
                for (int i = 0; i < hclen; ++i) lc[orden[i]] = (uint8_t)br.bits(3);
                Huff hc; hc.build(lc, 19);
                uint8_t l[288 + 30];
                int n = 0;
                while (n < hlit + hdist) {
                    const int s = hc.decode(br);
                    if (s < 0 || br.malo) return -1;
                    if (s < 16) { l[n++] = (uint8_t)s; continue; }
                    int rep = 0; uint8_t v = 0;
                    if (s == 16) {
                        if (n == 0) return -1;
                        v = l[n - 1]; rep = 3 + br.bits(2);
                    } else if (s == 17) {
                        rep = 3 + br.bits(3);
                    } else {
                        rep = 11 + br.bits(7);
                    }
                    if (n + rep > hlit + hdist) return -1;
                    while (rep-- > 0) l[n++] = v;
                }
                hl.build(l, hlit);
                hd.build(l + hlit, hdist);
            }

            for (;;) {
                const int s = hl.decode(br);
                if (s < 0 || br.malo) return -1;
                if (s < 256) {
                    if (out >= cap) return -1;
                    output[out++] = (uint8_t)s;
                } else if (s == 256) {
                    break;
                } else {
                    const int i = s - 257;
                    if (i >= 29) return -1;
                    const int length = klen[i] + br.bits(kelen[i]);
                    const int ds = hd.decode(br);
                    if (ds < 0 || ds >= 30 || br.malo) return -1;
                    const size_t dist = (size_t)kdist[ds] + (size_t)br.bits(kedist[ds]);
                    if (dist > out || out + (size_t)length > cap) return -1;
                    // Se copia byte a byte a proposito: los tramos solapados
                    // (dist < largo) son legales en deflate y memcpy los rompe.
                    size_t desde = out - dist;
                    for (int k = 0; k < length; ++k) output[out++] = output[desde++];
                }
            }
        } else {
            return -1;                          // tipo 3 = reservado
        }
        if (last) break;
    }
    return (long)out;
}

// ---------------------------------------------------------------- zip

struct Entry {
    const char *name;     // apunta adentro del buffer del zip
    uint16_t name_len;
    uint16_t metodo;
    uint32_t compressed;
    uint32_t crudo;
    uint32_t offset_local;  // al encabezado local, no a los datos
};

inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Busca el End Of Central Directory, que esta al final salvo comentario.
inline const uint8_t *find_eocd(const uint8_t *d, size_t n) {
    if (n < 22) return nullptr;
    const size_t cap = n > 66000 ? 66000 : n;
    for (size_t i = 22; i <= cap; ++i) {
        const uint8_t *p = d + n - i;
        if (le32(p) == 0x06054b50) return p;
    }
    return nullptr;
}

// Recorre el directorio central llamando a `fn(entrada)`. Devuelve cuantas vio.
template <typename F>
inline int walk(const uint8_t *d, size_t n, F fn) {
    const uint8_t *eocd = find_eocd(d, n);
    if (eocd == nullptr) return -1;
    const uint32_t total = le16(eocd + 10);
    const uint32_t off = le32(eocd + 16);
    if (off >= n) return -1;
    const uint8_t *p = d + off;
    int seen = 0;
    for (uint32_t i = 0; i < total; ++i) {
        if (p + 46 > d + n || le32(p) != 0x02014b50) break;
        Entry e;
        e.metodo = le16(p + 10);
        e.compressed = le32(p + 20);
        e.crudo = le32(p + 24);
        e.name_len = le16(p + 28);
        const uint16_t extra = le16(p + 30);
        const uint16_t coment = le16(p + 32);
        e.offset_local = le32(p + 42);
        e.name = (const char *)(p + 46);
        if ((const uint8_t *)e.name + e.name_len > d + n) break;
        fn(e);
        ++seen;
        p += 46 + e.name_len + extra + coment;
    }
    return seen;
}

// Extrae una entrada al buffer dado. Devuelve bytes escritos, o -1.
inline long extract(const uint8_t *d, size_t n, const Entry &e,
                    uint8_t *output, size_t cap) {
    if (e.offset_local + 30 > n) return -1;
    const uint8_t *lh = d + e.offset_local;
    if (le32(lh) != 0x04034b50) return -1;
    // Los largos del encabezado LOCAL, no los del central: pueden diferir.
    const uint16_t nlen = le16(lh + 26);
    const uint16_t elen = le16(lh + 28);
    const uint8_t *datos = lh + 30 + nlen + elen;
    if (datos + e.compressed > d + n) return -1;
    if (e.crudo > cap) return -1;
    if (e.metodo == 0) {
        if (e.compressed != e.crudo) return -1;
        memcpy(output, datos, e.crudo);
        return (long)e.crudo;
    }
    if (e.metodo != 8) return -1;
    const long got = inflate(datos, e.compressed, output, cap);
    if (got < 0 || (uint32_t)got != e.crudo) return -1;
    return got;
}

}  // namespace zipmini
