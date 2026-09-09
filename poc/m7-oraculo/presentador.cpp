// Presentador de referencia para M7.
//
// Presenta una cantidad conocida de frames y al final imprime su propio
// contador. Esa es la verdad: la app ES la que llama a Present.
//
// Sirve para darle al oraculo (PresentMon) algo que medir, y para comparar su
// numero contra uno que no estima nada.
//
// Uso:  presentador.exe [segundos]
//
// Compilar:
//   g++ -std=c++17 -O2 -o presentador.exe presentador.cpp -ld3d11 -ldxgi -luser32 -lole32

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>

int main(int argc, char **argv) {
    const int segundos = (argc > 1) ? atoi(argv[1]) : 20;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"presentador";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, L"presentador", L"presentador (M7)",
                             WS_OVERLAPPEDWINDOW, 80, 80, 480, 320,
                             nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(w, SW_SHOW);

    DXGI_SWAP_CHAIN_DESC d = {};
    d.BufferCount = 2;
    d.BufferDesc.Width = 480; d.BufferDesc.Height = 320;
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
        printf("no se pudo crear el swapchain\n");
        return 2;
    }

    // Algo que cambie en pantalla, para que no lo tomen por una ventana quieta.
    ID3D11Texture2D *back = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
    if (back) { dev->CreateRenderTargetView(back, nullptr, &rtv); back->Release(); }

    LARGE_INTEGER freq, ini, ahora;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ini);

    long long presentadas = 0;
    for (;;) {
        QueryPerformanceCounter(&ahora);
        const double t = (double)(ahora.QuadPart - ini.QuadPart) / (double)freq.QuadPart;
        if (t >= (double)segundos) break;

        if (rtv) {
            const float c[4] = { (float)(0.5 + 0.5 * sin(t * 3.0)), 0.15f, 0.35f, 1.0f };
            ctx->ClearRenderTargetView(rtv, c);
        }
        sc->Present(1, 0);
        ++presentadas;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    }

    QueryPerformanceCounter(&ahora);
    const double dur = (double)(ahora.QuadPart - ini.QuadPart) / (double)freq.QuadPart;
    printf("PRESENTADOR presentadas=%lld segundos=%.3f fps=%.2f\n",
           presentadas, dur, (double)presentadas / dur);
    fflush(stdout);

    if (rtv) rtv->Release();
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    return 0;
}
