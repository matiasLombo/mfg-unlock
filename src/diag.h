// Diagnostico: UNA linea por invariante roto, que dice la capa y el nombre.
//
// Lo que habia: "INVARIANTE ROTO: cuenta 1" seguido de seis lineas de
// contexto, "! LA COPIA QUE EJECUTA NO TIENE TODOS LOS PARCHES" en otro
// formato, "VEREDICTO: PASIVO" en otro, y un dump de treinta lineas por
// ventana donde hay que saber cual mirar. Para saber que se rompio habia que
// leer el log entero y conocer el codigo.
//
// Lo que hay: toda linea de invariante empieza con `INVARIANTE`, sigue con la
// capa (docs/arquitectura-por-capas.md), el nombre del invariante, un detalle
// en palabras y los numeros que hacen falta como `clave=valor`. Una sola
// linea, greppeable, con todo lo que un analisis necesita:
//
//   INVARIANTE capa2/politica cuenta>=2: con multiplicador es 1X, no genera
//       sel=8 gen=1 objetivo=255 tope=6 correccion=1
//
// Sin CRT ni Windows: se arma en un buffer con un constructor minimo, para que
// tools/test_diag.cpp lo pinche y para que sirva igual dentro del dll, que no
// usa printf.
#pragma once

namespace diag {

enum class Layer {
    SESSION,         // las fases: ARMADO -> VERIFICADO|PASIVO -> ACTIVO
    IDENTITY,      // capa 0: que copia del plugin ejecuta
    SUPPLY,     // capa 1: que set/snippet se carga y con que parches
    POLICY,       // capa 2: que se le pide al plugin (resolve/decidir_force)
    PRESENTATION,   // capa 3: swapchain, Present, el contador
};

inline const char *layer_name(Layer c) {
    switch (c) {
    case Layer::SESSION:       return "sesion";
    case Layer::IDENTITY:    return "capa0/identidad";
    case Layer::SUPPLY:   return "capa1/suministro";
    case Layer::POLICY:     return "capa2/politica";
    case Layer::PRESENTATION: return "capa3/presentacion";
    }
    return "?";
}

// Un buffer de linea. Nunca desborda: lo que no entra se corta.
struct Line {
    char b[256];
    int n = 0;
    Line() { b[0] = 0; }
    void text(const char *s) {
        while (*s != 0 && n < (int)sizeof(b) - 1) b[n++] = *s++;
        b[n] = 0;
    }
    void num(unsigned long long v) {
        char d[24]; int k = 0;
        if (v == 0) d[k++] = '0';
        while (v > 0 && k < 24) { d[k++] = (char)('0' + v % 10); v /= 10; }
        while (k > 0 && n < (int)sizeof(b) - 1) b[n++] = d[--k];
        b[n] = 0;
    }
    void snum(long long v) {
        if (v < 0) { text("-"); num((unsigned long long)(-v)); } else num((unsigned long long)v);
    }
    // ` clave=valor`; txtpar para un valor de texto (un 0 literal seria ambiguo)
    Line &pair(const char *key, long long v) { text(" "); text(key); text("="); snum(v); return *this; }
    Line &text_pair(const char *key, const char *v) { text(" "); text(key); text("="); text(v); return *this; }
};

// `INVARIANTE <capa> <nombre>: <detalle>` -- despues se le agregan pares.
inline Line invariant(Layer c, const char *name, const char *detail) {
    Line l;
    l.text("INVARIANTE ");
    l.text(layer_name(c));
    l.text(" ");
    l.text(name);
    l.text(": ");
    l.text(detail);
    return l;
}

// `VEREDICTO <fase>: <detalle>` -- la decision de la sesion, mismo formato.
inline Line verdict(const char *phase, const char *detail) {
    Line l;
    l.text("VEREDICTO ");
    l.text(phase);
    l.text(": ");
    l.text(detail);
    return l;
}

}  // namespace diag
