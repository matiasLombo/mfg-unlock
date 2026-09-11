// Los sitios de parche: DONDE se escribe en sl.dlss_g y en nvngx_dlssg, como
// busquedas puras sobre la seccion .text.
//
// EXTRACCION de los patch_* de proxy.cpp (2026-09-11): cada funcion de ahi
// buscaba un patron de bytes y escribia en el mismo bucle. Aca queda la
// busqueda, con los mismos bytes y la misma regla de unicidad; la escritura
// (VirtualProtect + los bytes nuevos) sigue en proxy.cpp, que es lo unico que
// necesita Windows.
//
// Por que importa: cuando NVIDIA suelta un build nuevo del snippet o del
// plugin, los sitios se mueven o desaparecen y el mod se apaga en silencio
// ([[mfg-ota-updates-break-patches]]). tools/test_sitios.cpp corre estas
// busquedas sobre los ARCHIVOS reales de la cache, sin juego, y dice cuantos
// sitios tiene cada uno: si un numero cambia, se sabe antes de abrir nada.
//
// Un patron es una lista de (mascara, valor) empaquetados en 16 bits:
// 0xFF00|v exacto, 0x0000 comodin, 0xF800|v los 5 bits altos. Los patrones
// con relaciones entre bytes (el registro del pacer, los saltos que tienen
// que coincidir) son funciones aparte.
#pragma once
#include <cstddef>

namespace sites {

typedef unsigned char u8;
typedef unsigned short pb;   // (mascara << 8) | valor

constexpr pb X(unsigned v) { return (pb)(0xFF00u | (v & 0xFF)); }   // exacto
constexpr pb W = 0;                                                  // comodin
constexpr pb M(unsigned mask, unsigned v) { return (pb)((mask << 8) | (v & 0xFF)); }

struct Pattern {
    const char *name;
    const pb *b;
    int n;
    int write_at;    // offset dentro del patron del primer byte que se escribe
};

inline bool matches(const u8 *t, const Pattern &p) {
    for (int k = 0; k < p.n; ++k) {
        const u8 m = (u8)(p.b[k] >> 8), v = (u8)(p.b[k] & 0xFF);
        if ((t[k] & m) != v) return false;
    }
    return true;
}

// Todas las apariciones. Devuelve cuantas hay (aunque no entren en `out`);
// `out` recibe el offset del PATRON (no del byte a escribir) de las primeras.
inline int find(const u8 *t, size_t len, const Pattern &p, size_t *out, int max) {
    int n = 0;
    if ((size_t)p.n > len) return 0;
    for (size_t i = 0; i + (size_t)p.n <= len; ++i) {
        if (!matches(t + i, p)) continue;
        if (n < max) out[n] = i;
        ++n;
    }
    return n;
}

// ---- sl.dlss_g (el plugin) ------------------------------------------------

// mov eax,[rdx+4]; mov r8d,0xC0; mov [rcx+4],eax -- la cuenta de work items.
// Unico. Se escribe push imm8 / pop rax sobre los 3 primeros bytes.
inline const pb kWorkItemCountBytes[] = { X(0x8B), X(0x42), X(0x04), X(0x41), X(0xB8), X(0xC0),
                                       X(0x00), X(0x00), X(0x00), X(0x89), X(0x41), X(0x04) };
inline const Pattern kWorkItemCount = { "cuenta-work-item", kWorkItemCountBytes, 12, 0 };
// Justo despues (offset +12) puede venir mov eax,[rdx+8]: el llenado.
inline const pb kFillCountBytes[] = { X(0x8B), X(0x42), X(0x08) };
inline const Pattern kFillCount = { "cuenta-fill", kFillCountBytes, 3, 0 };

// mov rcx,r14; call rel32; mov [r14+disp32],al -- la bandera de generacion.
// Unico. Se redirige el call (offset +3).
inline const pb kGenerationFlagBytes[] = { X(0x49), X(0x8B), X(0xCE), X(0xE8), W, W, W, W,
                                       X(0x41), X(0x88), X(0x86), W, W, X(0x00), X(0x00) };
inline const Pattern kGenerationFlag = { "flag-generacion", kGenerationFlagBytes, 15, 3 };

// El tope 5 del plugin, dos formas: mov [reg+0x45E4], 5 y mov edx,5; cmp
// ecx,edx; cmovb edx,ecx. Se escribe 6 sobre el 5.
inline const pb kCap6StoreBytes[] = { X(0xC7), M(0xF8, 0x80), X(0xE4), X(0x45), X(0x00), X(0x00),
                               X(0x05), X(0x00), X(0x00), X(0x00) };
inline const Pattern kCap6Store = { "tope6-store", kCap6StoreBytes, 10, 6 };
inline const pb kCap6CmovBytes[] = { X(0xBA), X(0x05), X(0x00), X(0x00), X(0x00),
                               X(0x3B), X(0xCA), X(0x0F), X(0x42), X(0xD1) };
inline const Pattern kCap6Cmov = { "tope6-cmov", kCap6CmovBytes, 10, 1 };

// El pacer de CPU, dos formas con relaciones entre bytes. Devuelven cuantas
// y el offset del byte a escribir de las primeras.
//   sete al; mov REG,r15d; movzx ecx,al; cmp edx,0x1E; cmovae REG,ecx
//   -> se anulan los 3 bytes del cmovae (offset +12).
inline int pacer_cmovae(const u8 *t, size_t len, size_t *out, int max) {
    int n = 0;
    for (size_t i = 0; i + 15 <= len; ++i) {
        if (t[i] != 0x0F || t[i + 1] != 0x94 || t[i + 2] != 0xC0) continue;
        if (t[i + 3] != 0x41 || t[i + 4] != 0x8B) continue;
        const u8 m1 = t[i + 5];
        if ((m1 & 0xC7) != 0xC7) continue;
        const unsigned reg = (m1 >> 3) & 7;
        if (t[i + 6] != 0x0F || t[i + 7] != 0xB6 || t[i + 8] != 0xC8) continue;
        if (t[i + 9] != 0x83 || t[i + 10] != 0xFA || t[i + 11] != 0x1E) continue;
        if (t[i + 12] != 0x0F || t[i + 13] != 0x43) continue;
        if (t[i + 14] != (u8)(0xC0 | (reg << 3) | 1)) continue;
        if (n < max) out[n] = i + 12;
        ++n;
    }
    return n;
}
//   test bl,bl; jne X; cmp edi,0x1E; jb X; mov bl,1 -- los dos saltos al
//   mismo destino. Se anula el mov bl,1 (offset +9).
inline int pacer_movbl(const u8 *t, size_t len, size_t *out, int max) {
    int n = 0;
    for (size_t i = 0; i + 11 <= len; ++i) {
        if (t[i] != 0x84 || t[i + 1] != 0xDB) continue;
        if (t[i + 2] != 0x75) continue;
        if (t[i + 4] != 0x83 || t[i + 5] != 0xFF || t[i + 6] != 0x1E) continue;
        if (t[i + 7] != 0x72) continue;
        if (t[i + 9] != 0xB3 || t[i + 10] != 0x01) continue;
        const long t1 = (long)(i + 4) + (signed char)t[i + 3];
        const long t2 = (long)(i + 9) + (signed char)t[i + 8];
        if (t1 != t2) continue;
        if (n < max) out[n] = i + 9;
        ++n;
    }
    return n;
}
//   [REX] cmp r/m,0x1E; [REX] setae r8 -- el respaldo cuando las otras dos no
//   estan. Devuelve cuantas y el offset del 0F del setcc.
inline int pacer_setae(const u8 *t, size_t len, size_t *out, int max) {
    int n = 0;
    for (size_t i = 0; i + 8 <= len; ++i) {
        size_t j = i;
        if (t[j] >= 0x40 && t[j] <= 0x4F) ++j;
        if (t[j] != 0x83) continue;
        const u8 m = t[j + 1];
        if ((m & 0xC0) != 0xC0 || (m & 0x38) != 0x38) continue;
        if (t[j + 2] != 0x1E) continue;
        size_t k = j + 3;
        if (t[k] >= 0x40 && t[k] <= 0x4F) ++k;
        if (t[k] != 0x0F || t[k + 1] != 0x93) continue;
        if ((t[k + 2] & 0xC0) != 0xC0) continue;
        if (n < max) out[n] = k;
        ++n;
    }
    return n;
}

// ---- nvngx_dlssg (el snippet) ----------------------------------------------

// cmp ebp,imm32; jl rel32; mov edi,5 -- el maximo por arquitectura. Y mov
// esi,5, que es unico. Se escribe el valor nuevo sobre el 5.
inline const pb kSnippetMaxEdiBytes[] = { X(0x81), X(0xFD), W, W, W, W, X(0x0F), X(0x8C), W, W, W, W,
                                    X(0xBF), X(0x05), X(0x00), X(0x00), X(0x00) };
inline const Pattern kSnippetMaxEdi = { "snippet-max-edi", kSnippetMaxEdiBytes, 17, 13 };
inline const pb kSnippetMaxEsiBytes[] = { X(0xBE), X(0x05), X(0x00), X(0x00), X(0x00) };
inline const Pattern kSnippetMaxEsi = { "snippet-max-esi", kSnippetMaxEsiBytes, 5, 1 };

// Las compuertas por arquitectura: cmp eax,0x1B0 y cmp r32,0x1B0 (Blackwell).
// Se escribe 0 sobre el inmediato: offset +1 y +2 respectivamente.
inline const unsigned kArchBlackwell = 0x1B0;
inline const pb kGateEaxBytes[] = { X(0x3D), X(0xB0), X(0x01), X(0x00), X(0x00) };
inline const Pattern kGateEax = { "gate-eax", kGateEaxBytes, 5, 1 };
inline const pb kGateRegBytes[] = { X(0x81), M(0xF8, 0xF8), X(0xB0), X(0x01), X(0x00), X(0x00) };
inline const Pattern kGateReg = { "gate-reg", kGateRegBytes, 6, 2 };

// ---- el .text de un archivo PE, para el test de host --------------------------
//
// Devuelve el offset y el largo de .text dentro del archivo (raw), o false.
inline bool text_of_file(const u8 *f, size_t n, size_t *off, size_t *len) {
    if (n < 0x40 || f[0] != 'M' || f[1] != 'Z') return false;
    const unsigned e_lfanew = *(const unsigned *)(f + 0x3C);
    if (e_lfanew + 0x18 > n || f[e_lfanew] != 'P' || f[e_lfanew + 1] != 'E') return false;
    const unsigned short nsec = *(const unsigned short *)(f + e_lfanew + 6);
    const unsigned short opt = *(const unsigned short *)(f + e_lfanew + 20);
    size_t sec = e_lfanew + 24 + opt;
    for (unsigned s = 0; s < nsec; ++s, sec += 40) {
        if (sec + 40 > n) return false;
        const char *nm = (const char *)(f + sec);
        if (nm[0] == '.' && nm[1] == 't' && nm[2] == 'e' && nm[3] == 'x' && nm[4] == 't') {
            const unsigned vsize = *(const unsigned *)(f + sec + 8);
            const unsigned rsize = *(const unsigned *)(f + sec + 16);
            const unsigned raw = *(const unsigned *)(f + sec + 20);
            *off = raw;
            *len = vsize < rsize ? vsize : rsize;
            return raw + *len <= n;
        }
    }
    return false;
}

}  // namespace sit
