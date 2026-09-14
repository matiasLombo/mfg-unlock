// overlay.h -- la tabla de pasadas en pantalla, con toggles en vivo.
//
// Se compila solo si hay ImGui en external/imgui (ver build-gpuprobe.sh). Sin
// ImGui, gpuprobe funciona igual: mide, analiza y aplica -- lo unico que falta
// es poder mirarlo mientras pasa. Por eso el overlay es opcional y no una
// dependencia del resto.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace gp {

// Se llama en cada Present, ANTES del Present real. Devuelve false si el
// overlay no esta disponible o no se pudo inicializar; en ese caso no se
// vuelve a intentar.
bool overlay_draw(ID3D12Device *device, ID3D12CommandQueue *queue,
                  IDXGISwapChain *swapchain);

// F10 lo prende y lo apaga. Devuelve el estado nuevo.
bool overlay_toggle();
bool overlay_visible();

void overlay_shutdown();

}  // namespace gp
