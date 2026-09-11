// Pincha la politica que CORRE (src/politica.h) con casos sacados de los logs.
//
// Cada caso pone la entrada que un juego produjo de verdad y espera lo que
// force_into escribio en esa corrida. No es lo que deberia pasar: es lo que
// pasa, para que un refactor que lo cambie se vea aca y no en un juego.
//
//   g++ -std=c++20 -O2 -I src tools/test_politica.cpp -o /tmp/tp.exe && /tmp/tp.exe
#include <cstdio>
#include "policy.h"

using namespace policy;

static int failures = 0;

static void check(const char *label, long got, long expected) {
    const bool ok = got == expected;
    if (!ok) ++failures;
    printf("  %-62s %s (dio %ld, esperado %ld)\n", label, ok ? "ok  " : "FALLA", got, expected);
}

// Base: nuestro snippet cargado (multiplicador), parche puesto, tope 6.
static ForceInput base(void) {
    ForceInput e{};
    e.passive = false;
    e.game_asked_on = false;
    e.game_wants = -1;
    e.sel = 0;
    e.force_generated = 0;
    e.multiplier = true;
    e.cycle_ceiling = 0;
    e.six = true;
    e.ceilfirst = false;
    e.interp_on = false;
    e.wic_ok = true;
    e.last_seen_generated = 0;
    e.cap = 6;
    return e;
}

int main(void) {
    printf("politica que corre -- casos de los logs\n\n");

    printf("guardas que no escriben nada\n");
    {
        ForceInput e = base(); e.passive = true; e.sel = 4;
        ForceOutput s = decide_force(e);
        check("PASIVO no escribe modo", s.write_mode ? 1 : 0, 0);
        check("PASIVO no escribe cuenta", s.write_count ? 1 : 0, 0);
    }
    {
        // GTA V pausado: pidio eOn antes, ahora escribe eOff 170 veces/s.
        ForceInput e = base(); e.game_asked_on = true; e.game_wants = 0; e.sel = 6;
        ForceOutput s = decide_force(e);
        check("GTA V en pausa (eOn visto, ahora eOff): no se pelea", s.write_mode ? 1 : 0, 0);
        check("  razon JUEGO_APAGO", (long)s.reason, (long)Reason::GAME_TURNED_OFF);
    }
    {
        // Halo intento16: 235 ventanas con eOff y NUNCA eOn antes. Ahi se fuerza.
        ForceInput e = base(); e.game_asked_on = false; e.game_wants = 0; e.sel = 4;
        ForceOutput s = decide_force(e);
        check("Halo (eOff sin eOn previo), fijo 4: se escribe eOn", s.mode, kOn);
        check("  cuenta 4 (multiplicador)", s.count, 4);
    }
    {
        ForceInput e = base(); e.sel = 0;
        ForceOutput s = decide_force(e);
        check("sel 0: nada", (long)s.reason, (long)Reason::NONE);
    }

    printf("\nmodos fijos\n");
    {
        ForceInput e = base(); e.sel = 1;
        check("sel 1 -> eOff", decide_force(e).mode, kOff);
        e.sel = 2;
        check("2X con multiplicador -> cuenta 2", decide_force(e).count, 2);
        e.multiplier = false;
        check("2X con generados (snippet de julio) -> cuenta 1", decide_force(e).count, 1);
        e.multiplier = true; e.sel = 6; e.cap = 5;
        check("6X con tope 5 -> 5", decide_force(e).count, 5);
        e.cap = 6;
        check("6X con tope 6 -> 6 (Halo entrego 5.98)", decide_force(e).count, 6);
    }

    printf("\nDYNAMIC\n");
    {
        ForceInput e = base(); e.sel = kSelDynFut; e.force_generated = 0;
        ForceOutput s = decide_force(e);
        check("cuenta 0 -> eOff, no eOn con cero", s.mode, kOff);
        check("  y no se escribe cuenta", s.write_count ? 1 : 0, 0);
    }
    {
        // Fallo 4: la semilla 1 con multiplicador es 1X. Con parche se corrige a 2.
        ForceInput e = base(); e.sel = kSelDynFut; e.force_generated = 1;
        ForceOutput s = decide_force(e);
        check("semilla 1 con parche -> invariante roto", s.invariant_broken ? 1 : 0, 1);
        check("  corregido a 2", s.count, 2);
    }
    {
        ForceInput e = base(); e.sel = kSelDynFut; e.force_generated = 3;
        check("cuenta 3 pasa tal cual (no se sube al techo del ciclo)",
                 decide_force(e).count, 3);
        e.cycle_ceiling = 4;
        check("  ni con ciclo_techo 4", decide_force(e).count, 3);
    }
    {
        // Snippet de julio (generados): la cuenta es la reserva, se declara el techo.
        ForceInput e = base(); e.sel = kSelDynFut; e.multiplier = false;
        e.force_generated = 2; e.cycle_ceiling = 3;
        check("generados: cuenta 2 con ciclo_techo 3 -> 3", decide_force(e).count, 3);
        e.cycle_ceiling = 6; e.six = false;
        check("  techo 6 sin mfg-seis: no se sube (quedaria 2)", decide_force(e).count, 2);
    }
    {
        ForceInput e = base(); e.sel = kSelDynFut; e.force_generated = 2;
        e.ceilfirst = true; e.cycle_ceiling = 4; e.interp_on = false;
        ForceOutput s = decide_force(e);
        check("A1 (ceilfirst) apagado: declara el techo 4", s.count, 4);
        check("  y lo dice", s.a1_declared ? 1 : 0, 1);
        e.interp_on = true;
        check("A1 con interpolacion andando: no toca", decide_force(e).count, 2);
    }

    printf("\nfreno (sin parche de la cuenta)\n");
    {
        // Fallo 2: Halo pide count 1 (generados = 2X). Nuestro snippet habla
        // multiplicador: se espeja 2, no 1.
        ForceInput e = base(); e.sel = kSelDynFut; e.wic_ok = false;
        e.force_generated = 4; e.last_seen_generated = 1;
        ForceOutput s = decide_force(e);
        check("Halo pide 1 (=2X): la cuenta se limita a 2", s.count, 2);
        check("  freno_limito con tope 2", s.brake_limited ? s.brake_cap : -1, 2);
    }
    {
        // "Nunca vimos que cuenta pide el juego" solo se detecta con el snippet
        // de julio: con multiplicador un 0 del juego se traduce a 1 y pasa la
        // guarda como si fuera dato. Lo que corre hoy escribe 1 (=1X) y deja
        // constancia del invariante. Se pincha tal cual; arreglarlo es otra fase.
        ForceInput e = base(); e.sel = kSelDynFut; e.wic_ok = false;
        e.force_generated = 4; e.last_seen_generated = 0;
        ForceOutput s = decide_force(e);
        check("multiplicador y juego con 0: la guarda no salta (1X)", s.count, 1);
        check("  y el invariante queda roto sin corregir", s.invariant_broken && !s.invariant_fixed ? 1 : 0, 1);
        e.multiplier = false;
        s = decide_force(e);
        check("generados y juego con 0: ahi si, no se toca nada", s.write_mode ? 1 : 0, 0);
        check("  razon FRENO_SIN_DATO", (long)s.reason, (long)Reason::BRAKE_NO_DATA);
    }
    {
        // El invariante NO se corrige con el freno: corregirlo congelo Halo 24 veces.
        ForceInput e = base(); e.sel = kSelDynFut; e.wic_ok = false;
        e.force_generated = 1; e.last_seen_generated = 1;
        ForceOutput s = decide_force(e);
        check("freno y cuenta 1: roto pero NO corregido", s.invariant_broken && !s.invariant_fixed ? 1 : 0, 1);
        check("  se escribe 1 tal cual", s.count, 1);
    }
    {
        ForceInput e = base(); e.sel = kSelDynFut; e.wic_ok = false;
        e.multiplier = false; e.force_generated = 3; e.last_seen_generated = 3;
        check("generados a generados: se espeja 3 sin traducir", decide_force(e).count, 3);
    }

    // ---- F6: resolve() (la politica que se quiere) contra decidir_force()
    // (la que corre), sobre la misma entrada. Donde coinciden, sobra una; donde
    // no, la diferencia queda escrita y pinchada: unificarlas es un cambio de
    // comportamiento que necesita el juego que lo distingue.
    printf("\nF6 -- resolve() contra decidir_force()\n");
    {
        // Modo fijo 4 con el juego pidiendo eOn y count 1 (generados): iguales.
        ForceInput e = base(); e.sel = 4; e.game_asked_on = true; e.game_wants = 1;
        e.last_seen_generated = 1;
        ForceOutput d = decide_force(e);
        PluginRequest r = resolve(GameRequest{ kOn, 1 }, Dialect::GENERATED,
                                 Dialect::MULTIPLIER, Decision{ 400, 6 });
        check("fijo 4X: las dos escriben eOn", d.mode == r.mode ? 1 : 0, 1);
        check("  y cuenta 4", d.count == r.count ? d.count : -1, 4);
        // GTA V en pausa: el juego pidio eOn antes y ahora eOff. Iguales en
        // efecto: ninguna enciende (decidir no escribe, resolve pasa eOff).
        e.game_wants = 0;
        d = decide_force(e);
        r = resolve(GameRequest{ kOff, 3 }, Dialect::GENERATED, Dialect::MULTIPLIER, Decision{ 400, 6 });
        check("pausa de GTA V: decidir no escribe, resolve pasa eOff", (!d.write_mode && r.mode == kOff) ? 1 : 0, 1);
        // DIVERGENCIA 1: Halo escribe eOff sin haber pedido eOn nunca.
        // decidir fuerza eOn (asi entrego 4.02); resolve pasaria eOff y Halo
        // no generaria jamas. resolve() esta mal aca: le falta la entrada
        // "alguna vez pidio eOn".
        e.game_asked_on = false;
        d = decide_force(e);
        check("Halo (eOff sin eOn previo): decidir fuerza eOn", d.mode, kOn);
        check("  DIVERGENCIA: resolve pasaria eOff (medido: Halo 4.02 con eOn forzado)", r.mode, kOff);
        // DIVERGENCIA 2: sin objetivo, resolve respeta la cuenta del juego
        // traducida (1 -> 2); decidir con sel 0 no escribe nada. Sin snippet
        // sustituido da lo mismo; con snippet sustituido resolve tiene razon
        // y decidir deja 1 (= 1X). Sin medir en un juego.
        e.sel = 0; e.game_asked_on = true; e.game_wants = 1;
        d = decide_force(e);
        r = resolve(GameRequest{ kOn, 1 }, Dialect::GENERATED, Dialect::MULTIPLIER, Decision{ 0, 6 });
        check("sin seleccion: decidir no escribe", d.write_count ? 1 : 0, 0);
        check("  DIVERGENCIA: resolve traduciria 1 -> 2", r.count, 2);
    }

    printf("\n%s\n", failures == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return failures ? 1 : 0;
}
