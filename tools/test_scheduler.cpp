// Pincha src/reparto.h simulando un juego: frames a una tasa que depende de
// la cuenta pedida, y se mide lo que el planificador entrega en promedio.
//
//   g++ -std=c++20 -O2 -I src tools/test_reparto.cpp -o /tmp/trp.exe && /tmp/trp.exe
#include <cstdio>
#include <cmath>
#include "scheduler.h"

using namespace scheduler;

static int failures = 0;

static void check(const char *label, double got, double lo, double hi) {
    const bool ok = got >= lo && got <= hi;
    if (!ok) ++failures;
    printf("  %-60s %s (dio %.4f, banda %.4f..%.4f)\n", label, ok ? "ok  " : "FALLA", got, lo, hi);
}

struct Log { int closures = 0; void num(const char *t, unsigned long long) { if (t[0] == 's') ++closures; } void line(const char *) {} };

// Corre `segundos` de juego. fps_de(cuenta) da la tasa de render con esa
// cuenta. Devuelve presentadas/renderizadas sobre la segunda mitad.
static double simulate(double ratio, double (*fps_de)(long), double seconds, Config c,
                      State *state_out = nullptr, int *closures_out = nullptr) {
    State e; Log log;
    const double per_frame = ratio;             // kBase = 0 (multiplicador)
    const long lo = (long)per_frame;
    const double frac = per_frame - (double)lo;
    double t = 0.0, dt = 1.0 / fps_de(lo + 1);
    double presented = 0.0, rendered = 0.0;
    while (t < seconds) {
        Input in{ per_frame, lo, frac, dt, dt };
        Output s = tick(e, c, in, log);
        // el frame que sigue dura lo que tarda el juego con ESA cuenta
        dt = 1.0 / fps_de(s.api);
        t += dt;
        if (t > seconds / 2.0) { presented += (double)s.api; rendered += 1.0; }
    }
    if (state_out) *state_out = e;
    if (closures_out) *closures_out = log.closures;
    return rendered > 0.0 ? presented / rendered : 0.0;
}

static double flat(long) { return 40.0; }
// El bloque alto cuesta: a cuenta 3 el juego rinde 35, a cuenta 2 rinde 45.
static double costly(long n) { return n >= 3 ? 35.0 : 45.0; }

int main(void) {
    printf("reparto por bloques -- lo que entrega contra lo que se pide\n\n");
    Config c{ 0, 0, true, false, false, false };

    printf("base plana a 40 fps\n");
    {
        int closures = 0;
        const double r = simulate(2.55, flat, 120.0, c, nullptr, &closures);
        // 18 de 32 bloques altos: 2.5625 exacto, es la cuantizacion de 1/32
        check("2.55 pedido -> 2.5625 (18/32 bloques altos), dentro del 1%", r, 2.55 * 0.99, 2.5625 * 1.01);
        check("hubo cierres de ciclo con datos", closures, 100, 100000);
        check("2.10 -> 2.09..2.13", simulate(2.10, flat, 120.0, c), 2.10 * 0.99, 2.125 * 1.01);
        check("2.90 -> 2.87..2.93", simulate(2.90, flat, 120.0, c), 2.875 * 0.99, 2.90 * 1.01);
        check("3.00 exacto -> 3.00", simulate(3.00, flat, 60.0, c), 3.0, 3.0);
        check("5.50 -> 5.50 (tope de la API es 5, el want llega a 6)", simulate(5.50, flat, 120.0, c), 5.0, 5.5 * 1.01);
    }

    printf("\nel bloque alto renderiza mas lento (35 contra 45): el reparto lo corrige\n");
    {
        State e;
        const double r = simulate(2.55, costly, 300.0, c, &e);
        // Sin correccion, 18 bloques a 35 fps y 14 a 45 pesan
        // (18*35*3 + 14*45*2) / (18*35 + 14*45) = 2.50. Con las tasas medidas
        // la particion sube a ~20 bloques y vuelve a 2.55.
        check("2.55 pedido -> 2.55 dentro del 1% con las tasas medidas", r, 2.55 * 0.99, 2.5625 * 1.01);
        check("  tasa medida del bloque bajo ~45", e.rate_lo, 43.0, 47.0);
        check("  tasa medida del bloque alto ~35", e.rate_hi, 33.0, 37.0);
        check("  bloques altos por ciclo: 20", e.hi_blocks, 19, 21);
    }

    printf("\nlatch: la particion se decide una vez por ciclo\n");
    {
        State e; Log log;
        Input in{ 2.55, 2, 0.55, 0.0, 0.025 };
        tick(e, c, in, log);
        check("primer tick decide 18", e.hi_blocks, 18, 18);
        in.frac = 0.30; in.per_frame = 2.30; in.dt = 0.025;
        tick(e, c, in, log);
        check("cambio chico a mitad de ciclo, con latch: sigue en 18", e.hi_blocks, 18, 18);
        Config sin = c; sin.latch = false;
        State e2; State e2b;
        in.frac = 0.55; in.per_frame = 2.55;
        tick(e2, sin, in, log);
        in.frac = 0.30; in.per_frame = 2.30;
        tick(e2, sin, in, log);
        check("sin latch (mfg-nolatch): recalcula ya, 10", e2.hi_blocks, 10, 10);
        (void)e2b;
        // Un salto grande (> 0.5) reinicia el ciclo con latch y todo.
        in.frac = 0.95; in.per_frame = 2.95;
        tick(e, c, in, log);
        check("salto grande con latch: reinicia y decide 30", e.hi_blocks, 30, 30);
        check("  y el reloj del ciclo vuelve a cero", e.sa_clock, 0.0, 0.03);
    }

    printf("\nvariantes de configuracion\n");
    {
        Config n = c; n.nullalt = true;
        check("nullalt: siempre lo+1", simulate(2.55, flat, 30.0, n), 3.0, 3.0);
        Config d = c; d.peralt = true;
        check("peralt (difusion por frame): 2.55 exacto", simulate(2.55, flat, 60.0, d), 2.55 * 0.995, 2.55 * 1.005);
        Config b = c; b.blocks = 8; b.block_ms = 50;
        check("8 bloques de 50 ms: 2.55 -> 2.50 (4/8) o 2.625 (5/8)", simulate(2.55, flat, 120.0, b), 2.49, 2.63);
    }

    printf("\n%s\n", failures == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return failures ? 1 : 0;
}
