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
#include "sites.h"

using namespace sites;

static int failures = 0;

static void check(const char *label, long got, long expected) {
    const bool ok = got == expected;
    if (!ok) ++failures;
    printf("  %-58s %s (dio %ld, esperado %ld)\n", label, ok ? "ok  " : "FALLA", got, expected);
}

static bool read(const char *path, std::vector<u8> &out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    const bool ok = fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

static bool text_of(const char *name, std::vector<u8> &f, const u8 **t, size_t *len) {
    char path[1024];
    const char *la = getenv("LOCALAPPDATA");
    snprintf(path, sizeof path, "%s\\mfg-unlock\\sdk\\2.12\\%s", la ? la : ".", name);
    if (!read(path, f)) { printf("  ! no se pudo leer %s\n", path); ++failures; return false; }
    size_t off = 0;
    if (!text_of_file(f.data(), f.size(), &off, len)) { printf("  ! %s no tiene .text\n", name); ++failures; return false; }
    *t = f.data() + off;
    printf("  %s: .text de %zu bytes\n", name, *len);
    return true;
}

int main(void) {
    printf("sitios de parche sobre los archivos reales de la cache 2.12\n\n");
    size_t at[8];

    printf("sl.dlss_g.dll (el plugin)\n");
    {
        std::vector<u8> f; const u8 *t; size_t len;
        if (text_of("sl.dlss_g.dll", f, &t, &len)) {
            const int wic = find(t, len, kWorkItemCount, at, 8);
            check("cuenta de work items: unica", wic, 1);
            if (wic == 1)
                check("  y el llenado viene justo despues (+12)", matches(t + at[0] + 12, kFillCount) ? 1 : 0, 1);
            check("bandera de generacion: unica", find(t, len, kGenerationFlag, at, 8), 1);
            const int a = find(t, len, kCap6Store, at, 8);
            const int b = find(t, len, kCap6Cmov, at, 8);
            check("tope 5 -> 6: dos sitios (store + cmov)", a + b, 2);
            check("  el store", a, 1);
            check("  el cmov", b, 1);
            const int p1 = pacer_cmovae(t, len, at, 8);
            const int p2 = pacer_movbl(t, len, at, 8);
            check("pacer de CPU: un sitio entre las dos formas", p1 + p2, 1);
            if (p1 + p2 == 0)
                check("  respaldo setae: unico", pacer_setae(t, len, at, 8), 1);
        }
    }

    printf("\nnvngx_dlssg.dll (el snippet)\n");
    {
        std::vector<u8> f; const u8 *t; size_t len;
        if (text_of("nvngx_dlssg.dll", f, &t, &len)) {
            const int ge = find(t, len, kGateEax, at, 8);
            const int gr = find(t, len, kGateReg, at, 8);
            check("compuertas Blackwell (0x1B0): dos", ge + gr, 2);
            const int ma = find(t, len, kSnippetMaxEdi, at, 8);
            const int mb = find(t, len, kSnippetMaxEsi, at, 8);
            check("maximo por arquitectura: cmp/jl/mov edi,5, uno", ma, 1);
            check("  mov esi,5: al menos uno (se escribe el primero)", mb >= 1 ? 1 : 0, 1);
        }
    }

    printf("\nel matcher\n");
    {
        const u8 bytes[] = { 0x00, 0xC7, 0x85, 0xE4, 0x45, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0xFF };
        check("mascara F8 sobre el modrm: 0x85 casa con 0x80", find(bytes, sizeof bytes, kCap6Store, at, 8), 1);
        check("  en el offset 1", (long)at[0], 1);
        const u8 bytes_b[] = { 0xC7, 0x88, 0xE4, 0x45, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00 };
        check("0x88 no casa (bit 3)", find(bytes_b, sizeof bytes_b, kCap6Store, at, 8), 0);
        check("patron mas largo que el buffer: 0", find(bytes_b, 4, kCap6Store, at, 8), 0);
    }

    printf("\n%s\n", failures == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return failures ? 1 : 0;
}
