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

enum class Capa {
    SESION,         // las fases: ARMADO -> VERIFICADO|PASIVO -> ACTIVO
    IDENTIDAD,      // capa 0: que copia del plugin ejecuta
    SUMINISTRO,     // capa 1: que set/snippet se carga y con que parches
    POLITICA,       // capa 2: que se le pide al plugin (resolve/decidir_force)
    PRESENTACION,   // capa 3: swapchain, Present, el contador
};

inline const char *nombre_capa(Capa c) {
    switch (c) {
    case Capa::SESION:       return "sesion";
    case Capa::IDENTIDAD:    return "capa0/identidad";
    case Capa::SUMINISTRO:   return "capa1/suministro";
    case Capa::POLITICA:     return "capa2/politica";
    case Capa::PRESENTACION: return "capa3/presentacion";
    }
    return "?";
}

// Un buffer de linea. Nunca desborda: lo que no entra se corta.
struct Linea {
    char b[256];
    int n = 0;
    Linea() { b[0] = 0; }
    void txt(const char *s) {
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
        if (v < 0) { txt("-"); num((unsigned long long)(-v)); } else num((unsigned long long)v);
    }
    // ` clave=valor`; txtpar para un valor de texto (un 0 literal seria ambiguo)
    Linea &par(const char *clave, long long v) { txt(" "); txt(clave); txt("="); snum(v); return *this; }
    Linea &txtpar(const char *clave, const char *v) { txt(" "); txt(clave); txt("="); txt(v); return *this; }
};

// `INVARIANTE <capa> <nombre>: <detalle>` -- despues se le agregan pares.
inline Linea invariante(Capa c, const char *nombre, const char *detalle) {
    Linea l;
    l.txt("INVARIANTE ");
    l.txt(nombre_capa(c));
    l.txt(" ");
    l.txt(nombre);
    l.txt(": ");
    l.txt(detalle);
    return l;
}

// `VEREDICTO <fase>: <detalle>` -- la decision de la sesion, mismo formato.
inline Linea veredicto(const char *fase, const char *detalle) {
    Linea l;
    l.txt("VEREDICTO ");
    l.txt(fase);
    l.txt(": ");
    l.txt(detalle);
    return l;
}

}  // namespace diag
