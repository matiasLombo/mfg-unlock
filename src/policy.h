// Capa 2, tal como esta hoy: la decision de force_into como funcion pura.
//
// Esto es una EXTRACCION, no un diseno. El arbol de abajo es el de force_into
// (proxy.cpp) movido a un lugar sin globales ni Windows, rama por rama y con
// los mismos numeros, para que se pueda compilar en el host y pinchar con
// tools/test_politica.cpp. Lo que force_into hacia bien lo sigue haciendo
// bien; lo que hacia mal, tambien -- y ahora se puede ver en un test antes de
// abrir un juego. Mejorarlo es otra fase, y se hace contra estos casos.
//
// Se distingue de resolve.h: aquello es la politica que se QUIERE, escrita
// limpia y nunca cableada; esto es la que CORRE. Cuando las dos coincidan
// caso por caso en el test, sobra una.
//
// Lo que force_into hace ademas de decidir -- escribir la struct, loguear con
// dedupe, g_target_written -- se queda alla. Aca solo entra lo que lee y sale
// lo que decide, mas los numeros que sus lineas de log necesitan.
#pragma once
#include "resolve.h"

namespace policy {

// Todo lo que force_into lee. Un campo por global, con el mismo significado.
struct ForceInput {
    bool passive;                 // fase_pasiva()
    bool game_asked_on;         // g_juego_pidio_on != 0
    long game_wants;           // g_juego_quiere: -1 nunca dijo, 0 eOff, 1 eOn
    long sel;                    // g_force_sel: 0 nada, 1 OFF, 2..6 fijo, 7/8 DYNAMIC
    long force_generated;        // g_force_generated, ya en NUESTRO dialecto
    bool multiplier;          // cuenta_es_multiplicador()
    long cycle_ceiling;            // g_ciclo_techo
    bool six;                   // g_seis
    bool ceilfirst;              // g_ceilfirst (mfg-ceilfirst.txt)
    bool interp_on;              // g_interp_on != 0
    bool wic_ok;                 // g_wic_ok: el parche de la cuenta esta puesto
    long last_seen_generated;    // g_last_seen_generated: lo que el juego escribio
    long cap;                   // tope_cuenta()
};

// Las mismas constantes de overlay.h, sin arrastrar ese header al host.
enum { kSelOff = 1, kSelDyn = 7, kSelDynFut = 8 };

enum class Reason {
    PASSIVE,          // no se reescribe nada: topologia rota
    GAME_TURNED_OFF,     // el juego pidio eOff despues de haber pedido eOn
    NONE,            // sel 0: no hay seleccion
    OFF,             // sel 1
    DYN_OFF,         // DYNAMIC con cuenta 0: eOff, no eOn con cero
    DYN_ON,          // DYNAMIC: eOn con la cuenta calculada
    BRAKE_NO_DATA,  // sin parche y sin saber que pide el juego: no se toca
    FIXED,            // sel 2..6
};

struct ForceOutput {
    Reason reason;
    bool write_mode;   long mode;
    bool write_count; long count;
    // Para las lineas de log de force_into, con sus mismas claves de dedupe.
    bool a1_declared;      long a1_ceiling;
    bool brake_limited;    long brake_cap;
    long to_write;        // lo que se iba a escribir antes del recorte a tope
    long remains;           // min(escribir, tope) antes de la correccion del invariante
    bool invariant_broken;
    bool invariant_fixed;
};

inline ForceOutput decide_force(const ForceInput &e) {
    ForceOutput s{};
    s.reason = Reason::NONE;
    if (e.passive) { s.reason = Reason::PASSIVE; return s; }
    // juego_apago_la_generacion(): la guarda de haberlo visto pedir eOn importa,
    // hay juegos que no llaman nunca con eOn y ahi respetar el eOff seria no
    // generar jamas.
    if (e.game_asked_on && e.game_wants == 0) { s.reason = Reason::GAME_TURNED_OFF; return s; }

    if (e.sel == kSelOff) {
        s.reason = Reason::OFF;
        s.write_mode = true; s.mode = kOff;
        return s;
    }
    if (e.sel == kSelDyn || e.sel == kSelDynFut) {
        if (e.force_generated <= 0) {
            s.reason = Reason::DYN_OFF;
            s.write_mode = true; s.mode = kOff;
            return s;
        }
        s.reason = Reason::DYN_ON;
        s.write_mode = true; s.mode = kOn;
        long to_write = e.force_generated;
        // Con la semantica de generados la cuenta de la API es la RESERVA y se
        // declara el techo del ciclo; con la de multiplicador la cuenta ES lo
        // que se entrega y no se sube.
        if (!e.multiplier) {
            const long techo = e.cycle_ceiling;
            if (techo > to_write && techo <= (e.six ? 6 : 5)) to_write = techo;
        }
        if (e.ceilfirst) {
            const long techo = e.cycle_ceiling;
            if (!e.interp_on && techo > to_write &&
                techo <= ((e.six || e.multiplier) ? 6 : 5)) {
                to_write = techo;
                s.a1_declared = true; s.a1_ceiling = techo;
            }
        }
        // Freno: sin el parche de la cuenta se espeja lo que pide el juego,
        // traducido de SU dialecto (el del snippet contra el que se compilo,
        // que force_into asume generados) al nuestro.
        if (!e.wic_ok) {
            const long theirs = from_multiplier(
                to_multiplier(e.last_seen_generated, Dialect::GENERATED),
                e.multiplier ? Dialect::MULTIPLIER : Dialect::GENERATED);
            if (theirs < 1 || theirs > 6) {
                // El modo vuelve como estaba: no se enciende la generacion con
                // una cuenta que no elegimos.
                s.reason = Reason::BRAKE_NO_DATA;
                s.write_mode = false;
                return s;
            }
            if (to_write > theirs) {
                s.brake_limited = true; s.brake_cap = theirs;
                to_write = theirs;
            }
        }
        s.to_write = to_write;
        s.remains = to_write > e.cap ? e.cap : to_write;
        // El invariante: con multiplicador, una cuenta menor a 2 es 1X. Se
        // corrige solo cuando manda el parche; con el freno solo se deja
        // constancia (corregirlo ahi fue lo que congelo Halo 24 veces).
        if (e.multiplier && s.remains < 2 && e.sel >= 2) {
            s.invariant_broken = true;
            if (e.wic_ok) { s.invariant_fixed = true; to_write = 2; }
        }
        s.write_count = true;
        s.count = to_write > e.cap ? e.cap : to_write;
        return s;
    }
    if (e.sel >= 2) {
        s.reason = Reason::FIXED;
        s.write_mode = true; s.mode = kOn;
        const long c = e.multiplier ? e.sel : e.sel - 1;
        s.write_count = true;
        s.count = c > e.cap ? e.cap : c;
        return s;
    }
    return s;
}

}  // namespace pol
