// jsonl.h -- el formato de salida: una linea JSON por evento, un archivo por
// sesion. Es el contrato entre el DLL (C++) y el analizador (Python).
//
// ENTRA: Events ya drenados del ring.
// SALE: lineas de texto; y al reves, un parser chico para replay y tests.
// DEPENDE DE: events.h, keys.h.
//
// Por que JSONL y no un binario: el archivo tiene que poder mirarse con
// `head` y con `grep` cuando algo no cierra, y el analizador tiene que poder
// leer una sesion a medio escribir (el juego crasheo, el ultimo renglon quedo
// cortado) sin perder las 200000 lineas anteriores. Un binario con indice al
// final no sobrevive eso.
//
// Las claves de 64 bits van como string hexadecimal, no como numero: un u64
// mayor a 2^53 se rompe en cualquier consumidor que use doubles (jq, JS), y
// ese bug aparece tarde y en silencio.
#pragma once

#include "events.h"
#include "keys.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gp {

inline constexpr int kJsonlSchema = 1;

// Cuanto ocupa la linea mas larga (un res con todos sus campos): 512 alcanza
// con margen. El writer nunca aloca.
inline constexpr size_t kMaxLine = 512;

inline double ticks_to_ms(u64 begin, u64 end, u64 freq) {
    if (freq == 0 || end < begin) return 0.0;
    return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(freq);
}

// Serializa un evento. Devuelve la cantidad de bytes escritos (sin el '\n'),
// o 0 si el evento no se serializa (None). out tiene que tener kMaxLine.
size_t format_event(char *out, size_t cap, const Event &e, const OutputInfo &res);

// La primera linea de cada sesion. Sin esto el analizador no sabe contra que
// resolucion normalizar ni que version de esquema esta leyendo.
size_t format_header(char *out, size_t cap, const char *exe, const char *adapter,
                     const OutputInfo &res, u64 vram_budget, const char *version);

// --- parser ---------------------------------------------------------------
//
// Solo objetos planos: {"a":1,"b":"x"}. No hay anidamiento en el formato y no
// lo va a haber -- si algun dia hace falta, es senal de que ese dato tenia que
// ser otra linea.

struct KV {
    const char *key;
    size_t      key_len;
    const char *val;
    size_t      val_len;
    bool        is_str;
};

struct ParsedLine {
    KV     kv[32];
    size_t n = 0;

    const KV *find(const char *k) const {
        const size_t kl = std::strlen(k);
        for (size_t i = 0; i < n; ++i)
            if (kv[i].key_len == kl && std::memcmp(kv[i].key, k, kl) == 0)
                return &kv[i];
        return nullptr;
    }
    bool has(const char *k) const { return find(k) != nullptr; }

    u64 u(const char *k, u64 def = 0) const {
        const KV *e = find(k);
        if (!e) return def;
        // Hexadecimal con 0x (las claves) o decimal.
        if (e->val_len > 2 && e->val[0] == '0' && (e->val[1] == 'x' || e->val[1] == 'X')) {
            u64 v = 0;
            for (size_t i = 2; i < e->val_len; ++i) {
                const char c = e->val[i];
                u64 d;
                if (c >= '0' && c <= '9') d = static_cast<u64>(c - '0');
                else if (c >= 'a' && c <= 'f') d = static_cast<u64>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = static_cast<u64>(c - 'A' + 10);
                else break;
                v = v * 16 + d;
            }
            return v;
        }
        u64 v = 0;
        for (size_t i = 0; i < e->val_len; ++i) {
            if (e->val[i] < '0' || e->val[i] > '9') break;
            v = v * 10 + static_cast<u64>(e->val[i] - '0');
        }
        return v;
    }

    double f(const char *k, double def = 0.0) const {
        const KV *e = find(k);
        if (!e || e->val_len == 0 || e->val_len >= 64) return def;
        char buf[64];
        std::memcpy(buf, e->val, e->val_len);
        buf[e->val_len] = 0;
        return std::strtod(buf, nullptr);
    }

    // Devuelve el string sin comillas. No copia: apunta a la linea original.
    const char *s(const char *k, size_t *len) const {
        const KV *e = find(k);
        if (!e) { *len = 0; return nullptr; }
        *len = e->val_len;
        return e->val;
    }

    bool str_is(const char *k, const char *expect) const {
        size_t n2 = 0;
        const char *p = s(k, &n2);
        return p && n2 == std::strlen(expect) && std::memcmp(p, expect, n2) == 0;
    }
};

// Parsea una linea. Devuelve false si no es un objeto JSON plano bien formado
// -- una linea cortada por un crash del juego cae aca y se saltea.
bool parse_line(const char *line, size_t len, ParsedLine &out);

// Reconstruye un Event de una linea parseada. Devuelve false para lineas que
// no son eventos (el header). El descriptor se rearma con los numeros crudos,
// no con la categoria: la categoria es derivada y se vuelve a calcular.
bool event_from_line(const ParsedLine &p, Event &e);

}  // namespace gp
