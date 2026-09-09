// M2: enganchar Present tomando la vtable de un swapchain PROPIO descartable.
//
// La hipotesis a probar: la vtable la comparten todas las instancias creadas por
// la misma implementacion de DXGI, asi que si creamos un swapchain nuestro, le
// leemos la vtable y parcheamos la ranura de Present, quedan enganchados TODOS
// los swapchains del proceso -- incluido el del juego, sin importar quien
// envolvio su factory.
//
// Esto importa porque en Halo nuestro dll engancha CreateSwapChainForHwnd en la
// factory y nunca ve el swapchain: cero lineas de swapchain en todo el log.
//
// La POC se valida sola. Hay dos swapchains:
//
//   A -- el "del juego". Se crea PRIMERO y se presenta N veces.
//   B -- el nuestro, descartable, creado DESPUES, sobre otra ventana.
//
// Se parchea la vtable a traves de B y se cuenta cuantas veces se llamo el hook
// mientras presentabamos por A. La verdad es N, que la conocemos porque la
// escribimos nosotros.
//
// Compilar:
//   g++ -std=c++17 -O2 -o m2.exe m2.cpp -ld3d11 -ldxgi -luser32 -lole32
//
// Salida esperada si la hipotesis es cierta: enganchadas == presentadas.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>

static volatile LONG g_hook_calls = 0;

typedef HRESULT(__stdcall *PFN_Present)(IDXGISwapChain *, UINT, UINT);
static PFN_Present g_orig_present = nullptr;

static HRESULT __stdcall hk_Present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    InterlockedIncrement(&g_hook_calls);
    return g_orig_present(sc, sync, flags);
}

// Ventana minima, invisible: no queremos molestar a nadie en pantalla.
static HWND crear_ventana(const wchar_t *nombre) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = nombre;
    RegisterClassExW(&wc);
    return CreateWindowExW(0, nombre, nombre, WS_OVERLAPPEDWINDOW,
                           0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
}

static bool crear_swapchain(HWND hwnd, ID3D11Device **dev,
                            ID3D11DeviceContext **ctx, IDXGISwapChain **sc) {
    DXGI_SWAP_CHAIN_DESC d = {};
    d.BufferCount = 2;
    d.BufferDesc.Width = 320;
    d.BufferDesc.Height = 240;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = hwnd;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl;
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &d, sc, dev, &fl, ctx);
    return SUCCEEDED(hr);
}

int main(void) {
    const int kPresentaciones = 300;

    // A: el swapchain "del juego". Se crea primero, a proposito.
    HWND wa = crear_ventana(L"m2_juego");
    ID3D11Device *devA = nullptr; ID3D11DeviceContext *ctxA = nullptr;
    IDXGISwapChain *scA = nullptr;
    if (!crear_swapchain(wa, &devA, &ctxA, &scA)) {
        printf("NO SE PUDO PROBAR: no se pudo crear el swapchain A\n");
        return 2;
    }

    // B: el nuestro, descartable. Otra ventana, creado despues.
    HWND wb = crear_ventana(L"m2_descartable");
    ID3D11Device *devB = nullptr; ID3D11DeviceContext *ctxB = nullptr;
    IDXGISwapChain *scB = nullptr;
    if (!crear_swapchain(wb, &devB, &ctxB, &scB)) {
        printf("NO SE PUDO PROBAR: no se pudo crear el swapchain B\n");
        return 2;
    }

    void **vtA = *(void ***)scA;
    void **vtB = *(void ***)scB;
    printf("vtable de A: %p\n", (void *)vtA);
    printf("vtable de B: %p\n", (void *)vtB);
    printf("comparten vtable: %s\n", vtA == vtB ? "SI" : "NO");

    // Present es la ranura 8: IUnknown 0-2, IDXGIObject 3-6,
    // IDXGIDeviceSubObject::GetDevice 7, IDXGISwapChain::Present 8.
    const int kRanuraPresent = 8;
    g_orig_present = (PFN_Present)vtB[kRanuraPresent];

    DWORD viejo = 0;
    if (!VirtualProtect(&vtB[kRanuraPresent], sizeof(void *),
                        PAGE_READWRITE, &viejo)) {
        printf("NO SE PUDO PROBAR: VirtualProtect fallo, %lu\n", GetLastError());
        return 2;
    }
    vtB[kRanuraPresent] = (void *)&hk_Present;
    VirtualProtect(&vtB[kRanuraPresent], sizeof(void *), viejo, &viejo);
    printf("ranura parcheada a traves de B\n");

    // Ahora se presenta SOLO por A, que es el que no tocamos.
    for (int i = 0; i < kPresentaciones; ++i) {
        scA->Present(0, DXGI_PRESENT_TEST);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    }

    const LONG contadas = g_hook_calls;
    printf("\npresentadas por A (verdad): %d\n", kPresentaciones);
    printf("contadas por el hook      : %ld\n", contadas);
    if (kPresentaciones > 0) {
        const double dif = 100.0 * (double)(contadas - kPresentaciones) /
                           (double)kPresentaciones;
        printf("diferencia                : %+.1f%%\n", dif);
    }

    // Restaurar antes de salir, por prolijidad.
    if (VirtualProtect(&vtB[kRanuraPresent], sizeof(void *),
                       PAGE_READWRITE, &viejo)) {
        vtB[kRanuraPresent] = (void *)g_orig_present;
        VirtualProtect(&vtB[kRanuraPresent], sizeof(void *), viejo, &viejo);
    }
    if (scB) scB->Release();
    if (ctxB) ctxB->Release();
    if (devB) devB->Release();
    if (scA) scA->Release();
    if (ctxA) ctxA->Release();
    if (devA) devA->Release();
    return contadas == kPresentaciones ? 0 : 1;
}
