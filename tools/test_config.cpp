// Pincha src/config.h con lo que hay de verdad al lado de la dll en los tres
// juegos (2026-09-11), y con los bordes del parser que DllMain aplicaba.
//
//   g++ -std=c++20 -O2 -I src tools/test_config.cpp -o /tmp/tc.exe && /tmp/tc.exe
#include <cstdio>
#include <cstring>
#include "config.h"

using namespace cfg;

static int fallos = 0;

static void chequear(const char *caso, long got, long esperado) {
    const bool ok = got == esperado;
    if (!ok) ++fallos;
    printf("  %-62s %s (dio %ld, esperado %ld)\n", caso, ok ? "ok  " : "FALLA", got, esperado);
}

// Una carpeta simulada: los archivos que existen y lo que traen.
struct Carpeta {
    const wchar_t *nombres[8];
    const char *contenidos[8];
    int n;
    bool existe(const wchar_t *f) const {
        for (int i = 0; i < n; ++i) if (wcscmp(nombres[i], f) == 0) return true;
        return false;
    }
    unsigned leer(const wchar_t *f, char *b, unsigned cap) const {
        for (int i = 0; i < n; ++i)
            if (wcscmp(nombres[i], f) == 0 && contenidos[i] != nullptr) {
                unsigned k = 0;
                while (contenidos[i][k] != 0 && k < cap) { b[k] = contenidos[i][k]; ++k; }
                return k;
            }
        return 0;
    }
};

static Ajustes cargar(const Carpeta &c) {
    Ajustes a;
    leer_banderas(a, [&](const wchar_t *f) { return c.existe(f); });
    leer_numericos(a, [&](const wchar_t *f, char *b, unsigned cap) { return c.leer(f, b, cap); });
    return a;
}

static const int kPanRows = 9, kMaxCustom = 600;

int main(void) {
    printf("configuracion -- las carpetas reales y los bordes del parser\n\n");

    printf("sin ningun archivo: los defectos\n");
    {
        Carpeta c{ {}, {}, 0 };
        Ajustes a = cargar(c);
        chequear("frac encendido", a.frac, 1);
        chequear("slowalt encendido", a.slowalt, 1);
        chequear("seis encendido", a.seis, 1);
        chequear("wic encendido", a.wic, 1);
        chequear("cubins encendido", a.cubins, 1);
        chequear("panel encendido", a.panel, 1);
        chequear("sat encendido", a.sat, 1);
        chequear("latch encendido", a.latch, 1);
        chequear("deuda encendida", a.deuda, 1);
        chequear("ota apagado", a.ota, 0);
        chequear("x6 apagado", a.x6, 0);
        chequear("queue -1 (sin archivo)", a.queue, -1);
        chequear("mode -1 (sin settings)", a.mode, -1);
    }

    printf("\nGTA V: mfg-debug.txt, mfg-sllog.txt, mfg-settings.txt\n");
    {
        Carpeta c{ { L"mfg-debug.txt", L"mfg-sllog.txt", L"mfg-settings.txt" },
                   { nullptr, nullptr, "mode 6\r\ntarget 400\r\ndynfps 165\r\nhud 1\r\n" }, 3 };
        Ajustes a = cargar(c);
        chequear("debug", a.debug, 1);
        chequear("sllog", a.sllog, 1);
        chequear("todo lo demas en defecto (seis)", a.seis, 1);
        char b[256]; unsigned n = c.leer(L"mfg-settings.txt", b, 255);
        parsear_settings(b, n, kPanRows, kMaxCustom, a);
        chequear("mode 6", a.mode, 6);
        chequear("target 400", a.target, 400);
        chequear("dynfps 165", a.dynfps, 165);
        chequear("hud 1", a.hud, 1);
    }

    printf("\nHalo: mfg-seis.txt y mfg-panel.txt son archivos VIEJOS, no hacen nada\n");
    {
        Carpeta c{ { L"mfg-seis.txt", L"mfg-panel.txt", L"mfg-sllog.txt", L"mfg-settings.txt" },
                   { nullptr, nullptr, nullptr, "mode 6\r\ntarget 600\r\ndynfps 240\r\nhud 1\r\n" }, 4 };
        Ajustes a = cargar(c);
        chequear("seis sigue en su defecto (encendido)", a.seis, 1);
        chequear("panel sigue en su defecto (encendido)", a.panel, 1);
        char b[256]; unsigned n = c.leer(L"mfg-settings.txt", b, 255);
        parsear_settings(b, n, kPanRows, kMaxCustom, a);
        chequear("target 600 (el tope)", a.target, 600);
        chequear("dynfps 240", a.dynfps, 240);
    }

    printf("\nCyberpunk: solo settings\n");
    {
        Carpeta c{ { L"mfg-settings.txt" }, { "mode 6\r\ntarget 588\r\ndynfps 180\r\nhud 1\r\n" }, 1 };
        Ajustes a = cargar(c);
        char b[256]; unsigned n = c.leer(L"mfg-settings.txt", b, 255);
        parsear_settings(b, n, kPanRows, kMaxCustom, a);
        chequear("mode 6", a.mode, 6);
        chequear("target 588", a.target, 588);
    }

    printf("\nbordes del parser de settings\n");
    {
        Ajustes a;
        const char *t = "mode 9\r\n";          // fuera de kPanRows
        parsear_settings(t, (unsigned)strlen(t), kPanRows, kMaxCustom, a);
        chequear("mode fuera de rango se ignora", a.mode, -1);
        t = "target 601\r\n";
        parsear_settings(t, (unsigned)strlen(t), kPanRows, kMaxCustom, a);
        chequear("target fuera de rango se ignora", a.target, -1);
        t = "mode = 4\r\n";
        parsear_settings(t, (unsigned)strlen(t), kPanRows, kMaxCustom, a);
        chequear("mode = 4 (con igual y espacios)", a.mode, 4);
        t = "mode\r\n";
        Ajustes b;
        parsear_settings(t, (unsigned)strlen(t), kPanRows, kMaxCustom, b);
        chequear("mode sin numero: nada (escritura truncada)", b.mode, -1);
        t = "hud 0\r\n";
        parsear_settings(t, (unsigned)strlen(t), kPanRows, kMaxCustom, b);
        chequear("hud 0", b.hud, 0);
        t = "dynfps 1001\r\n";
        parsear_settings(t, (unsigned)strlen(t), kPanRows, kMaxCustom, b);
        chequear("dynfps 1001 se ignora", b.dynfps, -1);
        t = "target 150\r\n";
        parsear_settings(t, (unsigned)strlen(t), kPanRows, kMaxCustom, b);
        chequear("target 150 pasa crudo (el piso lo pone quien llama)", b.target, 150);
    }

    printf("\nnumericos\n");
    {
        Carpeta c{ { L"mfg-blockms.txt", L"mfg-markergap.txt", L"mfg-slowframe.txt",
                     L"mfg-jitter.txt", L"mfg-clamplatency.txt", L"mfg-queue.txt", L"mfg-blocks.txt" },
                   { "24\r\n", "500 250 8000 300", "16000 20000 500", "95", "2", "3", "4" }, 7 };
        Ajustes a = cargar(c);
        chequear("blockms 24", a.blockms, 24);
        chequear("markergap trae 4", a.marker_n, 4);
        chequear("  el tercero 8000", a.marker[2], 8000);
        chequear("slowframe trae 3", a.slowframe_n, 3);
        chequear("  el primero 16000", a.slowframe[0], 16000);
        chequear("jitter 95 fuera de rango: queda 0", a.jitter, 0);
        chequear("clamplatency 2", a.clamplatency, 2);
        chequear("queue 3", a.queue, 3);
        chequear("blocks 4", a.blocks, 4);
    }
    {
        Carpeta c{ { L"mfg-blocks.txt", L"mfg-queue.txt", L"mfg-blockms.txt" },
                   { "1", "7", "abc" }, 3 };
        Ajustes a = cargar(c);
        chequear("blocks 1 (menor a 2): queda 0", a.blocks, 0);
        chequear("queue 7: queda -1", a.queue, -1);
        chequear("blockms sin digitos: queda 0", a.blockms, 0);
    }

    printf("\n%s\n", fallos == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return fallos ? 1 : 0;
}
