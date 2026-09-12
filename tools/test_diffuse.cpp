// El nucleo puro de fracdiff (scheduler::diffuse_step): la difusion por frame
// promedia R y reparte los +1 lo mas parejo posible (propiedad Euclidiana).
// Testeable sin GPU, antes de tocar el plugin. Ver
// docs/investigacion-fraccionales.md.
//
//   g++ -std=c++20 -O2 -I src tools/test_diffuse.cpp -o /tmp/td.exe && /tmp/td.exe
#include <cstdio>
#include <cmath>
#include <vector>
#include "scheduler.h"
using scheduler::diffuse_step;

static int failures = 0;
static void check(const char *label, bool ok, const char *detail) {
    if (!ok) ++failures;
    printf("  %-58s %s  %s\n", label, ok ? "ok  " : "FALLA", detail);
}

// La propiedad de maxima evenness (Euclidiano/Bresenham): entre dos frames
// "altos" consecutivos la distancia toma a lo sumo dos valores, y difieren en 1.
static bool max_even(const std::vector<int> &seq, int hi) {
    std::vector<int> gaps;
    int last = -1;
    for (int i = 0; i < (int)seq.size(); ++i)
        if (seq[i] == hi) { if (last >= 0) gaps.push_back(i - last); last = i; }
    if (gaps.size() < 2) return true;
    int mn = gaps[0], mx = gaps[0];
    for (int g : gaps) { if (g < mn) mn = g; if (g > mx) mx = g; }
    return mx - mn <= 1;
}

static void run(double R) {
    const int N = 4000;
    double acc = 0.0;
    std::vector<int> seq;
    long sum = 0;
    const int lo = (int)R, hi = (int)R + 1;
    for (int i = 0; i < N; ++i) {
        long n = diffuse_step(acc, R);
        seq.push_back((int)n);
        sum += n;
        if (n != lo && n != hi) { check("solo lo o lo+1", false, ""); return; }
    }
    const double mean = (double)sum / N;
    char d[80];
    snprintf(d, sizeof d, "R=%.2f  media=%.4f", R, mean);
    check("la media sigue R (0.5%)", std::fabs(mean - R) < 0.005 * (R > 0 ? R : 1), d);
    check("  reparto de maxima evenness (Euclidiano)", max_even(seq, hi), d);
}

// La composicion que hace fracres: diffuse_step topado al max declarado por el
// juego (count_cap). El valor escrito nunca debe salir de [0, cap] -- es lo que
// evita el crash de escribir mas slots de los reservados (ngx-caps-the-count).
// Con R <= cap promedia R exacto; con R > cap se clava en cap (se pierde ratio,
// no se gana un crash).
static void run_capped(double R, long cap) {
    const int N = 4000;
    double acc = 0.0;
    long sum = 0, mn = 1000, mx = -1000;
    for (int i = 0; i < N; ++i) {
        long n = diffuse_step(acc, R);
        if (n > cap) n = cap;          // el tope de fracres (count_cap)
        if (n < 0) n = 0;
        if (n < mn) mn = n;
        if (n > mx) mx = n;
        sum += n;
    }
    char d[96];
    const double mean = (double)sum / N;
    const double target = R <= (double)cap ? R : (double)cap;
    snprintf(d, sizeof d, "R=%.2f cap=%ld  media=%.4f  [%ld,%ld]", R, cap, mean, mn, mx);
    check("  nunca sale de [0,cap]", mn >= 0 && mx <= cap, d);
    check("  media = min(R,cap) (0.5%)", std::fabs(mean - target) < 0.005 * (target > 0 ? target : 1), d);
}

int main(void) {
    printf("diffuse_step: promedia R y reparte parejo\n");
    for (double R : { 2.25, 2.50, 2.75, 3.10, 3.50, 4.33, 5.50, 5.90 }) run(R);

    printf("\ncomposicion de fracres: diffuse topado a count_cap\n");
    run_capped(2.50, 6);     // holgado: promedia 2.5
    run_capped(3.50, 3);     // Halo: cap 3, R>cap -> se clava en 3, sin pasarse
    run_capped(2.50, 3);     // Halo con fraccional dentro del cap: 2.5 exacto
    run_capped(5.50, 6);     // banda alta con techo 6: 5.5 exacto
    run_capped(5.50, 5);     // techo 5: se clava en 5

    printf("\nborde: entero exacto no alterna\n");
    {
        double acc = 0.0; bool alt = false;
        for (int i = 0; i < 100; ++i) if (diffuse_step(acc, 3.0) != 3) alt = true;
        check("R=3.00: siempre 3, sin +1", !alt, "");
    }
    printf("\nun escalon por frame como maximo (nunca salta 2)\n");
    {
        double acc = 0.0; bool jump = false;
        for (int i = 0; i < 200; ++i) { long n = diffuse_step(acc, 2.9); if (n > 3) jump = true; }
        check("R=2.90: nunca 4 (un escalon)", !jump, "");
    }

    printf("\n%s\n", failures == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return failures ? 1 : 0;
}
