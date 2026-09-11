// Los swapchains adoptados (src/adopted.h), con los dos casos de GTA V del
// 11/09: uno muere y vuelve el otro; un transitorio que presenta menos no
// se elige aunque sea el mas nuevo.
//
//   g++ -std=c++20 -O2 -I src tools/test_adopted.cpp -o /tmp/ta.exe && /tmp/ta.exe
#include <cstdio>
#include "adopted.h"

static int failures = 0;
static void check(const char *label, long got, long expected) {
    const bool ok = got == expected;
    if (!ok) ++failures;
    printf("  %-66s %s (dio %ld, esperado %ld)\n", label, ok ? "ok  " : "FALLA", got, expected);
}

// Dos swapchains falsos: A presenta 1 por frame, B (el transitorio) 1 de cada 3.
static int A = 0, B = 0;
static bool alive_a = true, alive_b = true;
static unsigned count_a = 0, count_b = 0;
static bool alive(void *c) { return c == &A ? alive_a : alive_b; }
static bool count(void *c, unsigned *out) { *out = c == &A ? count_a : count_b; return true; }

int main(void) {
    using namespace adopted;
    printf("adopted: la lista\n");
    {
        List l;
        l.adopt(&A);
        check("uno adoptado: es el actual", l.current() == &A, 1);
        l.adopt(&B);
        check("el segundo va primero en la lista (mas nuevo)", l.e[0].chain == &B, 1);
        check("  pero el actual sigue siendo el primero elegido", l.current() == &A, 1);
        l.adopt(&A);
        check("adoptar de nuevo no duplica", l.n, 2);
        l.forget(&A);
        check("olvidar al actual pasa al que queda", l.current() == &B, 1);
        l.forget(&B);
        check("sin adoptados: nullptr", l.current() == nullptr, 1);
    }

    printf("\nGTA V 11/09, 111 s: el adoptado se destruyo -> vuelve el anterior, sin tocarlo\n");
    {
        List l;
        alive_a = alive_b = true; count_a = count_b = 0;
        l.adopt(&A); l.adopt(&B);
        bool dropped = false; unsigned c = 0;
        // 50 frames: B (el nuevo) presenta cada frame, A tambien; empate -> el mas nuevo
        for (int f = 0; f < 50; ++f) { ++count_a; ++count_b; l.pick(alive, count, &c, &dropped); }
        check("con los dos presentando igual, se elige el mas nuevo (B)", l.current() == &B, 1);
        alive_b = false;
        void *p = l.pick(alive, count, &c, &dropped);
        check("B muerto: se suelta y devuelve A", p == &A, 1);
        check("  y lo dice", dropped, 1);
        check("  la lista queda con uno", l.n, 1);
    }

    printf("\nGTA V 11/09, 70-131 s: el transitorio presenta a un tercio -> no se elige\n");
    {
        List l;
        alive_a = alive_b = true; count_a = count_b = 0;
        l.adopt(&A);            // el del juego, viejo
        l.adopt(&B);            // el transitorio, nuevo
        unsigned c = 0; bool d = false;
        for (int f = 0; f < 3 * kPeriod; ++f) {
            ++count_a;
            if (f % 3 == 0) ++count_b;
            l.pick(alive, count, &c, &d);
        }
        check("tras un periodo manda el que mas presento (A)", l.current() == &A, 1);
        check("  y la cuenta devuelta es la de A", c == count_a, 1);
    }

    printf("\nel primer periodo, antes de decidir: el mas nuevo, sin leer basura\n");
    {
        List l;
        alive_a = alive_b = true; count_a = 1000; count_b = 5;
        l.adopt(&A); l.adopt(&B);
        unsigned c = 0;
        l.pick(alive, count, &c, nullptr);
        check("primera lectura: no hay delta todavia, elegido = primero adoptado", l.current() == &A, 1);
        check("  cuenta del elegido", c, 1000);
    }

    printf("\n%s\n", failures == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return failures ? 1 : 0;
}
