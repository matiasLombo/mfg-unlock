// stats.h -- la estadistica del harness A/B. Puro y testeable.
//
// ENTRA: dos muestras de frametime (ON y OFF), ya sin los frames de warmup.
// SALE: la diferencia de medianas en ms, un intervalo de confianza y un
//   p-valor. Y el veredicto: si el intervalo cruza cero, el candidato SE
//   DESCARTA.
// DEPENDE DE: <cmath>, <vector>, <algorithm>.
//
// Por que mediana y no promedio: los frametimes tienen cola derecha (un hitch
// de 40 ms en 600 frames mueve el promedio y no dice nada de la pasada que
// estamos midiendo). Por que Mann-Whitney y no un t-test: no asume normalidad,
// que es justo lo que los frametimes no son.
//
// Y por que esto existe: la "ganancia estimada" del analizador es una cuenta
// sobre pixeles. El numero que va al reporte sale de aca, de medir el juego
// con la optimizacion prendida y apagada alternando cada pocos frames -- lo
// unico inmune al drift de clocks de la GPU, que en diez minutos de gameplay
// mueve los ms mas que muchas de las optimizaciones que buscamos.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gp {

inline double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const size_t n = v.size();
    std::nth_element(v.begin(), v.begin() + static_cast<long>(n / 2), v.end());
    const double hi = v[n / 2];
    if (n % 2) return hi;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(n / 2 - 1), v.end());
    return (v[n / 2 - 1] + hi) / 2.0;
}

inline double percentile_of(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = p * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = lo + 1 < v.size() ? lo + 1 : lo;
    const double frac = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

// RNG propio y determinista: dos corridas del analizador sobre la misma sesion
// tienen que dar el mismo intervalo, o el reporte no es reproducible.
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed = 0x9E3779B97F4A7C15ull) : s(seed ? seed : 1) {}
    std::uint64_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return s;
    }
    size_t below(size_t n) { return static_cast<size_t>(next() % n); }
};

struct ABResult {
    size_t n_on = 0, n_off = 0;
    double median_on = 0.0;     // frametime con la optimizacion prendida
    double median_off = 0.0;
    double delta_ms = 0.0;      // off - on: positivo = la optimizacion GANA
    double ci_lo = 0.0, ci_hi = 0.0;   // IC 95% de delta_ms
    double p_value = 1.0;
    double u_statistic = 0.0;
    bool   significant = false; // p < alpha Y el IC no cruza cero
    // Cuanto se mueve el ruido de la propia medicion: si delta_ms es mas chico
    // que esto, la ganancia no se puede distinguir del jitter frame a frame.
    double noise_ms = 0.0;
};

// Mann-Whitney U con correccion por empates y aproximacion normal. Devuelve el
// p-valor a dos colas. Con menos de 8 muestras por lado la aproximacion no
// vale y devolvemos 1.0 -- preferimos "no se puede decir" a un falso positivo.
inline double mann_whitney_p(const std::vector<double> &a,
                             const std::vector<double> &b, double *u_out) {
    const size_t na = a.size(), nb = b.size();
    if (u_out) *u_out = 0.0;
    if (na < 8 || nb < 8) return 1.0;

    // Rangos sobre la muestra unida, promediando los empates.
    std::vector<std::pair<double, int>> all;
    all.reserve(na + nb);
    for (double x : a) all.emplace_back(x, 0);
    for (double x : b) all.emplace_back(x, 1);
    std::sort(all.begin(), all.end(),
              [](const auto &x, const auto &y) { return x.first < y.first; });

    std::vector<double> rank(all.size());
    double tie_sum = 0.0;
    for (size_t i = 0; i < all.size();) {
        size_t j = i;
        while (j + 1 < all.size() && all[j + 1].first == all[i].first) ++j;
        const double r = (static_cast<double>(i) + static_cast<double>(j)) / 2.0 + 1.0;
        for (size_t k = i; k <= j; ++k) rank[k] = r;
        const double t = static_cast<double>(j - i + 1);
        if (t > 1) tie_sum += t * t * t - t;
        i = j + 1;
    }

    double ra = 0.0;
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].second == 0) ra += rank[i];

    const double dna = static_cast<double>(na), dnb = static_cast<double>(nb);
    const double ua = ra - dna * (dna + 1.0) / 2.0;
    const double ub = dna * dnb - ua;
    const double u = std::min(ua, ub);
    if (u_out) *u_out = u;

    const double mu = dna * dnb / 2.0;
    const double n = dna + dnb;
    const double sigma2 = (dna * dnb / 12.0) *
                          ((n + 1.0) - tie_sum / (n * (n - 1.0)));
    if (sigma2 <= 0.0) return 1.0;
    // Correccion de continuidad: sin ella los p chicos salen optimistas.
    const double z = (std::fabs(u - mu) - 0.5) / std::sqrt(sigma2);
    if (z <= 0.0) return 1.0;
    return std::erfc(z / std::sqrt(2.0));
}

// IC por bootstrap de percentiles sobre la diferencia de medianas.
inline void bootstrap_ci(const std::vector<double> &on,
                         const std::vector<double> &off, double &lo, double &hi,
                         int iters = 2000, std::uint64_t seed = 12345) {
    lo = hi = 0.0;
    if (on.size() < 4 || off.size() < 4) return;
    Rng rng(seed);
    std::vector<double> deltas;
    deltas.reserve(static_cast<size_t>(iters));
    std::vector<double> sa(on.size()), sb(off.size());
    for (int it = 0; it < iters; ++it) {
        for (size_t i = 0; i < on.size(); ++i) sa[i] = on[rng.below(on.size())];
        for (size_t i = 0; i < off.size(); ++i) sb[i] = off[rng.below(off.size())];
        deltas.push_back(median_of(sb) - median_of(sa));
    }
    std::sort(deltas.begin(), deltas.end());
    lo = deltas[static_cast<size_t>(0.025 * static_cast<double>(deltas.size() - 1))];
    hi = deltas[static_cast<size_t>(0.975 * static_cast<double>(deltas.size() - 1))];
}

// El ruido propio de la medicion: la mediana de |x[i] - x[i-1]| dentro de la
// misma condicion. Es lo que se mueve el frametime sin que nadie toque nada.
inline double frame_noise(const std::vector<double> &v) {
    if (v.size() < 3) return 0.0;
    std::vector<double> d;
    d.reserve(v.size() - 1);
    for (size_t i = 1; i < v.size(); ++i) d.push_back(std::fabs(v[i] - v[i - 1]));
    return median_of(d);
}

inline ABResult ab_compare(const std::vector<double> &on,
                           const std::vector<double> &off, double alpha = 0.01) {
    ABResult r;
    r.n_on = on.size();
    r.n_off = off.size();
    if (on.empty() || off.empty()) return r;
    r.median_on = median_of(on);
    r.median_off = median_of(off);
    r.delta_ms = r.median_off - r.median_on;
    r.p_value = mann_whitney_p(on, off, &r.u_statistic);
    bootstrap_ci(on, off, r.ci_lo, r.ci_hi);
    r.noise_ms = std::max(frame_noise(on), frame_noise(off));
    // Tres condiciones, y las tres tienen que darse: el test dice que la
    // diferencia existe, el intervalo no cruza cero (o sea que el signo es
    // seguro), y la ganancia es positiva. Una "ganancia" negativa significa
    // que la optimizacion EMPEORA el frametime, y eso tambien hay que verlo.
    r.significant = r.p_value < alpha && r.ci_lo > 0.0 && r.delta_ms > 0.0;
    return r;
}

}  // namespace gp
