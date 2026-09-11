// adopted.h -- los swapchains adoptados: cual se usa para contar, y cuando se
// suelta. Pura: quien esta vivo y cuanto presento se le inyectan.
//
// "La ultima instancia gana" fallo dos veces el 11/09 en GTA V: un swapchain
// transitorio (CreateSwapChain flags 2, no el del juego) se adopto, lo
// liberaron, y el puntero colgado crasheo a los 111 s; con la comprobacion
// de vida puesta, mientras el transitorio vivio (70-131 s) el contador lo
// leia a el, que presentaba a un tercio del juego (0.33 en el menu). Aca se
// elige el que MAS presento en el ultimo periodo, entre los que siguen vivos.
//
// ENTRA: adopt(chain) desde hook_swapchain_present; forget(chain) desde el
//   descartable propio; pick(alive, count) una vez por frame renderizado,
//   con un predicado de vida (VirtualQuery + modulo de la vtable) y un lector
//   de GetLastPresentCount.
// SALE: pick devuelve el swapchain elegido (o nullptr) y su cuenta; current()
//   el ultimo elegido sin releer nada.
// DEPENDE DE: nada. Test: tools/test_adopted.cpp.
#pragma once

namespace adopted {

constexpr int kMax = 4;
constexpr int kPeriod = 45;   // frames renderizados entre decisiones: una ventana de medicion

struct Entry {
    void *chain;
    unsigned last_count;   // ultimo GetLastPresentCount leido
    unsigned delta;        // presentaciones acumuladas desde la ultima decision
    bool seen;             // ya tiene un last_count valido
};

struct List {
    Entry e[kMax];
    int n = 0;
    void *chosen = nullptr;
    int calls = 0;

    int find(const void *c) const {
        for (int i = 0; i < n; ++i) if (e[i].chain == c) return i;
        return -1;
    }
    // El mas nuevo primero. Si ya estaba, no se mueve.
    void adopt(void *c) {
        if (c == nullptr || find(c) >= 0) return;
        if (n == kMax) { remove_at(kMax - 1); }
        for (int i = n; i > 0; --i) e[i] = e[i - 1];
        e[0] = Entry{ c, 0, 0, false };
        ++n;
        if (chosen == nullptr) chosen = c;
    }
    void remove_at(int i) {
        const void *c = e[i].chain;
        for (int j = i; j < n - 1; ++j) e[j] = e[j + 1];
        --n;
        if (chosen == c) chosen = n > 0 ? e[0].chain : nullptr;
    }
    void forget(const void *c) {
        const int i = find(c);
        if (i >= 0) remove_at(i);
    }
    void *current() const { return chosen; }

    // Una vez por frame renderizado. `alive(chain)` dice si se puede tocar;
    // `count(chain, &out)` lee la cuenta del runtime y dice si pudo. Los
    // muertos se sueltan al pasar. Cada kPeriod llamadas se elige el de mayor
    // delta (empate: el mas nuevo) y los deltas vuelven a cero.
    template <class Alive, class Count>
    void *pick(Alive alive, Count count, unsigned *count_out, bool *dropped) {
        if (dropped != nullptr) *dropped = false;
        for (int i = 0; i < n;) {
            if (!alive(e[i].chain)) { remove_at(i); if (dropped != nullptr) *dropped = true; continue; }
            unsigned c = 0;
            if (count(e[i].chain, &c)) {
                if (e[i].seen && c >= e[i].last_count) e[i].delta += c - e[i].last_count;
                e[i].last_count = c;
                e[i].seen = true;
            }
            ++i;
        }
        if (n == 0) { chosen = nullptr; return nullptr; }
        if (chosen == nullptr || find(chosen) < 0) chosen = e[0].chain;
        if (++calls >= kPeriod) {
            calls = 0;
            int best = 0;
            for (int i = 1; i < n; ++i) if (e[i].delta > e[best].delta) best = i;
            chosen = e[best].chain;
            for (int i = 0; i < n; ++i) e[i].delta = 0;
        }
        const int k = find(chosen);
        if (count_out != nullptr) *count_out = k >= 0 ? e[k].last_count : 0;
        return chosen;
    }
};

}  // namespace adopted
