// capture.h -- una captura del backbuffer a PNG.
//
// ENTRA: el swapchain del juego, una queue directa y el device.
// SALE: un PNG en %LOCALAPPDATA%\gpuprobe\sessions\.
// DEPENDE DE: core/png.h, d3d12.
//
// Para que: el harness A/B mide la ganancia, pero el COSTO VISUAL no se mide,
// se mira. Dos capturas del mismo punto de la escena, una con la optimizacion
// y otra sin ella, es la unica forma honesta de evaluar si media resolucion en
// el SSR se nota o no.
//
// Esta funcion SI frena la GPU: copia el backbuffer a un readback y espera la
// fence. Es deliberado y esta acotado -- pasa dos veces por candidato, no por
// frame -- y el frame en el que pasa queda marcado para que el analizador no
// lo use como muestra.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace gp {

bool capture_swapchain(ID3D12Device *device, ID3D12CommandQueue *queue,
                       IDXGISwapChain *swapchain, const char *path);

}  // namespace gp
