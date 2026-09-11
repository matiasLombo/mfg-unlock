// Corre las busquedas de src/sitios.h sobre los ARCHIVOS reales que el mod
// carga (la cache en %LOCALAPPDATA%\mfg-unlock\sdk\2.12) y compara con lo
// que los juegos loguean. Sin juego, sin GPU: si NVIDIA suelta un build
// nuevo y un sitio se mueve, esto lo dice antes que nadie.
//
// Los numeros esperados salen del log de Cyberpunk del 2026-09-11
// (mfg-unlock.cp-6x-steam-ok.log): "tope del plugin subido a 6, sitios: 2",
// "work item count is ours", "fill count is ours too", "generation flag
// redirected", "CPU pacer enabled, sites: 1", "gates rewritten: 2", "tope
// del snippet subido a 6, sitios: 2".
//
//   g++ -std=c++20 -O2 -I src tools/test_sitios.cpp -o /tmp/ts.exe && /tmp/ts.exe
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "sitios.h"

using namespace sit;

static int fallos = 0;

static void chequear(const char *caso, long got, long esperado) {
    const bool ok = got == esperado;
    if (!ok) ++fallos;
    printf("  %-58s %s (dio %ld, esperado %ld)\n", caso, ok ? "ok  " : "FALLA", got, esperado);
}

static bool leer(const char *ruta, std::vector<u8> &out) {
    FILE *f = fopen(ruta, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    const bool ok = fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

static bool text_de(const char *nombre, std::vector<u8> &f, const u8 **t, size_t *len) {
    char ruta[1024];
    const char *la = getenv("LOCALAPPDATA");
    snprintf(ruta, sizeof ruta, "%s\\mfg-unlock\\sdk\\2.12\\%s", la ? la : ".", nombre);
    if (!leer(ruta, f)) { printf("  ! no se pudo leer %s\n", ruta); ++fallos; return false; }
    size_t off = 0;
    if (!text_del_archivo(f.data(), f.size(), &off, len)) { printf("  ! %s no tiene .text\n", nombre); ++fallos; return false; }
    *t = f.data() + off;
    printf("  %s: .text de %zu bytes\n", nombre, *len);
    return true;
}

int main(void) {
    printf("sitios de parche sobre los archivos reales de la cache 2.12\n\n");
    size_t at[8];

    printf("sl.dlss_g.dll (el plugin)\n");
    {
        std::vector<u8> f; const u8 *t; size_t len;
        if (text_de("sl.dlss_g.dll", f, &t, &len)) {
            const int wic = buscar(t, len, kCuentaWorkItem, at, 8);
            chequear("cuenta de work items: unica", wic, 1);
            if (wic == 1)
                chequear("  y el llenado viene justo despues (+12)", casa(t + at[0] + 12, kCuentaFill) ? 1 : 0, 1);
            chequear("bandera de generacion: unica", buscar(t, len, kFlagGeneracion, at, 8), 1);
            const int a = buscar(t, len, kTope6A, at, 8);
            const int b = buscar(t, len, kTope6B, at, 8);
            chequear("tope 5 -> 6: dos sitios (store + cmov)", a + b, 2);
            chequear("  el store", a, 1);
            chequear("  el cmov", b, 1);
            const int p1 = pacer_cmovae(t, len, at, 8);
            const int p2 = pacer_movbl(t, len, at, 8);
            chequear("pacer de CPU: un sitio entre las dos formas", p1 + p2, 1);
            if (p1 + p2 == 0)
                chequear("  respaldo setae: unico", pacer_setae(t, len, at, 8), 1);
        }
    }

    printf("\nnvngx_dlssg.dll (el snippet)\n");
    {
        std::vector<u8> f; const u8 *t; size_t len;
        if (text_de("nvngx_dlssg.dll", f, &t, &len)) {
            const int ge = buscar(t, len, kGateEax, at, 8);
            const int gr = buscar(t, len, kGateReg, at, 8);
            chequear("compuertas Blackwell (0x1B0): dos", ge + gr, 2);
            const int ma = buscar(t, len, kSnippetMaxA, at, 8);
            const int mb = buscar(t, len, kSnippetMaxB, at, 8);
            chequear("maximo por arquitectura: cmp/jl/mov edi,5, uno", ma, 1);
            chequear("  mov esi,5: al menos uno (se escribe el primero)", mb >= 1 ? 1 : 0, 1);
        }
    }

    printf("\nel matcher\n");
    {
        const u8 bytes[] = { 0x00, 0xC7, 0x85, 0xE4, 0x45, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0xFF };
        chequear("mascara F8 sobre el modrm: 0x85 casa con 0x80", buscar(bytes, sizeof bytes, kTope6A, at, 8), 1);
        chequear("  en el offset 1", (long)at[0], 1);
        const u8 bytes2[] = { 0xC7, 0x88, 0xE4, 0x45, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00 };
        chequear("0x88 no casa (bit 3)", buscar(bytes2, sizeof bytes2, kTope6A, at, 8), 0);
        chequear("patron mas largo que el buffer: 0", buscar(bytes2, 4, kTope6A, at, 8), 0);
    }

    printf("\n%s\n", fallos == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return fallos ? 1 : 0;
}
