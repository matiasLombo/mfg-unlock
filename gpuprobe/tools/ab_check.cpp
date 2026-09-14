// ab_check.cpp -- imprime el veredicto A/B de dos muestras sinteticas usando
// core/stats.h, para poder compararlo con el del analizador en Python.
//
//   g++ -std=c++20 -I gpuprobe gpuprobe/tools/ab_check.cpp -o ab_check
//   ./ab_check <base_on> <base_off> <n> <seed_on> <seed_off>
//
// Existe por una razon concreta: el overlay decide EN VIVO si un candidato
// gana (C++), y el reporte lo decide OFFLINE (Python). Si las dos
// implementaciones no dan lo mismo sobre las mismas muestras, el usuario ve un
// numero en pantalla y otro en el reporte, y deja de creerle a los dos. El
// generador de muestras es el mismo xorshift64 en los dos lados, asi que la
// comparacion es exacta y no "parecida".
#include "core/stats.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace gp;

static std::vector<double> synth(double base, size_t n, std::uint64_t seed,
                                 double jitter = 0.35) {
    Rng rng(seed);
    std::vector<double> v;
    v.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const double u = static_cast<double>(rng.next() % 10000) / 10000.0;
        v.push_back(base + (u - 0.5) * 2.0 * jitter);
    }
    return v;
}

int main(int argc, char **argv) {
    const double base_on = argc > 1 ? std::atof(argv[1]) : 7.0;
    const double base_off = argc > 2 ? std::atof(argv[2]) : 8.0;
    const size_t n = argc > 3 ? static_cast<size_t>(std::atoi(argv[3])) : 400;
    const std::uint64_t s_on = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 3;
    const std::uint64_t s_off = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 4;

    const ABResult r = ab_compare(synth(base_on, n, s_on),
                                  synth(base_off, n, s_off));
    std::printf("median_on %.9f\n", r.median_on);
    std::printf("median_off %.9f\n", r.median_off);
    std::printf("delta_ms %.9f\n", r.delta_ms);
    std::printf("ci_lo %.9f\n", r.ci_lo);
    std::printf("ci_hi %.9f\n", r.ci_hi);
    std::printf("p_value %.9g\n", r.p_value);
    std::printf("significant %d\n", r.significant ? 1 : 0);
    return 0;
}
