// Reproduce sin GPU y sin juego los fallos de politica del 2026-09-10.
//
// Cada caso sale de una medicion, no de un supuesto, y la referencia esta en el
// comentario. Corre en milisegundos:
//
//   g++ -std=c++20 -O2 -I src tools/test_resolve.cpp -o /tmp/tr.exe && /tmp/tr.exe
//
// Esto es lo que rompe la dependencia de "necesito una corrida para saber si
// rompi la politica": tres de los seis fallos del dia se ven aca.
#include <cstdio>
#include "resolve.h"

using namespace policy;

static int failures = 0;

static void check(const char *label, long got, long expected) {
    const bool ok = got == expected;
    if (!ok) ++failures;
    printf("  %-58s %s (dio %ld, esperado %ld)\n", label, ok ? "ok  " : "FALLA", got, expected);
}

int main(void) {
    printf("politica -- casos sacados de mediciones reales\n\n");

    // ---- FALLO 3: GTA V escribe eOff al pausar, 170 veces por segundo.
    // Medido: forzarle eOn encima daba rafagas de 1000-2000 fps sin presentar.
    // El modo tiene que pasar tal cual, aunque haya un objetivo alto.
    printf("fallo 3 -- el eOff del juego manda\n");
    {
        GameRequest g{ kOff, 3 };
        Decision d{ 600, 6 };
        PluginRequest r = resolve(g, Dialect::GENERATED, Dialect::MULTIPLIER, d);
        check("modo del juego eOff con objetivo 6.00 -> sale eOff", r.mode, kOff);
        check("  y la cuenta no se inventa", r.count, 0);
    }

    // ---- FALLO 2: Halo pide count 1 en su dialecto (snippet de julio,
    // GENERADOS), o sea 2X. Con nuestro snippet la cuenta es el MULTIPLICADOR:
    // escribir 1 seria 1X, la mitad de lo que pidio.
    printf("\nfallo 2 -- la cuenta del juego esta en SU dialecto\n");
    {
        GameRequest g{ kOn, 1 };
        Decision d{ 0, 6 };            // sin objetivo: se respeta al juego
        PluginRequest r = resolve(g, Dialect::GENERATED, Dialect::MULTIPLIER, d);
        check("Halo pide 1 (=2X en generados) -> escribimos 2 (=2X)", r.count, 2);
    }
    {
        // Y al reves: si el nuestro hablara generados, 2X se escribe como 1.
        GameRequest g{ kOn, 2 };
        Decision d{ 0, 6 };
        PluginRequest r = resolve(g, Dialect::MULTIPLIER, Dialect::GENERATED, d);
        check("mismo 2X hacia un snippet que habla generados -> 1", r.count, 1);
    }

    // ---- FALLO 4: la semilla de DYNAMIC era el literal 1 = 1X con la semantica
    // nueva. Sin generacion no hay base que medir y el controlador se espera a
    // si mismo. La semilla tiene que salir de lo que pidio el juego.
    printf("\nfallo 4 -- la semilla sale del juego, no de un literal\n");
    {
        GameRequest g{ kOn, 1 };       // Halo: 2X en generados
        Decision d{ 0, 6 };
        PluginRequest r = resolve(g, Dialect::GENERATED, Dialect::MULTIPLIER, d);
        check("sin objetivo, la cuenta nunca es 1 (=1X, no genera)", r.count >= 2 ? 1 : 0, 1);
    }

    // ---- El objetivo manda cuando existe, y se acota al tope del binario vivo.
    printf("\nobjetivo y tope\n");
    {
        GameRequest g{ kOn, 1 };
        check("objetivo 4.00 con tope 6 -> 4",
                 resolve(g, Dialect::GENERATED, Dialect::MULTIPLIER, Decision{400, 6}).count, 4);
        check("objetivo 6.00 con tope 3 -> 3 (no se pide de mas)",
                 resolve(g, Dialect::GENERATED, Dialect::MULTIPLIER, Decision{600, 3}).count, 3);
        check("objetivo 1.00 -> 2, que es el piso de un modo que genera",
                 resolve(g, Dialect::GENERATED, Dialect::MULTIPLIER, Decision{100, 6}).count, 2);
    }

    // ---- IDEMPOTENCIA: GTA V llama 170 veces por segundo con lo mismo. La
    // misma entrada tiene que dar la misma salida, siempre; "cambio la cuenta"
    // se define contra lo ENVIADO, no contra la llamada recibida.
    printf("\nidempotencia\n");
    {
        GameRequest g{ kOn, 3 };
        Decision d{ 400, 6 };
        PluginRequest a = resolve(g, Dialect::GENERATED, Dialect::MULTIPLIER, d);
        bool same = true;
        for (int i = 0; i < 170; ++i) {
            PluginRequest b = resolve(g, Dialect::GENERATED, Dialect::MULTIPLIER, d);
            if (b.mode != a.mode || b.count != a.count) same = false;
        }
        check("170 llamadas identicas dan la misma salida", same ? 1 : 0, 1);
    }

    printf("\n%s\n", failures == 0 ? "todos los casos en verde"
                                 : "HAY CASOS EN ROJO");
    return failures ? 1 : 0;
}
