// Pincha src/controlador.h con las mediciones que justificaron sus reglas.
//
//   g++ -std=c++20 -O2 -I src tools/test_controlador.cpp -o /tmp/tk.exe && /tmp/tk.exe
#include <cstdio>
#include <cstring>
#include "controller.h"

using namespace controller;

static int failures = 0;

static void check(const char *label, long got, long expected) {
    const bool ok = got == expected;
    if (!ok) ++failures;
    printf("  %-64s %s (dio %ld, esperado %ld)\n", label, ok ? "ok  " : "FALLA", got, expected);
}

// Junta las lineas para poder preguntar si algo se dijo.
struct Log {
    char b[64][96]; int n = 0;
    void line(const char *s) { if (n < 64) { strncpy(b[n], s, 95); b[n][95] = 0; ++n; } }
    void num(const char *s, unsigned long long v) { if (n < 64) { snprintf(b[n], 96, "%s%llu", s, v); ++n; } }
    int count(const char *prefix) const {
        int c = 0;
        for (int i = 0; i < n; ++i) if (strncmp(b[i], prefix, strlen(prefix)) == 0) ++c;
        return c;
    }
};

static long x100(double v) { return (long)(v * 100.0 + 0.5); }

int main(void) {
    printf("controlador DYNAMIC -- reglas contra sus mediciones\n\n");

    printf("sat: la tabla de GTA V (355 ventanas): de 3.5x a 5.0x las presentadas no se mueven\n");
    {
        State e; Config c; Log log;
        // w0: primera ventana, sin base anterior -> inestable, no aprende.
        control(e, c, 42.0, 150.0, log);
        check("primera ventana: no hay techo", x100(e.sat_ratio_max), 0);
        auto window = [&](double asked, double pres) {
            e.asked_sum = asked; e.asked_n = 1;
            control(e, c, 42.0, pres, log);
        };
        window(3.5, 161.1);
        window(4.0, 165.1);
        check("4.0 cobro 4 fps mas que 3.5: todavia no es techo", x100(e.sat_ratio_max), 0);
        window(4.5, 162.6);
        check("4.5 no cobro nada: techo en el ratio ANTERIOR, 4.00", x100(e.sat_ratio_max), 400);
        check("  y lo dijo", log.count("sat: techo detectado"), 1);
        window(4.0, 165.0);
        check("con techo y presentadas sostenidas: tantea mas barato, 3.90", x100(e.sat_ratio_max), 390);
        window(3.9, 155.0);
        check("cae un 6%: un paso atras, 4.00, y se congela", x100(e.sat_ratio_max), 400);
        check("  quieto", e.sat_frozen ? 1 : 0, 1);
        window(4.0, 165.0);
        check("quieto: no tantea mas", x100(e.sat_ratio_max), 400);
        control(e, c, 50.0, 165.0, log);     // la base salta 8 fps
        check("la base cambio: se olvida el techo", x100(e.sat_ratio_max), 0);
        check("  y lo dijo", log.count("sat: la base cambio"), 1);
    }

    printf("\nsesgo por tramo: pidiendo 2.33 entrega 2.47 (0.941), pidiendo 3.75 entrega 3.78\n");
    {
        State e; Config c; Log log;
        control(e, c, 40.0, 100.0, log);
        e.asked_sum = 2.33; e.asked_n = 1; control(e, c, 40.0, 2.47 * 40.0, log);
        check("tramo 2 aprende hacia 0.94 con ganancia 0.25: 0.985", x100(e.bias[2]), 99);
        e.asked_sum = 3.75; e.asked_n = 1; control(e, c, 40.0, 3.78 * 40.0, log);
        check("tramo 3 aprende aparte: 0.998", x100(e.bias[3]), 100);
        check("tramo 2 no se toco", x100(e.bias[2]), 99);
        e.clipped = true;
        e.asked_sum = 2.33; e.asked_n = 1; control(e, c, 40.0, 2.0 * 40.0, log);
        check("salida recortada: NO aprende (asi llego a 1.25 una vez)", x100(e.bias[2]), 99);
        e.clipped = false;
        for (int i = 0; i < 40; ++i) { e.asked_sum = 2.5; e.asked_n = 1; control(e, c, 40.0, 40.0 * 4.0, log); }
        check("entregando el doble de lo pedido: el sesgo topa en 0.80", x100(e.bias[2]), 80);
    }

    printf("\naplicar: objetivo 180 fps con base 37 (Cyberpunk, cp-dyn-steam-ok)\n");
    {
        State e; Config c; Log log;
        ApplyInput in{ 37.0, 180.0, 1000000, 1000000, 1000, 200 };
        ApplyOutput s = apply(e, c, in, log);
        check("primer tick: 180/37 = 4.86 -> 486", s.changes ? s.next : -1, 486);
        check("  no es una caida: sin sonda", s.probe ? 1 : 0, 0);
        check("  el pedido en vigor (2.00) se acumula para el sesgo", x100(e.asked_sum), 200);
        // 10 ms despues, 2 presentaciones: la deuda se mueve 0.2, el ratio no
        // cruza la banda muerta.
        in.now_qpc += 10000; in.pc += 2; in.dyn_target = 486;
        s = apply(e, c, in, log);
        check("10 ms y 2 presentadas: dentro de la banda muerta, no cambia", s.changes ? 1 : 0, 0);
        // Un salto de 0.6 s es un cambio de escena: la deuda se olvida.
        e.debt = 20.0;
        in.now_qpc += 600000; in.pc += 100;
        apply(e, c, in, log);
        check("salto de 0.6 s: la deuda vuelve a 0", x100(e.debt), 0);
    }
    {
        State e; Config c; Log log;
        ApplyInput in{ 20.0, 180.0, 1000000, 1000000, 1000, 486 };
        ApplyOutput s = apply(e, c, in, log);
        check("base 20: 180/20 = 9 -> se recorta a 6.00", s.next, 600);
        check("  y queda marcado como recortado (el sesgo no aprende)", e.clipped ? 1 : 0, 1);
        check("  lo dice una vez", log.count("dynamic: target needs more than 6x"), 1);
        in.now_qpc += 10000; in.pc += 1; in.dyn_target = 600;
        apply(e, c, in, log);
        check("  y no lo repite", log.count("dynamic: target needs more than 6x"), 1);
    }
    {
        State e; Config c; Log log;
        e.sat_ratio_max = 4.0;
        ApplyInput in{ 37.0, 180.0, 1000000, 1000000, 1000, 200 };
        ApplyOutput s = apply(e, c, in, log);
        check("con techo medido 4.00: 4.86 se recorta a 400", s.next, 400);
        check("  y lo dice", log.count("dynamic: recortado al techo medido"), 1);
        c.sat_on = false;
        s = apply(e, c, in, log);
        check("sat apagado (mfg-sinsat): no recorta, 486", s.next, 486);
    }
    {
        State e; Config c; Log log;
        c.use_debt = false;
        e.debt = 30.0;
        ApplyInput in{ 37.0, 180.0, 1000000, 1000000, 1000, 200 };
        ApplyOutput s = apply(e, c, in, log);
        check("sin integrador (mfg-sin-deuda): la deuda no entra, 486", s.next, 486);
    }
    {
        State e; Config c; Log log;
        ApplyInput in{ 37.0, 180.0, 1000000, 1000000, 1000, 580 };
        ApplyOutput s = apply(e, c, in, log);
        check("de 5.80 a 4.86 es una caida de mas de 0.80: arranca la sonda", s.probe ? 1 : 0, 1);
    }

    printf("\n%s\n", failures == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return failures ? 1 : 0;
}
