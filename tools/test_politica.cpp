// Pincha la politica que CORRE (src/politica.h) con casos sacados de los logs.
//
// Cada caso pone la entrada que un juego produjo de verdad y espera lo que
// force_into escribio en esa corrida. No es lo que deberia pasar: es lo que
// pasa, para que un refactor que lo cambie se vea aca y no en un juego.
//
//   g++ -std=c++20 -O2 -I src tools/test_politica.cpp -o /tmp/tp.exe && /tmp/tp.exe
#include <cstdio>
#include "politica.h"

using namespace pol;

static int fallos = 0;

static void chequear(const char *caso, long got, long esperado) {
    const bool ok = got == esperado;
    if (!ok) ++fallos;
    printf("  %-62s %s (dio %ld, esperado %ld)\n", caso, ok ? "ok  " : "FALLA", got, esperado);
}

// Base: nuestro snippet cargado (multiplicador), parche puesto, tope 6.
static EntradaForce base(void) {
    EntradaForce e{};
    e.pasivo = false;
    e.juego_pidio_on = false;
    e.juego_quiere = -1;
    e.sel = 0;
    e.force_generated = 0;
    e.multiplicador = true;
    e.ciclo_techo = 0;
    e.seis = true;
    e.ceilfirst = false;
    e.interp_on = false;
    e.wic_ok = true;
    e.last_seen_generated = 0;
    e.tope = 6;
    return e;
}

int main(void) {
    printf("politica que corre -- casos de los logs\n\n");

    printf("guardas que no escriben nada\n");
    {
        EntradaForce e = base(); e.pasivo = true; e.sel = 4;
        SalidaForce s = decidir_force(e);
        chequear("PASIVO no escribe modo", s.escribir_modo ? 1 : 0, 0);
        chequear("PASIVO no escribe cuenta", s.escribir_cuenta ? 1 : 0, 0);
    }
    {
        // GTA V pausado: pidio eOn antes, ahora escribe eOff 170 veces/s.
        EntradaForce e = base(); e.juego_pidio_on = true; e.juego_quiere = 0; e.sel = 6;
        SalidaForce s = decidir_force(e);
        chequear("GTA V en pausa (eOn visto, ahora eOff): no se pelea", s.escribir_modo ? 1 : 0, 0);
        chequear("  razon JUEGO_APAGO", (long)s.razon, (long)Razon::JUEGO_APAGO);
    }
    {
        // Halo intento16: 235 ventanas con eOff y NUNCA eOn antes. Ahi se fuerza.
        EntradaForce e = base(); e.juego_pidio_on = false; e.juego_quiere = 0; e.sel = 4;
        SalidaForce s = decidir_force(e);
        chequear("Halo (eOff sin eOn previo), fijo 4: se escribe eOn", s.modo, kOn);
        chequear("  cuenta 4 (multiplicador)", s.cuenta, 4);
    }
    {
        EntradaForce e = base(); e.sel = 0;
        SalidaForce s = decidir_force(e);
        chequear("sel 0: nada", (long)s.razon, (long)Razon::NADA);
    }

    printf("\nmodos fijos\n");
    {
        EntradaForce e = base(); e.sel = 1;
        chequear("sel 1 -> eOff", decidir_force(e).modo, kOff);
        e.sel = 2;
        chequear("2X con multiplicador -> cuenta 2", decidir_force(e).cuenta, 2);
        e.multiplicador = false;
        chequear("2X con generados (snippet de julio) -> cuenta 1", decidir_force(e).cuenta, 1);
        e.multiplicador = true; e.sel = 6; e.tope = 5;
        chequear("6X con tope 5 -> 5", decidir_force(e).cuenta, 5);
        e.tope = 6;
        chequear("6X con tope 6 -> 6 (Halo entrego 5.98)", decidir_force(e).cuenta, 6);
    }

    printf("\nDYNAMIC\n");
    {
        EntradaForce e = base(); e.sel = kSelDynFut; e.force_generated = 0;
        SalidaForce s = decidir_force(e);
        chequear("cuenta 0 -> eOff, no eOn con cero", s.modo, kOff);
        chequear("  y no se escribe cuenta", s.escribir_cuenta ? 1 : 0, 0);
    }
    {
        // Fallo 4: la semilla 1 con multiplicador es 1X. Con parche se corrige a 2.
        EntradaForce e = base(); e.sel = kSelDynFut; e.force_generated = 1;
        SalidaForce s = decidir_force(e);
        chequear("semilla 1 con parche -> invariante roto", s.invariante_roto ? 1 : 0, 1);
        chequear("  corregido a 2", s.cuenta, 2);
    }
    {
        EntradaForce e = base(); e.sel = kSelDynFut; e.force_generated = 3;
        chequear("cuenta 3 pasa tal cual (no se sube al techo del ciclo)",
                 decidir_force(e).cuenta, 3);
        e.ciclo_techo = 4;
        chequear("  ni con ciclo_techo 4", decidir_force(e).cuenta, 3);
    }
    {
        // Snippet de julio (generados): la cuenta es la reserva, se declara el techo.
        EntradaForce e = base(); e.sel = kSelDynFut; e.multiplicador = false;
        e.force_generated = 2; e.ciclo_techo = 3;
        chequear("generados: cuenta 2 con ciclo_techo 3 -> 3", decidir_force(e).cuenta, 3);
        e.ciclo_techo = 6; e.seis = false;
        chequear("  techo 6 sin mfg-seis: no se sube (quedaria 2)", decidir_force(e).cuenta, 2);
    }
    {
        EntradaForce e = base(); e.sel = kSelDynFut; e.force_generated = 2;
        e.ceilfirst = true; e.ciclo_techo = 4; e.interp_on = false;
        SalidaForce s = decidir_force(e);
        chequear("A1 (ceilfirst) apagado: declara el techo 4", s.cuenta, 4);
        chequear("  y lo dice", s.a1_declaro ? 1 : 0, 1);
        e.interp_on = true;
        chequear("A1 con interpolacion andando: no toca", decidir_force(e).cuenta, 2);
    }

    printf("\nfreno (sin parche de la cuenta)\n");
    {
        // Fallo 2: Halo pide count 1 (generados = 2X). Nuestro snippet habla
        // multiplicador: se espeja 2, no 1.
        EntradaForce e = base(); e.sel = kSelDynFut; e.wic_ok = false;
        e.force_generated = 4; e.last_seen_generated = 1;
        SalidaForce s = decidir_force(e);
        chequear("Halo pide 1 (=2X): la cuenta se limita a 2", s.cuenta, 2);
        chequear("  freno_limito con tope 2", s.freno_limito ? s.freno_tope : -1, 2);
    }
    {
        // "Nunca vimos que cuenta pide el juego" solo se detecta con el snippet
        // de julio: con multiplicador un 0 del juego se traduce a 1 y pasa la
        // guarda como si fuera dato. Lo que corre hoy escribe 1 (=1X) y deja
        // constancia del invariante. Se pincha tal cual; arreglarlo es otra fase.
        EntradaForce e = base(); e.sel = kSelDynFut; e.wic_ok = false;
        e.force_generated = 4; e.last_seen_generated = 0;
        SalidaForce s = decidir_force(e);
        chequear("multiplicador y juego con 0: la guarda no salta (1X)", s.cuenta, 1);
        chequear("  y el invariante queda roto sin corregir", s.invariante_roto && !s.invariante_corregido ? 1 : 0, 1);
        e.multiplicador = false;
        s = decidir_force(e);
        chequear("generados y juego con 0: ahi si, no se toca nada", s.escribir_modo ? 1 : 0, 0);
        chequear("  razon FRENO_SIN_DATO", (long)s.razon, (long)Razon::FRENO_SIN_DATO);
    }
    {
        // El invariante NO se corrige con el freno: corregirlo congelo Halo 24 veces.
        EntradaForce e = base(); e.sel = kSelDynFut; e.wic_ok = false;
        e.force_generated = 1; e.last_seen_generated = 1;
        SalidaForce s = decidir_force(e);
        chequear("freno y cuenta 1: roto pero NO corregido", s.invariante_roto && !s.invariante_corregido ? 1 : 0, 1);
        chequear("  se escribe 1 tal cual", s.cuenta, 1);
    }
    {
        EntradaForce e = base(); e.sel = kSelDynFut; e.wic_ok = false;
        e.multiplicador = false; e.force_generated = 3; e.last_seen_generated = 3;
        chequear("generados a generados: se espeja 3 sin traducir", decidir_force(e).cuenta, 3);
    }

    printf("\n%s\n", fallos == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return fallos ? 1 : 0;
}
