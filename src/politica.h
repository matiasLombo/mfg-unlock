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

namespace pol {

// Todo lo que force_into lee. Un campo por global, con el mismo significado.
struct EntradaForce {
    bool pasivo;                 // fase_pasiva()
    bool juego_pidio_on;         // g_juego_pidio_on != 0
    long juego_quiere;           // g_juego_quiere: -1 nunca dijo, 0 eOff, 1 eOn
    long sel;                    // g_force_sel: 0 nada, 1 OFF, 2..6 fijo, 7/8 DYNAMIC
    long force_generated;        // g_force_generated, ya en NUESTRO dialecto
    bool multiplicador;          // cuenta_es_multiplicador()
    long ciclo_techo;            // g_ciclo_techo
    bool seis;                   // g_seis
    bool ceilfirst;              // g_ceilfirst (mfg-ceilfirst.txt)
    bool interp_on;              // g_interp_on != 0
    bool wic_ok;                 // g_wic_ok: el parche de la cuenta esta puesto
    long last_seen_generated;    // g_last_seen_generated: lo que el juego escribio
    long tope;                   // tope_cuenta()
};

// Las mismas constantes de overlay.h, sin arrastrar ese header al host.
enum { kSelOff = 1, kSelDyn = 7, kSelDynFut = 8 };

enum class Razon {
    PASIVO,          // no se reescribe nada: topologia rota
    JUEGO_APAGO,     // el juego pidio eOff despues de haber pedido eOn
    NADA,            // sel 0: no hay seleccion
    OFF,             // sel 1
    DYN_OFF,         // DYNAMIC con cuenta 0: eOff, no eOn con cero
    DYN_ON,          // DYNAMIC: eOn con la cuenta calculada
    FRENO_SIN_DATO,  // sin parche y sin saber que pide el juego: no se toca
    FIJO,            // sel 2..6
};

struct SalidaForce {
    Razon razon;
    bool escribir_modo;   long modo;
    bool escribir_cuenta; long cuenta;
    // Para las lineas de log de force_into, con sus mismas claves de dedupe.
    bool a1_declaro;      long a1_techo;
    bool freno_limito;    long freno_tope;
    long escribir;        // lo que se iba a escribir antes del recorte a tope
    long queda;           // min(escribir, tope) antes de la correccion del invariante
    bool invariante_roto;
    bool invariante_corregido;
};

inline SalidaForce decidir_force(const EntradaForce &e) {
    SalidaForce s{};
    s.razon = Razon::NADA;
    if (e.pasivo) { s.razon = Razon::PASIVO; return s; }
    // juego_apago_la_generacion(): la guarda de haberlo visto pedir eOn importa,
    // hay juegos que no llaman nunca con eOn y ahi respetar el eOff seria no
    // generar jamas.
    if (e.juego_pidio_on && e.juego_quiere == 0) { s.razon = Razon::JUEGO_APAGO; return s; }

    if (e.sel == kSelOff) {
        s.razon = Razon::OFF;
        s.escribir_modo = true; s.modo = kOff;
        return s;
    }
    if (e.sel == kSelDyn || e.sel == kSelDynFut) {
        if (e.force_generated <= 0) {
            s.razon = Razon::DYN_OFF;
            s.escribir_modo = true; s.modo = kOff;
            return s;
        }
        s.razon = Razon::DYN_ON;
        s.escribir_modo = true; s.modo = kOn;
        long escribir = e.force_generated;
        // Con la semantica de generados la cuenta de la API es la RESERVA y se
        // declara el techo del ciclo; con la de multiplicador la cuenta ES lo
        // que se entrega y no se sube.
        if (!e.multiplicador) {
            const long techo = e.ciclo_techo;
            if (techo > escribir && techo <= (e.seis ? 6 : 5)) escribir = techo;
        }
        if (e.ceilfirst) {
            const long techo = e.ciclo_techo;
            if (!e.interp_on && techo > escribir &&
                techo <= ((e.seis || e.multiplicador) ? 6 : 5)) {
                escribir = techo;
                s.a1_declaro = true; s.a1_techo = techo;
            }
        }
        // Freno: sin el parche de la cuenta se espeja lo que pide el juego,
        // traducido de SU dialecto (el del snippet contra el que se compilo,
        // que force_into asume generados) al nuestro.
        if (!e.wic_ok) {
            const long suyo = desde_multiplicador(
                a_multiplicador(e.last_seen_generated, Dialecto::GENERADOS),
                e.multiplicador ? Dialecto::MULTIPLICADOR : Dialecto::GENERADOS);
            if (suyo < 1 || suyo > 6) {
                // El modo vuelve como estaba: no se enciende la generacion con
                // una cuenta que no elegimos.
                s.razon = Razon::FRENO_SIN_DATO;
                s.escribir_modo = false;
                return s;
            }
            if (escribir > suyo) {
                s.freno_limito = true; s.freno_tope = suyo;
                escribir = suyo;
            }
        }
        s.escribir = escribir;
        s.queda = escribir > e.tope ? e.tope : escribir;
        // El invariante: con multiplicador, una cuenta menor a 2 es 1X. Se
        // corrige solo cuando manda el parche; con el freno solo se deja
        // constancia (corregirlo ahi fue lo que congelo Halo 24 veces).
        if (e.multiplicador && s.queda < 2 && e.sel >= 2) {
            s.invariante_roto = true;
            if (e.wic_ok) { s.invariante_corregido = true; escribir = 2; }
        }
        s.escribir_cuenta = true;
        s.cuenta = escribir > e.tope ? e.tope : escribir;
        return s;
    }
    if (e.sel >= 2) {
        s.razon = Razon::FIJO;
        s.escribir_modo = true; s.modo = kOn;
        const long c = e.multiplicador ? e.sel : e.sel - 1;
        s.escribir_cuenta = true;
        s.cuenta = c > e.tope ? e.tope : c;
        return s;
    }
    return s;
}

}  // namespace pol
