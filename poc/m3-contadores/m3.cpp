// M3: que devuelven GetLastPresentCount y GetFrameStatistics, y con que
// granularidad.
//
// Los dos son metodos del propio IDXGISwapChain, asi que no necesitan ETW ni
// privilegios ni enganchar nada: alcanza con TENER el swapchain. Combinado con
// M2 -- que demostro que la vtable es compartida -- serian el camino completo.
//
// La POC presenta una cantidad conocida y despues compara. La verdad es el
// contador del bucle, que lo escribimos nosotros.
//
// Se prueba en dos modos, porque la documentacion dice que GetFrameStatistics
// solo funciona en algunos:
//
//   DISCARD    -- el modo clasico
//   FLIP_DISCARD -- el moderno, que es el que usan los juegos nuevos
//
// Compilar:
//   g++ -std=c++17 -O2 -o m3.exe m3.cpp -ld3d11 -ldxgi -luser32 -lole32

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>

static HWND crear_ventana(const wchar_t *nombre) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = nombre;
    RegisterClassExW(&wc);
    HWND h = CreateWindowExW(0, nombre, nombre, WS_OVERLAPPEDWINDOW,
                             0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(h, SW_SHOWNOACTIVATE);   // visible: sin mostrar no hay scanout
    return h;
}

static void probar(const wchar_t *nombre, DXGI_SWAP_EFFECT efecto,
                   const char *etiqueta, int presentaciones) {
    printf("\n=== %s\n", etiqueta);
    HWND w = crear_ventana(nombre);
    DXGI_SWAP_CHAIN_DESC d = {};
    d.BufferCount = 2;
    d.BufferDesc.Width = 320;
    d.BufferDesc.Height = 240;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = w;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    d.SwapEffect = efecto;

    ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
    IDXGISwapChain *sc = nullptr;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &d, &sc, &dev, &fl, &ctx))) {
        printf("  no se pudo crear el swapchain en este modo\n");
        return;
    }

    UINT antes = 0;
    const HRESULT hr0 = sc->GetLastPresentCount(&antes);
    printf("  GetLastPresentCount inicial: hr=0x%08lX valor=%u\n",
           (unsigned long)hr0, antes);

    for (int i = 0; i < presentaciones; ++i) {
        sc->Present(1, 0);            // con vsync, para que llegue a pantalla
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    }

    UINT despues = 0;
    const HRESULT hr1 = sc->GetLastPresentCount(&despues);
    DXGI_FRAME_STATISTICS fs = {};
    const HRESULT hr2 = sc->GetFrameStatistics(&fs);

    printf("  presentadas (verdad)       : %d\n", presentaciones);
    printf("  GetLastPresentCount final  : hr=0x%08lX valor=%u  delta=%d\n",
           (unsigned long)hr1, despues, (int)(despues - antes));
    if (SUCCEEDED(hr2)) {
        printf("  GetFrameStatistics         : hr=OK\n");
        printf("    PresentCount             : %u\n", fs.PresentCount);
        printf("    PresentRefreshCount      : %u\n", fs.PresentRefreshCount);
        printf("    SyncRefreshCount         : %u\n", fs.SyncRefreshCount);
    } else {
        printf("  GetFrameStatistics         : hr=0x%08lX (falla)\n",
               (unsigned long)hr2);
        if (hr2 == DXGI_ERROR_FRAME_STATISTICS_DISJOINT)
            printf("    = DXGI_ERROR_FRAME_STATISTICS_DISJOINT\n");
    }

    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(w);
}

int main(void) {
    const int kN = 120;
    probar(L"m3_discard", DXGI_SWAP_EFFECT_DISCARD, "modo DISCARD (clasico)", kN);
    probar(L"m3_flip", DXGI_SWAP_EFFECT_FLIP_DISCARD, "modo FLIP_DISCARD (moderno)", kN);
    return 0;
}
