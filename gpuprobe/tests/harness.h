// harness.h -- el minimo para que un test sea un main() que devuelve 0 o 1.
// Sin framework: los tests de host se compilan con g++ plano y corren en CI
// sin instalar nada, igual que tools/test-host.sh en la raiz del repo.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gp_test {

inline int g_failures = 0;
inline int g_checks = 0;

inline void fail(const char *file, int line, const char *expr) {
    ++g_failures;
    std::fprintf(stderr, "  FALLO %s:%d: %s\n", file, line, expr);
}

inline int report(const char *name) {
    std::printf("%s: %d checks, %d fallos -- %s\n", name, g_checks, g_failures,
                g_failures == 0 ? "OK" : "ROJO");
    return g_failures == 0 ? 0 : 1;
}

}  // namespace gp_test

#define CHECK(expr)                                                    \
    do {                                                               \
        ++gp_test::g_checks;                                           \
        if (!(expr)) gp_test::fail(__FILE__, __LINE__, #expr);         \
    } while (0)

#define CHECK_EQ_U64(a, b)                                                     \
    do {                                                                       \
        ++gp_test::g_checks;                                                   \
        const std::uint64_t va_ = (std::uint64_t)(a);                          \
        const std::uint64_t vb_ = (std::uint64_t)(b);                          \
        if (va_ != vb_) {                                                      \
            char buf_[160];                                                    \
            std::snprintf(buf_, sizeof buf_, "%s == %s (0x%llx vs 0x%llx)",    \
                          #a, #b, (unsigned long long)va_,                     \
                          (unsigned long long)vb_);                            \
            gp_test::fail(__FILE__, __LINE__, buf_);                           \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        ++gp_test::g_checks;                                                   \
        const double va_ = (double)(a), vb_ = (double)(b);                     \
        const double d_ = va_ > vb_ ? va_ - vb_ : vb_ - va_;                   \
        if (!(d_ <= (double)(tol))) {                                          \
            char buf_[160];                                                    \
            std::snprintf(buf_, sizeof buf_, "%s ~= %s (%g vs %g)", #a, #b,    \
                          va_, vb_);                                           \
            gp_test::fail(__FILE__, __LINE__, buf_);                           \
        }                                                                      \
    } while (0)
