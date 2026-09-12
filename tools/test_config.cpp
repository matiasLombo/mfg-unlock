// Pincha src/config.h con lo que hay de verdad al lado de la dll en los tres
// juegos (2026-09-11), y con los bordes del parser que DllMain aplicaba.
//
//   g++ -std=c++20 -O2 -I src tools/test_config.cpp -o /tmp/tc.exe && /tmp/tc.exe
#include <cstdio>
#include <cstring>
#include "config.h"

using namespace config;

static int failures = 0;

static void check(const char *label, long got, long expected) {
    const bool ok = got == expected;
    if (!ok) ++failures;
    printf("  %-62s %s (dio %ld, esperado %ld)\n", label, ok ? "ok  " : "FALLA", got, expected);
}

// Una carpeta simulada: los archivos que existen y lo que traen.
struct Folder {
    const wchar_t *names[8];
    const char *contents[8];
    int n;
    bool exists(const wchar_t *f) const {
        for (int i = 0; i < n; ++i) if (wcscmp(names[i], f) == 0) return true;
        return false;
    }
    unsigned read(const wchar_t *f, char *b, unsigned cap) const {
        for (int i = 0; i < n; ++i)
            if (wcscmp(names[i], f) == 0 && contents[i] != nullptr) {
                unsigned k = 0;
                while (contents[i][k] != 0 && k < cap) { b[k] = contents[i][k]; ++k; }
                return k;
            }
        return 0;
    }
};

static Settings load(const Folder &c) {
    Settings a;
    read_flags(a, [&](const wchar_t *f) { return c.exists(f); });
    read_numerics(a, [&](const wchar_t *f, char *b, unsigned cap) { return c.read(f, b, cap); });
    return a;
}

static const int kPanRows = 9, kMaxCustom = 600;

int main(void) {
    printf("configuracion -- las carpetas reales y los bordes del parser\n\n");

    printf("sin ningun archivo: los defectos\n");
    {
        Folder c{ {}, {}, 0 };
        Settings a = load(c);
        check("frac encendido", a.frac, 1);
        check("slowalt encendido", a.slowalt, 1);
        check("seis encendido", a.seis, 1);
        check("wic encendido", a.wic, 1);
        check("cubins encendido", a.cubins, 1);
        check("panel encendido", a.panel, 1);
        check("sat encendido", a.sat, 1);
        check("latch encendido", a.latch, 1);
        check("deuda encendida", a.deuda, 1);
        check("ota apagado", a.ota, 0);
        check("x6 apagado", a.x6, 0);
        check("host apagado", a.host, 0);
        check("hosthudless apagado", a.hosthudless, 0);
        check("dynstep 0 (sin grilla)", a.dynstep, 0);
        check("fracdiff apagado", a.fracdiff, 0);
        check("queue -1 (sin archivo)", a.queue, -1);
        check("mode -1 (sin settings)", a.mode, -1);
    }

    printf("\nGTA V: mfg-debug.txt, mfg-sllog.txt, mfg-settings.txt\n");
    {
        Folder c{ { L"mfg-debug.txt", L"mfg-sllog.txt", L"mfg-settings.txt" },
                   { nullptr, nullptr, "mode 6\r\ntarget 400\r\ndynfps 165\r\nhud 1\r\n" }, 3 };
        Settings a = load(c);
        check("debug", a.debug, 1);
        check("sllog", a.sllog, 1);
        check("todo lo demas en defecto (seis)", a.seis, 1);
        char b[256]; unsigned n = c.read(L"mfg-settings.txt", b, 255);
        parse_settings(b, n, kPanRows, kMaxCustom, a);
        check("mode 6", a.mode, 6);
        check("target 400", a.target, 400);
        check("dynfps 165", a.dynfps, 165);
        check("hud 1", a.hud, 1);
    }

    printf("\nHalo: mfg-seis.txt y mfg-panel.txt son archivos VIEJOS, no hacen nada\n");
    {
        Folder c{ { L"mfg-seis.txt", L"mfg-panel.txt", L"mfg-sllog.txt", L"mfg-settings.txt" },
                   { nullptr, nullptr, nullptr, "mode 6\r\ntarget 600\r\ndynfps 240\r\nhud 1\r\n" }, 4 };
        Settings a = load(c);
        check("seis sigue en su defecto (encendido)", a.seis, 1);
        check("panel sigue en su defecto (encendido)", a.panel, 1);
        char b[256]; unsigned n = c.read(L"mfg-settings.txt", b, 255);
        parse_settings(b, n, kPanRows, kMaxCustom, a);
        check("target 600 (el tope)", a.target, 600);
        check("dynfps 240", a.dynfps, 240);
    }

    printf("\nCyberpunk: solo settings\n");
    {
        Folder c{ { L"mfg-settings.txt" }, { "mode 6\r\ntarget 588\r\ndynfps 180\r\nhud 1\r\n" }, 1 };
        Settings a = load(c);
        char b[256]; unsigned n = c.read(L"mfg-settings.txt", b, 255);
        parse_settings(b, n, kPanRows, kMaxCustom, a);
        check("mode 6", a.mode, 6);
        check("target 588", a.target, 588);
    }

    printf("\nbordes del parser de settings\n");
    {
        Settings a;
        const char *t = "mode 9\r\n";          // fuera de kPanRows
        parse_settings(t, (unsigned)strlen(t), kPanRows, kMaxCustom, a);
        check("mode fuera de rango se ignora", a.mode, -1);
        t = "target 601\r\n";
        parse_settings(t, (unsigned)strlen(t), kPanRows, kMaxCustom, a);
        check("target fuera de rango se ignora", a.target, -1);
        t = "mode = 4\r\n";
        parse_settings(t, (unsigned)strlen(t), kPanRows, kMaxCustom, a);
        check("mode = 4 (con igual y espacios)", a.mode, 4);
        t = "mode\r\n";
        Settings b;
        parse_settings(t, (unsigned)strlen(t), kPanRows, kMaxCustom, b);
        check("mode sin numero: nada (escritura truncada)", b.mode, -1);
        t = "hud 0\r\n";
        parse_settings(t, (unsigned)strlen(t), kPanRows, kMaxCustom, b);
        check("hud 0", b.hud, 0);
        t = "dynfps 1001\r\n";
        parse_settings(t, (unsigned)strlen(t), kPanRows, kMaxCustom, b);
        check("dynfps 1001 se ignora", b.dynfps, -1);
        t = "target 150\r\n";
        parse_settings(t, (unsigned)strlen(t), kPanRows, kMaxCustom, b);
        check("target 150 pasa crudo (el piso lo pone quien llama)", b.target, 150);
    }

    printf("\nnumericos\n");
    {
        Folder c{ { L"mfg-blockms.txt", L"mfg-markergap.txt", L"mfg-slowframe.txt",
                     L"mfg-jitter.txt", L"mfg-clamplatency.txt", L"mfg-queue.txt", L"mfg-blocks.txt" },
                   { "24\r\n", "500 250 8000 300", "16000 20000 500", "95", "2", "3", "4" }, 7 };
        Settings a = load(c);
        check("blockms 24", a.blockms, 24);
        check("markergap trae 4", a.marker_n, 4);
        check("  el tercero 8000", a.marker[2], 8000);
        check("slowframe trae 3", a.slowframe_n, 3);
        check("  el primero 16000", a.slowframe[0], 16000);
        check("jitter 95 fuera de rango: queda 0", a.jitter, 0);
        check("clamplatency 2", a.clamplatency, 2);
        check("queue 3", a.queue, 3);
        check("blocks 4", a.blocks, 4);
    }
    {
        Folder c{ { L"mfg-blocks.txt", L"mfg-queue.txt", L"mfg-blockms.txt" },
                   { "1", "7", "abc" }, 3 };
        Settings a = load(c);
        check("blocks 1 (menor a 2): queda 0", a.blocks, 0);
        check("queue 7: queda -1", a.queue, -1);
        check("blockms sin digitos: queda 0", a.blockms, 0);
    }

    printf("\nmfg-config.txt: el archivo unico, polaridad directa\n");
    {
        // Lo mismo que los archivos de Halo/GTA V, dicho en un archivo.
        Settings a;
        const char *t = "seis 0\r\nfrac 0\r\ndebug 1\r\nsllog 1\r\nblockms 24\r\n"
                        "queue 2\r\nmarkergap 500 250 8000 300\r\nslowframe 16000\r\n"
                        "# comentario\r\nnoexiste 1\r\nx6 7\r\n";
        const int seen = parse_config(t, (unsigned)strlen(t), a);
        check("claves reconocidas", seen, 8);
        check("seis 0 apaga el 6X", a.seis, 0);
        check("frac 0 apaga el fraccional", a.frac, 0);
        check("debug 1", a.debug, 1);
        check("sllog 1", a.sllog, 1);
        check("blockms 24", a.blockms, 24);
        check("queue 2", a.queue, 2);
        check("markergap trae 4", a.marker_n, 4);
        check("  el cuarto 300", a.marker[3], 300);
        check("slowframe trae 1", a.slowframe_n, 1);
        check("x6 7 no es 0|1: se ignora", a.x6, 0);
        check("lo demas queda en defecto (wic)", a.wic, 1);
    }
    {
        // Archivo y clave a la vez: la clave explicita gana, en cualquier sentido.
        Folder c{ { L"mfg-sinseis.txt" }, { nullptr }, 1 };
        Settings a = load(c);
        check("mfg-sinseis.txt apaga", a.seis, 0);
        const char *t = "seis 1\n";
        parse_config(t, (unsigned)strlen(t), a);
        check("  y `seis 1` en mfg-config.txt lo vuelve a encender", a.seis, 1);
        t = "blocks 3\n";
        parse_config(t, (unsigned)strlen(t), a);
        check("blocks 3 con separador de una sola letra", a.blocks, 3);
        t = "blockms 5\n";
        parse_config(t, (unsigned)strlen(t), a);
        check("blockms no pisa blocks (prefijo comun)", a.blocks, 3);
        check("  y si pone blockms", a.blockms, 5);
        t = "seis=0\n";
        parse_config(t, (unsigned)strlen(t), a);
        check("clave=valor tambien vale", a.seis, 0);
    }

    printf("\n%s\n", failures == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return failures ? 1 : 0;
}
