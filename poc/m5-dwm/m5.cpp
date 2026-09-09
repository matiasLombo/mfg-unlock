// M5: que da DwmGetCompositionTimingInfo, y si distingue la app del escritorio.
//
// Es una API de composicion del escritorio, no del swapchain: no necesita
// privilegios ni enganchar nada. La pregunta es si sirve para contar las
// presentaciones de UNA aplicacion, que es lo que necesitamos.
//
// La POC presenta una cantidad conocida y mira si algun contador de DWM se mueve
// en la misma proporcion. Si DWM cuenta refrescos del escritorio y no
// presentaciones de la app, los numeros no van a seguirse.
//
// Compilar:
//   g++ -std=c++17 -O2 -o m5.exe m5.cpp -ld3d11 -ldxgi -ldwmapi -luser32 -lole32

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <cstdio>

int main(void) {
    const int kN = 120;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"m5_dwm";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, L"m5_dwm", L"m5_dwm", WS_OVERLAPPEDWINDOW,
                             0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(w, SW_SHOWNOACTIVATE);

    DXGI_SWAP_CHAIN_DESC d = {};
    d.BufferCount = 2;
    d.BufferDesc.Width = 320; d.BufferDesc.Height = 240;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = w;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
    IDXGISwapChain *sc = nullptr;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &d, &sc, &dev, &fl, &ctx))) {
        printf("NO SE PUDO PROBAR: no se pudo crear el swapchain\n");
        return 2;
    }

    DWM_TIMING_INFO t0 = {}; t0.cbSize = sizeof(t0);
    const HRESULT h0 = DwmGetCompositionTimingInfo(nullptr, &t0);
    printf("DwmGetCompositionTimingInfo inicial: hr=0x%08lX\n", (unsigned long)h0);
    if (FAILED(h0)) {
        printf("NO ANDA: la llamada fallo\n");
        return 1;
    }
    printf("  cFrame            %llu\n", (unsigned long long)t0.cFrame);
    printf("  cRefresh          %llu\n", (unsigned long long)t0.cRefresh);
    printf("  cDXPresent        %u\n", (unsigned)t0.cDXPresent);
    printf("  cFramesDisplayed  %llu\n", (unsigned long long)t0.cFramesDisplayed);

    for (int i = 0; i < kN; ++i) {
        sc->Present(1, 0);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    }

    DWM_TIMING_INFO t1 = {}; t1.cbSize = sizeof(t1);
    const HRESULT h1 = DwmGetCompositionTimingInfo(nullptr, &t1);
    printf("\npresentadas por la app (verdad): %d\n", kN);
    if (FAILED(h1)) { printf("la segunda llamada fallo: 0x%08lX\n", (unsigned long)h1); return 1; }
    printf("deltas de DWM en el mismo lapso:\n");
    printf("  cFrame            %+lld\n", (long long)(t1.cFrame - t0.cFrame));
    printf("  cRefresh          %+lld\n", (long long)(t1.cRefresh - t0.cRefresh));
    printf("  cDXPresent        %+lld\n", (long long)((long long)t1.cDXPresent - (long long)t0.cDXPresent));
    printf("  cFramesDisplayed  %+lld\n", (long long)(t1.cFramesDisplayed - t0.cFramesDisplayed));
    printf("\nnota: DWM es del escritorio entero. Si algun delta sigue a %d, hay que\n", kN);
    printf("comprobar que no sea coincidencia del refresco del monitor.\n");

    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    return 0;
}
