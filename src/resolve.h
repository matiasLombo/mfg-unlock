// Capa 2: que se le pide al plugin. Una funcion pura, sin globales.
//
// Existe porque la politica vivia en `force_into` como un arbol de cinco ramas
// sobre estado mutable, con varios escritores, y cada juego nuevo le agregaba
// una rama. Tres de los seis fallos del 2026-09-10 son de esta capa y ninguno
// necesitaba una GPU para reproducirse:
//
//   fallo 2  Halo pide `count 1`. En SU dialecto eso es 2X; con nuestro snippet
//            la cuenta es el multiplicador, asi que se leia como 1X.
//   fallo 3  GTA V escribe eOff al pausar, 170 veces por segundo, y le
//            forzabamos eOn encima. El bucle alternaba entre el multiplicador
//            limpio y rafagas de 1000-2000 fps que no presentan: imagen
//            congelada.
//   fallo 4  La semilla de DYNAMIC era el literal 1, que con la semantica nueva
//            es 1X. Sin generacion no hay base que medir, el controlador nunca
//            decide, y se espera a si mismo.
//
// Sin estado y sin dependencias de Windows a proposito: se compila igual dentro
// del dll y dentro de tools/test_resolve.cpp, asi que los tres se reproducen en
// milisegundos. Ver docs/arquitectura-por-capas.md, capa 2.
//
// ESTADO (2026-09-11, F6): resolve() NO es lo que corre. Lo que corre es
// decidir_force() en politica.h, que usa los helpers de dialecto de aca.
// tools/test_politica.cpp los compara caso por caso: coinciden en los modos
// fijos y en la pausa de GTA V, y divergen en dos cosas medibles --
//   1. un juego que escribe eOff sin haber pedido eOn nunca (Halo): resolve
//      pasaria eOff y no generaria; decidir fuerza eOn y Halo entrego 4.02.
//      A resolve le falta esa entrada.
//   2. sin seleccion, resolve traduce la cuenta del juego a nuestro dialecto;
//      decidir no escribe nada. Con snippet sustituido resolve tiene razon.
// Unificarlas es un cambio de comportamiento y necesita el juego que lo
// distingue (Halo para 1, cualquiera con la base sustituida y sel 0 para 2).
#pragma once

namespace policy {

// En que unidades habla cada lado. No es lo mismo el dialecto del JUEGO --
// fijado por el snippet contra el que se compilo -- que el nuestro, que lo fija
// el snippet que cargamos. Confundirlos es el fallo 2.
enum class Dialect { GENERATED, MULTIPLIER };

// Los modos de DLSSGMode que nos importan. eAuto lo decide el juego.
enum Mode { kOff = 0, kOn = 1, kAuto = 2 };

struct GameRequest {
    long mode;      // lo que el juego escribio en la struct
    long count;    // en SU dialecto
};

struct Decision {
    long target_x100;   // 0 = no hay modo fijo ni controlador: se respeta al juego
    long cap;            // el maximo que el binario vivo soporta
};

struct PluginRequest {
    long mode;
    long count;
};

inline long to_multiplier(long count, Dialect d) {
    if (d == Dialect::MULTIPLIER) return count;
    return count + 1;              // generados -> multiplicador
}

inline long from_multiplier(long mult, Dialect d) {
    if (d == Dialect::MULTIPLIER) return mult;
    return mult - 1;
}

inline long clamp(long v, long lo, long hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline PluginRequest resolve(GameRequest g, Dialect game_dialect,
                            Dialect our_dialect, Decision dec) {
    PluginRequest out;
    // EL MODO NO SE TOCA.
    //
    // Es passthrough por regla, no por rama. Si el juego apago la generacion,
    // apagada queda: pelearle el eOff es el fallo 3, y no hay objetivo que
    // justifique encender lo que el juego decidio apagar.
    out.mode = g.mode;
    if (g.mode == kOff) {
        out.count = 0;
        return out;
    }

    // LA CUENTA SE TRADUCE, NO SE REEMPLAZA.
    const long requested_mult = to_multiplier(g.count, game_dialect);
    // Sin objetivo se respeta lo que pidio el juego, traducido. Esa es tambien
    // la semilla de DYNAMIC: nunca un literal (fallo 4).
    long target_mult = dec.target_x100 > 0
                             ? (dec.target_x100 + 50) / 100
                             : requested_mult;
    // El piso es 2: con la semantica de multiplicador, 1 es "generacion
    // encendida produciendo nada", que no es lo que pidio ningun modo.
    const long cap = dec.cap > 0 ? dec.cap : 6;
    target_mult = clamp(target_mult, 2, cap);
    out.count = from_multiplier(target_mult, our_dialect);
    return out;
}

}  // namespace pol
