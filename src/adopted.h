// adopted.h -- los swapchains adoptados: cual se usa para contar, y cuando se
// suelta. Pura: quien esta vivo y cuanto presento se le inyectan.
//
// Se cuenta el MAS NUEVO. La version anterior (befac9d) elegia "el que mas
// presento en el ultimo periodo" para que un swapchain transitorio (GTA V,
// CreateSwapChain flags 2, 70-131 s) no desplazara al del juego -- y para eso
// leia GetLastPresentCount de TODOS los adoptados por frame. El viejo es
// justo el que el juego destruye al crear el nuevo, y una pagina liberada
// sigue mapeada: GTA V 11/09 19:0x, al pasar del menu al juego,
// 0xC0000005 en ntdll+0xfa7d = EnterCriticalSection sobre memoria a cero
// (la misma firma que el crash de salida de Cyberpunk). Sin un hook de
// Release -- que el overlay de Steam prohibe, [[never-byte-detour-present]]
// -- no hay forma de saber que un chain murio, asi que solo se toca el que
// mas probablemente vive: el ultimo que el proceso creo. El transitorio
// cuenta mal mientras vive (0.33 en el menu de GTA V) y es cosmetico.
//
// ENTRA: adopt(chain) desde hook_swapchain_present; forget(chain) desde el
//   descartable propio; pick(alive, count) una vez por frame renderizado,
//   con un predicado de vida (VirtualQuery + modulo de la vtable) y un lector
//   de GetLastPresentCount, que se aplican SOLO al elegido.
// SALE: pick devuelve el swapchain elegido (o nullptr) y su cuenta; current()
//   el ultimo elegido sin releer nada.
// DEPENDE DE: nada. Test: tools/test_adopted.cpp.
#pragma once

namespace adopted {

constexpr int kMax = 4;

struct List {
    void *e[kMax];   // el mas nuevo primero
    int n = 0;

    int find(const void *c) const {
        for (int i = 0; i < n; ++i) if (e[i] == c) return i;
        return -1;
    }
    void adopt(void *c) {
        if (c == nullptr || find(c) >= 0) return;
        if (n == kMax) --n;
        for (int i = n; i > 0; --i) e[i] = e[i - 1];
        e[0] = c;
        ++n;
    }
    void remove_at(int i) {
        for (int j = i; j < n - 1; ++j) e[j] = e[j + 1];
        --n;
    }
    void forget(const void *c) {
        const int i = find(c);
        if (i >= 0) remove_at(i);
    }
    void *current() const { return n > 0 ? e[0] : nullptr; }

    // Una vez por frame renderizado: el mas nuevo que siga vivo, y su cuenta.
    // Solo se toca el elegido; si murio se suelta y se prueba el siguiente.
    template <class Alive, class Count>
    void *pick(Alive alive, Count count, unsigned *count_out, bool *dropped) {
        if (dropped != nullptr) *dropped = false;
        if (count_out != nullptr) *count_out = 0;
        while (n > 0) {
            if (alive(e[0])) {
                unsigned c = 0;
                if (count(e[0], &c) && count_out != nullptr) *count_out = c;
                return e[0];
            }
            remove_at(0);
            if (dropped != nullptr) *dropped = true;
        }
        return nullptr;
    }
};

}  // namespace adopted
