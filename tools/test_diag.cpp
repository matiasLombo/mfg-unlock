// Pincha src/diag.h: la forma exacta de la linea, porque los scripts de
// analisis van a hacer grep sobre ella.
//
//   g++ -std=c++20 -O2 -I src tools/test_diag.cpp -o /tmp/td.exe && /tmp/td.exe
#include <cstdio>
#include <cstring>
#include "diag.h"

using namespace diag;

static int failures = 0;

static void check(const char *label, const char *got, const char *expected) {
    const bool ok = strcmp(got, expected) == 0;
    if (!ok) ++failures;
    printf("  %-40s %s\n", label, ok ? "ok  " : "FALLA");
    if (!ok) printf("    dio:      %s\n    esperado: %s\n", got, expected);
}

int main(void) {
    printf("diagnostico -- una linea por invariante\n\n");
    {
        Line l = invariant(Layer::POLICY, "cuenta>=2", "con multiplicador es 1X, no genera");
        l.pair("sel", 8).pair("gen", 1).pair("objetivo", 255).pair("tope", 6).pair("correccion", 1);
        check("capa2 con cinco pares", l.b,
                 "INVARIANTE capa2/politica cuenta>=2: con multiplicador es 1X, no genera"
                 " sel=8 gen=1 objetivo=255 tope=6 correccion=1");
    }
    {
        Line l = invariant(Layer::IDENTITY, "copia-parcheada", "la copia que ejecuta no tiene todos los parches");
        l.pair("cuenta", 0).pair("pacer", 2);
        check("capa0", l.b,
                 "INVARIANTE capa0/identidad copia-parcheada: la copia que ejecuta no tiene todos los parches cuenta=0 pacer=2");
    }
    {
        Line l = verdict("PASIVO", "no se identifico que copia ejecuta");
        l.pair("copias", 2).pair("vivas", 1).pair("resuelto", 1);
        check("veredicto", l.b,
                 "VEREDICTO PASIVO: no se identifico que copia ejecuta copias=2 vivas=1 resuelto=1");
    }
    {
        Line l = invariant(Layer::PRESENTATION, "contador", "GetLastPresentCount fallo");
        l.pair("hr", -2005270527LL).text_pair("modo", "runtime");
        check("negativos y texto", l.b,
                 "INVARIANTE capa3/presentacion contador: GetLastPresentCount fallo hr=-2005270527 modo=runtime");
    }
    {
        // Nunca desborda: 300 caracteres de detalle se cortan a 255.
        char long_text[301]; memset(long_text, 'x', 300); long_text[300] = 0;
        Line l = invariant(Layer::SESSION, "x", long_text);
        l.pair("k", 1);
        check("largo acotado", l.n == 255 && strlen(l.b) == 255 ? "ok" : "mal", "ok");
    }
    printf("\n%s\n", failures == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return failures ? 1 : 0;
}
