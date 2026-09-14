// hooks.h -- el enganche por vtable. Ver hooks.cpp para el por que.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace gp {

// Engancha lo que cuelga de un swapchain recien creado: la propia swapchain,
// la command queue con la que se creo, y el device de esa queue. Es el unico
// punto de entrada -- todo lo demas se descubre desde aca.
void hooks_attach_swapchain(IDXGISwapChain *swapchain, IUnknown *device_or_queue);

// Engancha la fabrica para ver nacer las swapchains.
void hooks_attach_factory(IDXGIFactory *factory);

// Roba las vtables creando objetos descartables propios. Devuelve false si no
// se pudo (no hay D3D12 en esta maquina, o el juego no lo usa): eso NO es un
// error, es un juego que no nos interesa.
//
// Funciona porque las vtables de D3D12 y DXGI viven en el runtime (d3d12core,
// dxgi) y son las MISMAS para cualquier device del proceso. Enganchar la de un
// device descartable propio engancha, por construccion, la del juego. Es la
// tecnica que este repositorio ya valido para el swapchain (ver README:
// "adoptar por un descartable D3D12"), y evita depender de un trampolin sobre
// funciones exportadas.
//
// Limite conocido: si el juego carga su propio runtime por Agility SDK, ese
// d3d12core es otro modulo y sus vtables son otras. En ese caso gpuprobe no
// engancha nada y se queda en passthrough, que es el comportamiento correcto.
bool hooks_steal_vtables();

// Cuantas vtables distintas llevamos enganchadas de cada cosa. Va al log y al
// overlay: un juego que recrea el swapchain con otra vtable es exactamente el
// caso que rompio este proyecto una vez.
struct HookStats {
    unsigned factories = 0;
    unsigned swapchains = 0;
    unsigned devices = 0;
    unsigned queues = 0;
    unsigned lists = 0;
};
HookStats hooks_stats();

}  // namespace gp
