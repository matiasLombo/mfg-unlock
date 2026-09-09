// M1: observar slDLSSGGetState en vez de consultarlo.
//
// La hipotesis a matar: el campo `numFramesActuallyPresented` del DLSSGState
// cuenta "frames presentados desde la ULTIMA llamada a slDLSSGGetState", y el
// juego tambien la llama. Si es asi, cuando NOSOTROS consultamos solo vemos el
// pedacito desde la ultima llamada del juego -- que es lo que explicaria el
// -94% medido en Cyberpunk (7.7 fps contra 147.3 reales).
//
// Si la hipotesis es cierta, ENGANCHAR la funcion y sumar lo que el plugin le
// devuelve a CUALQUIER llamador tiene que dar el total correcto, porque ningun
// reinicio se pierde.
//
// El testigo. No se compara contra la capa vieja del dll, que es justamente lo
// que no funciona en Halo. Esta POC trae su propio contador de presentaciones
// usando los dos mecanismos que ya se validaron a 0.0% en este mismo directorio:
// se toma la vtable del swapchain (M2) y se cuenta cada Present (M3). Esa es la
// verdad contra la que se mide.
//
// Se compila como DLL y se carga en un proceso que use Streamline. No toca
// src/proxy.cpp ni depende de el.
//
// Compilar:
//   g++ -shared -std=c++17 -O2 -w -I ../../external/minhook/include -o m1.dll \
//       m1.cpp ../../external/minhook/src/hook.c ../../external/minhook/src/buffer.c \
//       ../../external/minhook/src/trampoline.c ../../external/minhook/src/hde/hde64.c \
//       -ld3d11 -ldxgi -luser32 -lole32 -static-libgcc -static-libstdc++

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include "MinHook.h"

static FILE *g_log = nullptr;
static void anotar(const char *fmt, ...) {
    if (g_log == nullptr) return;
    va_list a; va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---- El testigo: presentaciones reales, por la vtable (M2 + M3) -------------

static volatile LONG g_presents = 0;
typedef HRESULT(__stdcall *PFN_Present)(IDXGISwapChain *, UINT, UINT);
static PFN_Present g_orig_present = nullptr;

static HRESULT __stdcall hk_Present(IDXGISwapChain *sc, UINT s, UINT f) {
    InterlockedIncrement(&g_presents);
    return g_orig_present(sc, s, f);
}

// Crea un swapchain propio descartable solo para leerle la vtable.
static bool enganchar_present(void) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"m1_descartable";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, L"m1_descartable", L"", WS_OVERLAPPEDWINDOW,
                             0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (w == nullptr) return false;

    DXGI_SWAP_CHAIN_DESC d = {};
    d.BufferCount = 2;
    d.BufferDesc.Width = 64; d.BufferDesc.Height = 64;
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
        anotar("testigo: no se pudo crear el swapchain descartable");
        return false;
    }

    void **vt = *(void ***)sc;
    g_orig_present = (PFN_Present)vt[8];
    DWORD viejo = 0;
    if (!VirtualProtect(&vt[8], sizeof(void *), PAGE_READWRITE, &viejo)) return false;
    vt[8] = (void *)&hk_Present;
    VirtualProtect(&vt[8], sizeof(void *), viejo, &viejo);
    anotar("testigo: Present enganchado por la vtable del descartable");
    sc->Release(); ctx->Release(); dev->Release();
    return true;
}

// ---- Lo que se investiga: slDLSSGGetState observado --------------------------

static volatile LONG g_estado_llamadas = 0;
static volatile LONG g_estado_frames = 0;    // suma de numFramesActuallyPresented

typedef unsigned (*PFN_GetState)(const void *, void *, const void *);
static PFN_GetState g_orig_getstate = nullptr;

static unsigned hk_GetState(const void *vp, void *estado, const void *opts) {
    const unsigned r = g_orig_getstate(vp, estado, opts);
    if (r == 0 && estado != nullptr) {
        // numFramesActuallyPresented esta en el offset 48 del DLSSGState. El
        // layout esta confirmado por el campo de al lado, el 52, que el dll ya
        // lee bien para el techo del sistema.
        const unsigned n = *(const unsigned *)((const unsigned char *)estado + 48);
        InterlockedIncrement(&g_estado_llamadas);
        if (n <= 1000) InterlockedExchangeAdd(&g_estado_frames, (LONG)n);
    }
    return r;
}

typedef unsigned (*PFN_GetFeatureFunction)(unsigned, const char *, void *&);
static PFN_GetFeatureFunction g_orig_gff = nullptr;

static unsigned hk_GetFeatureFunction(unsigned feature, const char *name, void *&fn) {
    const unsigned r = g_orig_gff(feature, name, fn);
    if (r == 0 && name != nullptr && fn != nullptr &&
        strcmp(name, "slDLSSGGetState") == 0 && g_orig_getstate == nullptr) {
        anotar("slDLSSGGetState entregado al llamador, enganchando");
        if (MH_CreateHook(fn, (void *)&hk_GetState, (void **)&g_orig_getstate) == MH_OK &&
            MH_EnableHook(fn) == MH_OK) {
            anotar("  enganchado OK");
            // Se le devuelve al juego el trampolin, para que sus llamadas pasen
            // por nosotros aunque guarde el puntero.
            fn = (void *)&hk_GetState;
        } else {
            anotar("  NO se pudo enganchar");
            g_orig_getstate = nullptr;
        }
    }
    return r;
}

static DWORD WINAPI hilo(LPVOID) {
    char ruta[MAX_PATH];
    GetTempPathA(MAX_PATH, ruta);
    strcat(ruta, "m1-observado.log");
    g_log = fopen(ruta, "w");
    anotar("M1 arrancado. log en %s", ruta);

    // Convivir, no reemplazar. La POC ocupa el lugar del dll real en el banco,
    // asi que carga al real primero: sin el, el sample termina en STATUS INVALID
    // y no hay nada que observar. La ruta viene por variable de entorno para no
    // clavarla aca.
    {
        char real[MAX_PATH] = {0};
        if (GetEnvironmentVariableA("M1_DLL_REAL", real, MAX_PATH) > 0) {
            const HMODULE h = LoadLibraryA(real);
            anotar("dll real cargado desde %s: %s", real, h ? "OK" : "FALLO");
        } else {
            anotar("sin M1_DLL_REAL: se observa sin el dll real");
        }
    }

    if (MH_Initialize() != MH_OK) { anotar("MinHook no arranco"); return 0; }

    HMODULE si = nullptr;
    for (int i = 0; i < 600 && si == nullptr; ++i) {
        si = GetModuleHandleW(L"sl.interposer.dll");
        if (si == nullptr) Sleep(100);
    }
    if (si == nullptr) { anotar("NO SE PUDO PROBAR: nunca aparecio sl.interposer"); return 0; }
    anotar("sl.interposer mapeado");

    void *f = (void *)GetProcAddress(si, "slGetFeatureFunction");
    if (f == nullptr) { anotar("NO SE PUDO PROBAR: sin slGetFeatureFunction"); return 0; }
    if (MH_CreateHook(f, (void *)&hk_GetFeatureFunction, (void **)&g_orig_gff) != MH_OK ||
        MH_EnableHook(f) != MH_OK) {
        anotar("NO SE PUDO PROBAR: no se pudo enganchar slGetFeatureFunction");
        return 0;
    }
    anotar("slGetFeatureFunction enganchado");

    enganchar_present();

    // Informe cada dos segundos: los dos numeros lado a lado.
    LONG p0 = 0, e0 = 0;
    for (;;) {
        Sleep(2000);
        const LONG p = g_presents, e = g_estado_frames, c = g_estado_llamadas;
        anotar("presentes(testigo) %ld (+%ld)   estado.suma %ld (+%ld)   llamadas %ld",
               p, p - p0, e, e - e0, c);
        p0 = p; e0 = e;
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD razon, LPVOID) {
    if (razon == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, hilo, nullptr, 0, nullptr);
    }
    return TRUE;
}
