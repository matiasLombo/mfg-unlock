// Los swapchains adoptados (src/adopted.h): se cuenta el mas nuevo, solo se
// toca ese, y cuando muere vuelve el anterior sin haberlo tocado antes.
//
// La version que elegia "el que mas presento" leia todos los adoptados y
// crasheo GTA V al pasar del menu al juego (ntdll+0xfa7d, seccion critica en
// memoria liberada): aca se comprueba que pick NO llama a alive/count sobre
// los que no son el elegido.
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

static int A = 0, B = 0;
static bool alive_a = true, alive_b = true;
static unsigned count_a = 0, count_b = 0;
static int touched_a = 0, touched_b = 0;
static bool alive(void *c) { if (c == &A) { ++touched_a; return alive_a; } ++touched_b; return alive_b; }
static bool count(void *c, unsigned *out) { *out = c == &A ? count_a : count_b; return true; }

int main(void) {
    using namespace adopted;
    printf("adopted: la lista\n");
    {
        List l;
        l.adopt(&A);
        check("uno adoptado: es el actual", l.current() == &A, 1);
        l.adopt(&B);
        check("el segundo pasa a ser el actual (mas nuevo)", l.current() == &B, 1);
        l.adopt(&A);
        check("adoptar de nuevo no duplica", l.n, 2);
        l.forget(&B);
        check("olvidar al actual pasa al que queda", l.current() == &A, 1);
        l.forget(&A);
        check("sin adoptados: nullptr", l.current() == nullptr, 1);
    }

    printf("\nGTA V 11/09 19:0x: el viejo NO se toca mientras el nuevo vive\n");
    {
        List l;
        alive_a = alive_b = true; count_a = 7; count_b = 3; touched_a = touched_b = 0;
        l.adopt(&A); l.adopt(&B);
        unsigned c = 0; bool d = false;
        for (int f = 0; f < 100; ++f) l.pick(alive, count, &c, &d);
        check("se cuenta el mas nuevo (B)", l.current() == &B, 1);
        check("  y su cuenta", c, 3);
        check("  A no se toco ni una vez", touched_a, 0);
    }

    printf("\nGTA V 11/09, 111 s: el elegido se destruyo -> vuelve el anterior\n");
    {
        List l;
        alive_a = alive_b = true; count_a = 7; count_b = 3;
        l.adopt(&A); l.adopt(&B);
        unsigned c = 0; bool d = false;
        alive_b = false;
        void *p = l.pick(alive, count, &c, &d);
        check("B muerto: se suelta y devuelve A", p == &A, 1);
        check("  lo dice", d, 1);
        check("  la cuenta es la de A", c, 7);
        check("  la lista queda con uno", l.n, 1);
        alive_a = false;
        p = l.pick(alive, count, &c, &d);
        check("A muerto tambien: nullptr", p == nullptr, 1);
        check("  lista vacia", l.n, 0);
    }

    printf("\n%s\n", failures == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return failures ? 1 : 0;
}
