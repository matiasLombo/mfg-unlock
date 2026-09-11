// Pincha src/diag.h: la forma exacta de la linea, porque los scripts de
// analisis van a hacer grep sobre ella.
//
//   g++ -std=c++20 -O2 -I src tools/test_diag.cpp -o /tmp/td.exe && /tmp/td.exe
#include <cstdio>
#include <cstring>
#include "diag.h"

using namespace diag;

static int fallos = 0;

static void chequear(const char *caso, const char *got, const char *esperado) {
    const bool ok = strcmp(got, esperado) == 0;
    if (!ok) ++fallos;
    printf("  %-40s %s\n", caso, ok ? "ok  " : "FALLA");
    if (!ok) printf("    dio:      %s\n    esperado: %s\n", got, esperado);
}

int main(void) {
    printf("diagnostico -- una linea por invariante\n\n");
    {
        Linea l = invariante(Capa::POLITICA, "cuenta>=2", "con multiplicador es 1X, no genera");
        l.par("sel", 8).par("gen", 1).par("objetivo", 255).par("tope", 6).par("correccion", 1);
        chequear("capa2 con cinco pares", l.b,
                 "INVARIANTE capa2/politica cuenta>=2: con multiplicador es 1X, no genera"
                 " sel=8 gen=1 objetivo=255 tope=6 correccion=1");
    }
    {
        Linea l = invariante(Capa::IDENTIDAD, "copia-parcheada", "la copia que ejecuta no tiene todos los parches");
        l.par("cuenta", 0).par("pacer", 2);
        chequear("capa0", l.b,
                 "INVARIANTE capa0/identidad copia-parcheada: la copia que ejecuta no tiene todos los parches cuenta=0 pacer=2");
    }
    {
        Linea l = veredicto("PASIVO", "no se identifico que copia ejecuta");
        l.par("copias", 2).par("vivas", 1).par("resuelto", 1);
        chequear("veredicto", l.b,
                 "VEREDICTO PASIVO: no se identifico que copia ejecuta copias=2 vivas=1 resuelto=1");
    }
    {
        Linea l = invariante(Capa::PRESENTACION, "contador", "GetLastPresentCount fallo");
        l.par("hr", -2005270527LL).txtpar("modo", "runtime");
        chequear("negativos y texto", l.b,
                 "INVARIANTE capa3/presentacion contador: GetLastPresentCount fallo hr=-2005270527 modo=runtime");
    }
    {
        // Nunca desborda: 300 caracteres de detalle se cortan a 255.
        char largo[301]; memset(largo, 'x', 300); largo[300] = 0;
        Linea l = invariante(Capa::SESION, "x", largo);
        l.par("k", 1);
        chequear("largo acotado", l.n == 255 && strlen(l.b) == 255 ? "ok" : "mal", "ok");
    }
    printf("\n%s\n", fallos == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return fallos ? 1 : 0;
}
