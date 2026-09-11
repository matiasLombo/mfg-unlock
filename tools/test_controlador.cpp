// Pincha src/controlador.h con las mediciones que justificaron sus reglas.
//
//   g++ -std=c++20 -O2 -I src tools/test_controlador.cpp -o /tmp/tk.exe && /tmp/tk.exe
#include <cstdio>
#include <cstring>
#include "controlador.h"

using namespace ctl;

static int fallos = 0;

static void chequear(const char *caso, long got, long esperado) {
    const bool ok = got == esperado;
    if (!ok) ++fallos;
    printf("  %-64s %s (dio %ld, esperado %ld)\n", caso, ok ? "ok  " : "FALLA", got, esperado);
}

// Junta las lineas para poder preguntar si algo se dijo.
struct Log {
    char b[64][96]; int n = 0;
    void linea(const char *s) { if (n < 64) { strncpy(b[n], s, 95); b[n][95] = 0; ++n; } }
    void num(const char *s, unsigned long long v) { if (n < 64) { snprintf(b[n], 96, "%s%llu", s, v); ++n; } }
    int cuenta(const char *prefijo) const {
        int c = 0;
        for (int i = 0; i < n; ++i) if (strncmp(b[i], prefijo, strlen(prefijo)) == 0) ++c;
        return c;
    }
};

static long x100(double v) { return (long)(v * 100.0 + 0.5); }

int main(void) {
    printf("controlador DYNAMIC -- reglas contra sus mediciones\n\n");

    printf("sat: la tabla de GTA V (355 ventanas): de 3.5x a 5.0x las presentadas no se mueven\n");
    {
        Estado e; Config c; Log log;
        // w0: primera ventana, sin base anterior -> inestable, no aprende.
        control(e, c, 42.0, 150.0, log);
        chequear("primera ventana: no hay techo", x100(e.sat_ratio_max), 0);
        auto ventana = [&](double asked, double pres) {
            e.asked_sum = asked; e.asked_n = 1;
            control(e, c, 42.0, pres, log);
        };
        ventana(3.5, 161.1);
        ventana(4.0, 165.1);
        chequear("4.0 cobro 4 fps mas que 3.5: todavia no es techo", x100(e.sat_ratio_max), 0);
        ventana(4.5, 162.6);
        chequear("4.5 no cobro nada: techo en el ratio ANTERIOR, 4.00", x100(e.sat_ratio_max), 400);
        chequear("  y lo dijo", log.cuenta("sat: techo detectado"), 1);
        ventana(4.0, 165.0);
        chequear("con techo y presentadas sostenidas: tantea mas barato, 3.90", x100(e.sat_ratio_max), 390);
        ventana(3.9, 155.0);
        chequear("cae un 6%: un paso atras, 4.00, y se congela", x100(e.sat_ratio_max), 400);
        chequear("  quieto", e.sat_quieto ? 1 : 0, 1);
        ventana(4.0, 165.0);
        chequear("quieto: no tantea mas", x100(e.sat_ratio_max), 400);
        control(e, c, 50.0, 165.0, log);     // la base salta 8 fps
        chequear("la base cambio: se olvida el techo", x100(e.sat_ratio_max), 0);
        chequear("  y lo dijo", log.cuenta("sat: la base cambio"), 1);
    }

    printf("\nsesgo por tramo: pidiendo 2.33 entrega 2.47 (0.941), pidiendo 3.75 entrega 3.78\n");
    {
        Estado e; Config c; Log log;
        control(e, c, 40.0, 100.0, log);
        e.asked_sum = 2.33; e.asked_n = 1; control(e, c, 40.0, 2.47 * 40.0, log);
        chequear("tramo 2 aprende hacia 0.94 con ganancia 0.25: 0.985", x100(e.bias[2]), 99);
        e.asked_sum = 3.75; e.asked_n = 1; control(e, c, 40.0, 3.78 * 40.0, log);
        chequear("tramo 3 aprende aparte: 0.998", x100(e.bias[3]), 100);
        chequear("tramo 2 no se toco", x100(e.bias[2]), 99);
        e.recortado = true;
        e.asked_sum = 2.33; e.asked_n = 1; control(e, c, 40.0, 2.0 * 40.0, log);
        chequear("salida recortada: NO aprende (asi llego a 1.25 una vez)", x100(e.bias[2]), 99);
        e.recortado = false;
        for (int i = 0; i < 40; ++i) { e.asked_sum = 2.5; e.asked_n = 1; control(e, c, 40.0, 40.0 * 4.0, log); }
        chequear("entregando el doble de lo pedido: el sesgo topa en 0.80", x100(e.bias[2]), 80);
    }

    printf("\naplicar: objetivo 180 fps con base 37 (Cyberpunk, cp-dyn-steam-ok)\n");
    {
        Estado e; Config c; Log log;
        EntradaAplicar in{ 37.0, 180.0, 1000000, 1000000, 1000, 200 };
        SalidaAplicar s = aplicar(e, c, in, log);
        chequear("primer tick: 180/37 = 4.86 -> 486", s.cambia ? s.next : -1, 486);
        chequear("  no es una caida: sin sonda", s.sonda ? 1 : 0, 0);
        chequear("  el pedido en vigor (2.00) se acumula para el sesgo", x100(e.asked_sum), 200);
        // 10 ms despues, 2 presentaciones: la deuda se mueve 0.2, el ratio no
        // cruza la banda muerta.
        in.now_qpc += 10000; in.pc += 2; in.dyn_target = 486;
        s = aplicar(e, c, in, log);
        chequear("10 ms y 2 presentadas: dentro de la banda muerta, no cambia", s.cambia ? 1 : 0, 0);
        // Un salto de 0.6 s es un cambio de escena: la deuda se olvida.
        e.debt = 20.0;
        in.now_qpc += 600000; in.pc += 100;
        aplicar(e, c, in, log);
        chequear("salto de 0.6 s: la deuda vuelve a 0", x100(e.debt), 0);
    }
    {
        Estado e; Config c; Log log;
        EntradaAplicar in{ 20.0, 180.0, 1000000, 1000000, 1000, 486 };
        SalidaAplicar s = aplicar(e, c, in, log);
        chequear("base 20: 180/20 = 9 -> se recorta a 6.00", s.next, 600);
        chequear("  y queda marcado como recortado (el sesgo no aprende)", e.recortado ? 1 : 0, 1);
        chequear("  lo dice una vez", log.cuenta("dynamic: target needs more than 6x"), 1);
        in.now_qpc += 10000; in.pc += 1; in.dyn_target = 600;
        aplicar(e, c, in, log);
        chequear("  y no lo repite", log.cuenta("dynamic: target needs more than 6x"), 1);
    }
    {
        Estado e; Config c; Log log;
        e.sat_ratio_max = 4.0;
        EntradaAplicar in{ 37.0, 180.0, 1000000, 1000000, 1000, 200 };
        SalidaAplicar s = aplicar(e, c, in, log);
        chequear("con techo medido 4.00: 4.86 se recorta a 400", s.next, 400);
        chequear("  y lo dice", log.cuenta("dynamic: recortado al techo medido"), 1);
        c.sat_on = false;
        s = aplicar(e, c, in, log);
        chequear("sat apagado (mfg-sinsat): no recorta, 486", s.next, 486);
    }
    {
        Estado e; Config c; Log log;
        c.usar_deuda = false;
        e.debt = 30.0;
        EntradaAplicar in{ 37.0, 180.0, 1000000, 1000000, 1000, 200 };
        SalidaAplicar s = aplicar(e, c, in, log);
        chequear("sin integrador (mfg-sin-deuda): la deuda no entra, 486", s.next, 486);
    }
    {
        Estado e; Config c; Log log;
        EntradaAplicar in{ 37.0, 180.0, 1000000, 1000000, 1000, 580 };
        SalidaAplicar s = aplicar(e, c, in, log);
        chequear("de 5.80 a 4.86 es una caida de mas de 0.80: arranca la sonda", s.sonda ? 1 : 0, 1);
    }

    printf("\n%s\n", fallos == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return fallos ? 1 : 0;
}
