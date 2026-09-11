// Pincha src/reparto.h simulando un juego: frames a una tasa que depende de
// la cuenta pedida, y se mide lo que el planificador entrega en promedio.
//
//   g++ -std=c++20 -O2 -I src tools/test_reparto.cpp -o /tmp/trp.exe && /tmp/trp.exe
#include <cstdio>
#include <cmath>
#include "reparto.h"

using namespace rep;

static int fallos = 0;

static void chequear(const char *caso, double got, double lo, double hi) {
    const bool ok = got >= lo && got <= hi;
    if (!ok) ++fallos;
    printf("  %-60s %s (dio %.4f, banda %.4f..%.4f)\n", caso, ok ? "ok  " : "FALLA", got, lo, hi);
}

struct Log { int cierres = 0; void num(const char *t, unsigned long long) { if (t[0] == 's') ++cierres; } void linea(const char *) {} };

// Corre `segundos` de juego. fps_de(cuenta) da la tasa de render con esa
// cuenta. Devuelve presentadas/renderizadas sobre la segunda mitad.
static double simular(double ratio, double (*fps_de)(long), double segundos, Config c,
                      Estado *estado_out = nullptr, int *cierres_out = nullptr) {
    Estado e; Log log;
    const double per_frame = ratio;             // kBase = 0 (multiplicador)
    const long lo = (long)per_frame;
    const double frac = per_frame - (double)lo;
    double t = 0.0, dt = 1.0 / fps_de(lo + 1);
    double pres = 0.0, ren = 0.0;
    while (t < segundos) {
        Entrada in{ per_frame, lo, frac, dt, dt };
        Salida s = tick(e, c, in, log);
        // el frame que sigue dura lo que tarda el juego con ESA cuenta
        dt = 1.0 / fps_de(s.api);
        t += dt;
        if (t > segundos / 2.0) { pres += (double)s.api; ren += 1.0; }
    }
    if (estado_out) *estado_out = e;
    if (cierres_out) *cierres_out = log.cierres;
    return ren > 0.0 ? pres / ren : 0.0;
}

static double plana(long) { return 40.0; }
// El bloque alto cuesta: a cuenta 3 el juego rinde 35, a cuenta 2 rinde 45.
static double cuesta(long n) { return n >= 3 ? 35.0 : 45.0; }

int main(void) {
    printf("reparto por bloques -- lo que entrega contra lo que se pide\n\n");
    Config c{ 0, 0, true, false, false, false };

    printf("base plana a 40 fps\n");
    {
        int cierres = 0;
        const double r = simular(2.55, plana, 120.0, c, nullptr, &cierres);
        // 18 de 32 bloques altos: 2.5625 exacto, es la cuantizacion de 1/32
        chequear("2.55 pedido -> 2.5625 (18/32 bloques altos), dentro del 1%", r, 2.55 * 0.99, 2.5625 * 1.01);
        chequear("hubo cierres de ciclo con datos", cierres, 100, 100000);
        chequear("2.10 -> 2.09..2.13", simular(2.10, plana, 120.0, c), 2.10 * 0.99, 2.125 * 1.01);
        chequear("2.90 -> 2.87..2.93", simular(2.90, plana, 120.0, c), 2.875 * 0.99, 2.90 * 1.01);
        chequear("3.00 exacto -> 3.00", simular(3.00, plana, 60.0, c), 3.0, 3.0);
        chequear("5.50 -> 5.50 (tope de la API es 5, el want llega a 6)", simular(5.50, plana, 120.0, c), 5.0, 5.5 * 1.01);
    }

    printf("\nel bloque alto renderiza mas lento (35 contra 45): el reparto lo corrige\n");
    {
        Estado e;
        const double r = simular(2.55, cuesta, 300.0, c, &e);
        // Sin correccion, 18 bloques a 35 fps y 14 a 45 pesan
        // (18*35*3 + 14*45*2) / (18*35 + 14*45) = 2.50. Con las tasas medidas
        // la particion sube a ~20 bloques y vuelve a 2.55.
        chequear("2.55 pedido -> 2.55 dentro del 1% con las tasas medidas", r, 2.55 * 0.99, 2.5625 * 1.01);
        chequear("  tasa medida del bloque bajo ~45", e.rate_lo, 43.0, 47.0);
        chequear("  tasa medida del bloque alto ~35", e.rate_hi, 33.0, 37.0);
        chequear("  bloques altos por ciclo: 20", e.hi_blocks, 19, 21);
    }

    printf("\nlatch: la particion se decide una vez por ciclo\n");
    {
        Estado e; Log log;
        Entrada in{ 2.55, 2, 0.55, 0.0, 0.025 };
        tick(e, c, in, log);
        chequear("primer tick decide 18", e.hi_blocks, 18, 18);
        in.frac = 0.30; in.per_frame = 2.30; in.dt = 0.025;
        tick(e, c, in, log);
        chequear("cambio chico a mitad de ciclo, con latch: sigue en 18", e.hi_blocks, 18, 18);
        Config sin = c; sin.latch = false;
        Estado e2; Estado e2b;
        in.frac = 0.55; in.per_frame = 2.55;
        tick(e2, sin, in, log);
        in.frac = 0.30; in.per_frame = 2.30;
        tick(e2, sin, in, log);
        chequear("sin latch (mfg-nolatch): recalcula ya, 10", e2.hi_blocks, 10, 10);
        (void)e2b;
        // Un salto grande (> 0.5) reinicia el ciclo con latch y todo.
        in.frac = 0.95; in.per_frame = 2.95;
        tick(e, c, in, log);
        chequear("salto grande con latch: reinicia y decide 30", e.hi_blocks, 30, 30);
        chequear("  y el reloj del ciclo vuelve a cero", e.sa_clock, 0.0, 0.03);
    }

    printf("\nvariantes de configuracion\n");
    {
        Config n = c; n.nullalt = true;
        chequear("nullalt: siempre lo+1", simular(2.55, plana, 30.0, n), 3.0, 3.0);
        Config d = c; d.peralt = true;
        chequear("peralt (difusion por frame): 2.55 exacto", simular(2.55, plana, 60.0, d), 2.55 * 0.995, 2.55 * 1.005);
        Config b = c; b.blocks = 8; b.block_ms = 50;
        chequear("8 bloques de 50 ms: 2.55 -> 2.50 (4/8) o 2.625 (5/8)", simular(2.55, plana, 120.0, b), 2.49, 2.63);
    }

    printf("\n%s\n", fallos == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return fallos ? 1 : 0;
}
