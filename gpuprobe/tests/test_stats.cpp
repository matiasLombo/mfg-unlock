// test_stats.cpp -- la estadistica que decide si un candidato entra al reporte.
//
// El test que mas importa es el NEGATIVO: dos muestras del mismo juego sin
// tocar nada tienen que dar "no significativo". Un harness que encuentra
// ganancias donde no las hay es peor que no tener harness.
#include "../core/stats.h"

#include "harness.h"

#include <vector>

using namespace gp;

// Frametimes sinteticos con cola derecha, como los de verdad: una base, jitter,
// y un hitch cada tanto.
static std::vector<double> sample(double base, size_t n, std::uint64_t seed,
                                  double jitter = 0.35, double hitch = 0.0) {
    Rng rng(seed);
    std::vector<double> v;
    v.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const double u = static_cast<double>(rng.next() % 10000) / 10000.0;
        double x = base + (u - 0.5) * 2.0 * jitter;
        if (hitch > 0.0 && (rng.next() % 100) == 0) x += hitch;
        v.push_back(x);
    }
    return v;
}

int main() {
    // --- mediana y percentiles ------------------------------------------
    CHECK_NEAR(median_of({1, 2, 3}), 2.0, 1e-9);
    CHECK_NEAR(median_of({1, 2, 3, 4}), 2.5, 1e-9);
    CHECK_NEAR(median_of({}), 0.0, 1e-9);
    CHECK_NEAR(percentile_of({1, 2, 3, 4, 5}, 0.0), 1.0, 1e-9);
    CHECK_NEAR(percentile_of({1, 2, 3, 4, 5}, 1.0), 5.0, 1e-9);
    CHECK_NEAR(percentile_of({1, 2, 3, 4, 5}, 0.5), 3.0, 1e-9);
    // La mediana ignora el hitch; el promedio no. Por eso usamos mediana.
    CHECK_NEAR(median_of({8.0, 8.1, 8.2, 8.3, 60.0}), 8.2, 1e-9);

    // --- el caso negativo: no tocamos nada, no hay ganancia --------------
    {
        const std::vector<double> a = sample(8.0, 600, 1, 0.35, 30.0);
        const std::vector<double> b = sample(8.0, 600, 2, 0.35, 30.0);
        const ABResult r = ab_compare(a, b);
        CHECK(!r.significant);
        CHECK(r.p_value > 0.01);
        // El intervalo tiene que CONTENER al cero.
        CHECK(r.ci_lo <= 0.0 && r.ci_hi >= 0.0);
    }

    // --- una ganancia real de 1 ms se detecta ----------------------------
    {
        const std::vector<double> on  = sample(7.0, 600, 3, 0.35, 30.0);
        const std::vector<double> off = sample(8.0, 600, 4, 0.35, 30.0);
        const ABResult r = ab_compare(on, off);
        CHECK(r.significant);
        CHECK_NEAR(r.delta_ms, 1.0, 0.2);
        CHECK(r.ci_lo > 0.0);
        CHECK(r.p_value < 0.001);
    }

    // --- una "optimizacion" que EMPEORA no pasa como ganancia ------------
    {
        const std::vector<double> on  = sample(9.0, 400, 5);
        const std::vector<double> off = sample(8.0, 400, 6);
        const ABResult r = ab_compare(on, off);
        CHECK(!r.significant);
        CHECK(r.delta_ms < 0.0);
    }

    // --- una ganancia mas chica que el jitter no se declara --------------
    // 0.02 ms de diferencia con 0.5 ms de jitter y 60 muestras: no alcanza.
    {
        const std::vector<double> on  = sample(7.98, 60, 7, 0.5);
        const std::vector<double> off = sample(8.00, 60, 8, 0.5);
        const ABResult r = ab_compare(on, off);
        CHECK(!r.significant);
    }

    // --- muestras chicas: preferimos "no se sabe" a un falso positivo ----
    {
        const std::vector<double> on  = {7.0, 7.1, 7.0, 6.9};
        const std::vector<double> off = {9.0, 9.1, 9.0, 8.9};
        const ABResult r = ab_compare(on, off);
        CHECK(!r.significant);
        CHECK_NEAR(r.p_value, 1.0, 1e-9);
    }

    // --- determinismo: dos corridas, el mismo intervalo ------------------
    {
        const std::vector<double> on  = sample(7.0, 200, 9);
        const std::vector<double> off = sample(8.0, 200, 10);
        const ABResult r1 = ab_compare(on, off);
        const ABResult r2 = ab_compare(on, off);
        CHECK_NEAR(r1.ci_lo, r2.ci_lo, 1e-12);
        CHECK_NEAR(r1.ci_hi, r2.ci_hi, 1e-12);
        CHECK_NEAR(r1.p_value, r2.p_value, 1e-12);
    }

    // --- empates masivos (frametime clavado por vsync) no rompen el test --
    {
        const std::vector<double> on(300, 16.666);
        const std::vector<double> off(300, 16.666);
        const ABResult r = ab_compare(on, off);
        CHECK(!r.significant);
        CHECK_NEAR(r.delta_ms, 0.0, 1e-9);
    }

    // --- el ruido frame a frame se mide ----------------------------------
    {
        const std::vector<double> flat(100, 8.0);
        CHECK_NEAR(frame_noise(flat), 0.0, 1e-9);
        const std::vector<double> jumpy = sample(8.0, 400, 11, 1.0);
        CHECK(frame_noise(jumpy) > 0.1);
    }

    return gp_test::report("test_stats");
}
