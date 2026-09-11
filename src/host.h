// host.h -- nosotros como aplicacion: Streamline y DLSS-G en un juego que no
// los trae (experimento, `host 1` en mfg-config.txt).
//
// Lo que hacemos en los tres juegos es parchear una aplicacion que YA llama a
// Streamline. Un juego con DLSS 2 solo (Metro Exodus EE: nvngx_dlss.dll 2.1,
// sin sl.*) no llama a nadie, asi que las cinco cosas que una aplicacion
// hace las hacemos nosotros:
//
//   1. slInit con nuestro set (la cache de %LOCALAPPDATA%\mfg-unlock\sdk),
//      antes de que el juego cree la fabrica DXGI.
//   2. Devolverle al juego los PROXIES de Streamline: sus llamadas a
//      CreateDXGIFactory* y D3D12CreateDevice se redirigen a los exports del
//      interposer, que devuelven objetos proxy; asi el swapchain que el juego
//      crea pasa por SL y DLSS-G puede interceptar Present (es el modo de
//      hooking global del SDK, sin tocar el exe).
//   3. Por frame, en el hook de NVSDK_NGX_D3D12_EvaluateFeature (DLSS-SR):
//      sacar de los parametros de NGX la profundidad, los vectores de
//      movimiento, la salida (que es el color SIN HUD: DLSS corre antes del
//      HUD) y el jitter, y tagearlos con slSetTag; constantes de camara con
//      matrices identidad (el juego no las expone; la calidad lo paga).
//   4. Marcadores de PCL: sim/render en el hook de NGX, present en nuestro
//      hook de Present sobre el proxy. Sin ellos DLSS-G se apaga
//      ([[reflex-goes-stale-in-gtav]]).
//   5. slDLSSGSetOptions eOn, por el interposer: pasa por nuestro
//      hk_slDLSSGSetOptions y force_into aplica la seleccion, como en
//      cualquier juego. Y por eso MANDA mfg-settings: con `mode 0` la
//      politica deja el byte de cadencia en 1 y el plugin evalua DLSS-G cada
//      frame para presentar 1:1 (medido: 45 presents por 45 frames, con el
//      plugin diciendo "enabled"). Con `mode 2` da 2.00 exacto.
//
// Medido en Metro Exodus EE, 2026-09-11, menu principal (DLSS 720p->1440p):
//   base 89-90 fps, PresentCount 90 por 45 frames en 138 ventanas seguidas,
//   sin hitches, sl.log limpio. Es la primera generacion de frames en un
//   juego que no trae Streamline.
//
// Lo que hizo falta, en el orden en que fallo (cada uno una corrida):
//   - slSetD3DDevice: en SL 2.x el host lo llama; se hace al crear el
//     swapchain con el device de la cola, porque Metro crea y destruye un
//     device de sondeo antes del real.
//   - projectId + engineVersion: el build PRODUCTION de sl.common rechaza el
//     app id temporal (100721531) y apaga NGX; NGX exige un GUID.
//   - nvngx_dlssg.dll al lado del exe: NGX decide "DLSS-G available" por la
//     existencia del archivo ahi (despues lo carga el nuestro por el loader).
//   - La fabrica PROXY se engancha aparte (hook_factory_host): hook_factory ya
//     habia tomado la real y el slot de Present quedaba sin escribir.
//   - Reflex: slReflexSetOptions(eLowLatency) + slReflexSleep por frame, o
//     "eDLSSGStatusFailReflexNotDetectedAtRuntime".
//
// ENTRA: la configuracion (g_host_on), CreateDXGIFactory* / D3D12CreateDevice
//   (hooks en present.h), NVSDK_NGX_D3D12_EvaluateFeature de _nvngx.dll
//   (host_poll lo engancha cuando el modulo aparece), Present (present.h).
// SALE: las llamadas a SL de arriba; g_host_tok (el token del frame en curso)
//   para los marcadores de Present; lineas "host:" en el log.
// DEPENDE DE: los headers del SDK (external/streamline/include, sacados del
//   zip de la cache), MinHook, loader.h (la ruta de la cache), y los
//   globales compartidos.
//
// Los parametros de NGX se leen por la vtable de NVSDK_NGX_Parameter con el
// orden de MSVC (las sobrecargas de un mismo nombre van en orden INVERSO de
// declaracion): Set(void*)=0 ... Set(ull)=7, Get(void**)=8,
// Get(ID3D12Resource**)=9, Get(int*)=11, Get(unsigned*)=12, Get(float*)=14.
// Se comprueba en el primer frame leyendo "Width" por los dos ordenes.
#pragma once
#include <d3d12.h>
#include "sl.h"
#include "sl_consts.h"
#include "sl_dlss_g.h"
#include "sl_pcl.h"
#include "sl_reflex.h"

// Vive en loader.h, que se incluye despues.
static bool path_in_our_sdk(const wchar_t *module, unsigned minor, wchar_t *out);
// Vive en present.h (se incluye despues): el swapchain adoptado, comprobado.
static IDXGISwapChain *live_swapchain(void);

static bool g_host_on = false;                 // config `host 1`
static bool g_host_hudless = false;            // config `hosthudless 1`: taggear la salida de DLSS como sin-HUD (se ve mal, ver abajo)
static HMODULE g_host_sl = nullptr;            // sl.interposer.dll, el nuestro
static bool g_host_inited = false;
static sl::FrameToken *g_host_tok = nullptr;   // el token del frame en curso
static volatile LONG g_host_reentry = 0;       // los exports del interposer llaman a los de sistema

// Lo que pedimos del interposer, por GetProcAddress.
static PFun_slInit *h_slInit = nullptr;
static PFun_slGetNewFrameToken *h_slGetNewFrameToken = nullptr;
static PFun_slSetConstants *h_slSetConstants = nullptr;
static PFun_slSetTag *h_slSetTag = nullptr;
static PFun_slGetFeatureFunction *h_slGetFeatureFunction = nullptr;
static PFun_slIsFeatureLoaded *h_slIsFeatureLoaded = nullptr;
static PFun_slSetD3DDevice *h_slSetD3DDevice = nullptr;
typedef HRESULT(WINAPI *PFN_HostCreateFactory)(REFIID, void **);
typedef HRESULT(WINAPI *PFN_HostCreateFactory2)(UINT, REFIID, void **);
typedef HRESULT(WINAPI *PFN_HostD3D12CreateDevice)(IUnknown *, int, REFIID, void **);
static PFN_HostCreateFactory h_CreateDXGIFactory = nullptr, h_CreateDXGIFactory1 = nullptr;
static PFN_HostCreateFactory2 h_CreateDXGIFactory2 = nullptr;
static PFN_HostD3D12CreateDevice h_D3D12CreateDevice = nullptr;
static PFun_slPCLSetMarker *h_slPCLSetMarker = nullptr;
static PFun_slDLSSGSetOptions *h_slDLSSGSetOptions = nullptr;
static PFun_slDLSSGGetState *h_slDLSSGGetState = nullptr;
static PFun_slReflexSetOptions *h_slReflexSetOptions = nullptr;
static PFun_slReflexSleep *h_slReflexSleep = nullptr;

static const sl::Feature kHostFeatures[] = { sl::kFeatureDLSS_G, sl::kFeaturePCL, sl::kFeatureReflex };
static wchar_t g_host_sdk_dir[MAX_PATH];
static const wchar_t *g_host_sdk_dirs[1];
static wchar_t g_host_log_dir[MAX_PATH];

static void host_sl_log(sl::LogType type, const char *msg) {
    // Lo mismo que sl.log, pero en nuestro log y solo lo que importa: errores
    // y avisos. Lo verboso queda en sl.log si `sllog 1`.
    if (type == sl::LogType::eInfo) return;
    char line[240];
    int k = 0;
    const char *pre = type == sl::LogType::eError ? "host: SL ERROR " : "host: SL warn ";
    while (pre[k] != 0) { line[k] = pre[k]; ++k; }
    for (int i = 0; msg[i] != 0 && msg[i] != '\n' && k < 238; ++i) line[k++] = msg[i];
    line[k] = 0;
    log_line(line);
}

// Cargar el interposer de la cache y hacer slInit. Corre en el hilo del juego,
// en la primera CreateDXGIFactory*, nunca bajo el loader lock.
static bool host_init(void) {
    if (g_host_inited) return g_host_sl != nullptr;
    g_host_inited = true;
    if (!path_in_our_sdk(L"sl.interposer.dll", 12, g_host_sdk_dir)) {
        log_line("host: no esta sl.interposer.dll en la cache 2.12; sin modo host");
        return false;
    }
    g_host_sl = LoadLibraryW(g_host_sdk_dir);
    if (g_host_sl == nullptr) {
        log_num("host: LoadLibrary del interposer fallo, error ", (unsigned)GetLastError());
        return false;
    }
    // La carpeta, para pathsToPlugins.
    int n = 0;
    while (g_host_sdk_dir[n] != 0) ++n;
    while (n > 0 && g_host_sdk_dir[n - 1] != L'\\') --n;
    g_host_sdk_dir[n > 0 ? n - 1 : 0] = 0;
    g_host_sdk_dirs[0] = g_host_sdk_dir;
    beside_dll(g_host_log_dir, L"");
    {
        int m = 0;
        while (g_host_log_dir[m] != 0) ++m;
        if (m > 0 && g_host_log_dir[m - 1] == L'\\') g_host_log_dir[m - 1] = 0;
    }
    h_slInit = reinterpret_cast<PFun_slInit *>(GetProcAddress(g_host_sl, "slInit"));
    h_slGetNewFrameToken = reinterpret_cast<PFun_slGetNewFrameToken *>(GetProcAddress(g_host_sl, "slGetNewFrameToken"));
    h_slSetConstants = reinterpret_cast<PFun_slSetConstants *>(GetProcAddress(g_host_sl, "slSetConstants"));
    h_slSetTag = reinterpret_cast<PFun_slSetTag *>(GetProcAddress(g_host_sl, "slSetTag"));
    h_slGetFeatureFunction = reinterpret_cast<PFun_slGetFeatureFunction *>(GetProcAddress(g_host_sl, "slGetFeatureFunction"));
    h_slIsFeatureLoaded = reinterpret_cast<PFun_slIsFeatureLoaded *>(GetProcAddress(g_host_sl, "slIsFeatureLoaded"));
    h_slSetD3DDevice = reinterpret_cast<PFun_slSetD3DDevice *>(GetProcAddress(g_host_sl, "slSetD3DDevice"));
    h_CreateDXGIFactory = reinterpret_cast<PFN_HostCreateFactory>(GetProcAddress(g_host_sl, "CreateDXGIFactory"));
    h_CreateDXGIFactory1 = reinterpret_cast<PFN_HostCreateFactory>(GetProcAddress(g_host_sl, "CreateDXGIFactory1"));
    h_CreateDXGIFactory2 = reinterpret_cast<PFN_HostCreateFactory2>(GetProcAddress(g_host_sl, "CreateDXGIFactory2"));
    h_D3D12CreateDevice = reinterpret_cast<PFN_HostD3D12CreateDevice>(GetProcAddress(g_host_sl, "D3D12CreateDevice"));
    if (!h_slInit || !h_slGetNewFrameToken || !h_slSetConstants || !h_slSetTag ||
        !h_slGetFeatureFunction || !h_CreateDXGIFactory2 || !h_D3D12CreateDevice) {
        log_line("host: al interposer le faltan exports; sin modo host");
        return false;
    }
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = g_cfg.sllog ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathsToPlugins = g_host_sdk_dirs;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = g_host_log_dir;
    pref.logMessageCallback = &host_sl_log;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking;
    pref.featuresToLoad = kHostFeatures;
    pref.numFeaturesToLoad = 3;
    // Sin projectId, sl.common (build PRODUCTION) rechaza el app id temporal
    // 100721531 y apaga NGX: "Please provide correct application id". Con
    // engineVersion + projectId usa NVSDK_NGX_D3D12_Init_with_ProjectID.
    pref.applicationId = 0;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0";
    pref.projectId = "a0b1c2d3-e4f5-4a6b-8c7d-9e0f1a2b3c4d";  // NGX exige un GUID: solo hex y guiones
    pref.renderAPI = sl::RenderAPI::eD3D12;
    const sl::Result r = h_slInit(pref, sl::kSDKVersion);
    log_num("host: slInit -> ", (unsigned)r);
    if (r != sl::Result::eOk) { g_host_sl = nullptr; return false; }
    return true;
}

// Los exports del interposer que devuelven proxies. Como esos exports llaman
// a los de sistema -- que tenemos enganchados -- la reentrada va al original.
static bool host_take(void) {
    if (!g_host_on) return false;
    if (InterlockedCompareExchange(&g_host_reentry, 1, 0) != 0) return false;
    if (!host_init()) { g_host_reentry = 0; return false; }
    return true;
}
static void host_release(void) { g_host_reentry = 0; }

// El device se declara al crear el swapchain, no al crear el device: en SL 2.x
// el host tiene que llamar slSetD3DDevice explicitamente, y Metro crea un
// device de sondeo que destruye en seguida (sl.log: "Destroyed D3D12Device
// proxy ... ref count 0"). Declarar ese lo dejaria colgado; el que manda es el
// dueno de la cola con la que se crea el swapchain.
static bool g_host_device_set = false;
static void host_set_device(IUnknown *queue) {
    if (!g_host_on || g_host_device_set || queue == nullptr || h_slSetD3DDevice == nullptr) return;
    ID3D12CommandQueue *q = nullptr;
    if (FAILED(queue->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&q))) || q == nullptr) {
        log_line("host: el swapchain no se crea con una cola de D3D12; no hay device que declarar");
        return;
    }
    ID3D12Device *dev = nullptr;
    const HRESULT hr = q->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&dev));
    q->Release();
    if (FAILED(hr) || dev == nullptr) {
        log_num("host: GetDevice de la cola fallo ", (unsigned)hr);
        return;
    }
    const sl::Result r = h_slSetD3DDevice(dev);
    dev->Release();
    g_host_device_set = (r == sl::Result::eOk);
    log_num("host: slSetD3DDevice -> ", (unsigned)r);
}

// ---- los parametros de NGX -------------------------------------------------
static bool g_ngx_order_msvc = true;     // se comprueba en el primer frame
static bool g_ngx_order_checked = false;

typedef unsigned (*PFN_NgxGetRes)(void *self, const char *name, ID3D12Resource **out);
typedef unsigned (*PFN_NgxGetF)(void *self, const char *name, float *out);
typedef unsigned (*PFN_NgxGetUI)(void *self, const char *name, unsigned *out);
static void *ngx_slot(const void *params, int i) { return (*reinterpret_cast<void *const *const *>(params))[i]; }
static bool ngx_get_res(const void *p, const char *name, ID3D12Resource **out) {
    *out = nullptr;
    const int slot = g_ngx_order_msvc ? 9 : 14;
    return reinterpret_cast<PFN_NgxGetRes>(ngx_slot(p, slot))(const_cast<void *>(p), name, out) == 1 && *out != nullptr;
}
static bool ngx_get_f(const void *p, const char *name, float *out) {
    const int slot = g_ngx_order_msvc ? 14 : 9;
    return reinterpret_cast<PFN_NgxGetF>(ngx_slot(p, slot))(const_cast<void *>(p), name, out) == 1;
}
static bool ngx_get_ui(const void *p, const char *name, unsigned *out) {
    const int slot = g_ngx_order_msvc ? 12 : 11;
    return reinterpret_cast<PFN_NgxGetUI>(ngx_slot(p, slot))(const_cast<void *>(p), name, out) == 1;
}
static void ngx_check_order(const void *p) {
    if (g_ngx_order_checked) return;
    g_ngx_order_checked = true;
    unsigned w = 0;
    g_ngx_order_msvc = true;
    const bool ok_msvc = ngx_get_ui(p, "Width", &w) && w >= 64 && w <= 16384;
    const unsigned w_msvc = w;
    g_ngx_order_msvc = false;
    w = 0;
    const bool ok_decl = ngx_get_ui(p, "Width", &w) && w >= 64 && w <= 16384;
    g_ngx_order_msvc = ok_msvc || !ok_decl;
    log_num("host: NGX vtable en orden MSVC (1 = si) ", (unsigned)(g_ngx_order_msvc ? 1 : 0));
    log_num("  Width por MSVC ", w_msvc);
    log_num("  Width por declaracion ", w);
}

// ---- por frame, desde el hook de NGX ---------------------------------------
typedef unsigned (*PFN_NgxEvaluate)(ID3D12GraphicsCommandList *, const void *, const void *, void *);
static PFN_NgxEvaluate g_orig_ngx_evaluate = nullptr;
static bool g_host_options_sent = false;
static LONG g_host_frames = 0;

static void host_pcl(sl::PCLMarker m) {
    if (h_slPCLSetMarker != nullptr && g_host_tok != nullptr) h_slPCLSetMarker(m, *g_host_tok);
}

static unsigned hk_ngx_evaluate(ID3D12GraphicsCommandList *cl, const void *handle, const void *params, void *cb) {
    const unsigned r = g_orig_ngx_evaluate(cl, handle, params, cb);
    // Que features pasan por aca: el juego (DLSS) y, si genera, el plugin
    // (DLSS-G en el hilo nv.sl.dlss_g.thread.dlssg). Por handle, con hilo y
    // resultado, dos veces. Es lo que mostro que el plugin SI evaluaba mientras
    // el multiplicador daba 1.00: el freno estaba en nuestra politica (mode 0).
    {
        static const void *hs[4] = { 0, 0, 0, 0 };
        static LONG hc[4] = { 0, 0, 0, 0 };
        static unsigned hr_[4] = { 0, 0, 0, 0 };
        static DWORD ht[4] = { 0, 0, 0, 0 };
        static LONG calls = 0;
        for (int i = 0; i < 4; ++i) {
            if (hs[i] == nullptr) hs[i] = handle;
            if (hs[i] == handle) { hc[i]++; hr_[i] = r; ht[i] = GetCurrentThreadId(); break; }
        }
        if (++calls == 300 || calls == 3000) {
            log_num("host: EvaluateFeature, llamadas ", (unsigned)calls);
            for (int i = 0; i < 4 && hs[i] != nullptr; ++i) {
                log_num("  feature (ptr bajo) ", (unsigned)(reinterpret_cast<size_t>(hs[i]) & 0xffffffffu));
                log_num("    llamadas ", (unsigned)hc[i]);
                log_num("    ultimo resultado ", hr_[i]);
                log_num("    hilo ", (unsigned)ht[i]);
                if (ht[i] == GetCurrentThreadId()) {
                    // El nombre del hilo, si lo tiene (los del plugin se llaman nv.sl.dlss_g.thread.*).
                    typedef HRESULT(WINAPI *PFN_GTD)(HANDLE, PWSTR *);
                    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
                    PFN_GTD gtd = k32 ? reinterpret_cast<PFN_GTD>(GetProcAddress(k32, "GetThreadDescription")) : nullptr;
                    PWSTR name = nullptr;
                    if (gtd != nullptr && SUCCEEDED(gtd(GetCurrentThread(), &name)) && name != nullptr) {
                        char a[64]; int c = 0;
                        for (; name[c] != 0 && c < 63; ++c) a[c] = (char)(name[c] < 128 ? name[c] : '?');
                        a[c] = 0;
                        log_line(a[0] ? a : "    (hilo sin nombre)");
                        LocalFree(name);
                    }
                }
            }
            if (h_slDLSSGGetState != nullptr) {
                sl::DLSSGState st{};
                const sl::ViewportHandle vp0(0u);
                if (h_slDLSSGGetState(vp0, st, nullptr) == sl::Result::eOk) {
                    log_num("  DLSSG status ", (unsigned)st.status);
                    log_num("  numFramesActuallyPresented ", (unsigned)st.numFramesActuallyPresented);
                    log_num("  minWidthOrHeight ", (unsigned)st.minWidthOrHeight);
                    log_num("  estimatedVRAMUsageInBytes (MB) ", (unsigned)(st.estimatedVRAMUsageInBytes >> 20));
                }
            }
        }
    }
    {
        static bool said = false;
        if (!said) { said = true; log_num("host: primer EvaluateFeature del juego; devolvio ", r); }
    }
    if (g_host_sl == nullptr || params == nullptr) return r;
    ngx_check_order(params);
    ID3D12Resource *depth = nullptr, *mvec = nullptr, *output = nullptr;
    ngx_get_res(params, "Depth", &depth);
    ngx_get_res(params, "MotionVectors", &mvec);
    ngx_get_res(params, "Output", &output);
    float jx = 0, jy = 0, sx = 1, sy = 1;
    ngx_get_f(params, "Jitter.Offset.X", &jx);
    ngx_get_f(params, "Jitter.Offset.Y", &jy);
    ngx_get_f(params, "MV.Scale.X", &sx);
    ngx_get_f(params, "MV.Scale.Y", &sy);
    unsigned w = 0, h = 0, ow = 0, oh = 0, flags = 0;
    ngx_get_ui(params, "Width", &w);
    ngx_get_ui(params, "Height", &h);
    ngx_get_ui(params, "OutWidth", &ow);
    ngx_get_ui(params, "OutHeight", &oh);
    ngx_get_ui(params, "DLSS.Feature.Create.Flags", &flags);
    if (ow == 0 && output != nullptr) {
        const D3D12_RESOURCE_DESC d = output->GetDesc();
        ow = (unsigned)d.Width; oh = d.Height;
    }
    const LONG n = InterlockedIncrement(&g_host_frames);
    if (n == 1 || n == 300) {
        log_num("host: NGX evaluate, frame ", (unsigned)n);
        log_num("  depth (1 = hay) ", (unsigned)(depth ? 1 : 0));
        log_num("  mvec (1 = hay) ", (unsigned)(mvec ? 1 : 0));
        log_num("  output (1 = hay) ", (unsigned)(output ? 1 : 0));
        log_num("  render w ", w); log_num("  render h ", h);
        log_num("  out w ", ow); log_num("  out h ", oh);
        log_num("  jitter x1000 (+32768 si negativo) ", (unsigned)(jx < 0 ? 32768 + (unsigned)(-jx * 1000) : (unsigned)(jx * 1000)));
        log_num("  mv scale x1000 ", (unsigned)(sx * 1000));
        log_num("  create flags ", flags);
        // Formato y tamano de cada buffer, y el del backbuffer: el 11/09 el
        // blur en movimiento era la salida de DLSS en HDR lineal (IsHDR) contra
        // un backbuffer RGBA8, y nuestro log no lo decia; lo dijo sl.log.
        const ID3D12Resource *bufs[3] = { depth, mvec, output };
        const char *names[3] = { "  depth: formato ", "  mvec: formato ", "  salida: formato " };
        for (int b = 0; b < 3; ++b) {
            if (bufs[b] == nullptr) continue;
            const D3D12_RESOURCE_DESC d = const_cast<ID3D12Resource *>(bufs[b])->GetDesc();
            log_num(names[b], (unsigned)d.Format);
            log_num("    ancho ", (unsigned)d.Width);
            log_num("    alto ", d.Height);
        }
        {
            IDXGISwapChain *chain = live_swapchain();
            DXGI_SWAP_CHAIN_DESC sd;
            if (chain != nullptr && SUCCEEDED(chain->GetDesc(&sd)))
                log_num("  backbuffer: formato ", (unsigned)sd.BufferDesc.Format);
            else
                log_line("  backbuffer: sin swapchain adoptado todavia");
        }
    }
    if (depth == nullptr || mvec == nullptr || output == nullptr) return r;

    sl::FrameToken *tok = nullptr;
    if (h_slGetNewFrameToken(tok, nullptr) != sl::Result::eOk || tok == nullptr) return r;
    g_host_tok = tok;
    if (h_slPCLSetMarker == nullptr) {
        void *fn = nullptr;
        if (h_slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", fn) == sl::Result::eOk)
            h_slPCLSetMarker = reinterpret_cast<PFun_slPCLSetMarker *>(fn);
        log_num("host: slPCLSetMarker resuelto (1 = si) ", (unsigned)(h_slPCLSetMarker ? 1 : 0));
    }
    // Reflex: DLSS-G se niega sin el ("eDLSSGStatusFailReflexNotDetectedAtRuntime
    // - sl.reflex must be enabled and active", Metro 2026-09-11). Modo low
    // latency una vez y un slReflexSleep por frame, antes de la simulacion.
    if (h_slReflexSetOptions == nullptr) {
        void *fn = nullptr;
        if (h_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", fn) == sl::Result::eOk)
            h_slReflexSetOptions = reinterpret_cast<PFun_slReflexSetOptions *>(fn);
        fn = nullptr;
        if (h_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", fn) == sl::Result::eOk)
            h_slReflexSleep = reinterpret_cast<PFun_slReflexSleep *>(fn);
        log_num("host: slReflexSetOptions resuelto (1 = si) ", (unsigned)(h_slReflexSetOptions ? 1 : 0));
        if (h_slReflexSetOptions != nullptr) {
            sl::ReflexOptions ro{};
            ro.mode = sl::ReflexMode::eLowLatency;
            log_num("host: slReflexSetOptions(eLowLatency) -> ", (unsigned)h_slReflexSetOptions(ro));
        }
    }
    if (h_slReflexSleep != nullptr) h_slReflexSleep(*tok);
    host_pcl(sl::PCLMarker::eSimulationStart);
    host_pcl(sl::PCLMarker::eSimulationEnd);
    host_pcl(sl::PCLMarker::eRenderSubmitStart);

    sl::Constants c{};
    // Matrices identidad: el juego no las expone. Camara estatica para DLSS-G.
    for (int i = 0; i < 4; ++i) {
        c.cameraViewToClip.row[i] = sl::float4(i == 0, i == 1, i == 2, i == 3);
        c.clipToCameraView.row[i] = c.cameraViewToClip.row[i];
        c.clipToPrevClip.row[i] = c.cameraViewToClip.row[i];
        c.prevClipToClip.row[i] = c.cameraViewToClip.row[i];
        c.clipToLensClip.row[i] = c.cameraViewToClip.row[i];
    }
    c.jitterOffset = sl::float2(jx, jy);
    // NGX: mvec * MV.Scale = pixeles de render. SL quiere el factor que los
    // lleva a [-1,1]: pixeles / dimensiones.
    c.mvecScale = sl::float2(w > 0 ? sx / (float)w : 1.0f, h > 0 ? sy / (float)h : 1.0f);
    c.cameraPinholeOffset = sl::float2(0, 0);
    c.cameraPos = sl::float3(0, 0, 0);
    c.cameraUp = sl::float3(0, 1, 0);
    c.cameraRight = sl::float3(1, 0, 0);
    c.cameraFwd = sl::float3(0, 0, 1);
    c.cameraNear = 0.1f;
    c.cameraFar = 10000.0f;
    c.cameraFOV = 1.2f;
    c.cameraAspectRatio = h > 0 ? (float)w / (float)h : 1.777f;
    c.depthInverted = (flags & 8) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = (flags & 4) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    const sl::ViewportHandle vp(0u);
    const sl::Result rc = h_slSetConstants(c, *tok, vp);

    // Los estados: las entradas de DLSS estan como SRV (0x40), la salida como
    // UAV (0x8). Con eDisableCLStateTracking SL confia en esto.
    sl::Resource rDepth(sl::ResourceType::eTex2d, depth, 0x40);
    sl::Resource rMvec(sl::ResourceType::eTex2d, mvec, 0x40);
    sl::Resource rHud(sl::ResourceType::eTex2d, output, 0x8);
    sl::Extent eIn{ 0, 0, w, h };
    sl::Extent eOut{ 0, 0, ow, oh };
    sl::ResourceTag tags[3] = {
        sl::ResourceTag(&rDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &eIn),
        sl::ResourceTag(&rMvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &eIn),
        // Sin extent: con el del output el plugin avisa "Invalid backbuffer
        // resource extent, IF optionally specified by the client".
        sl::ResourceTag(&rHud, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, nullptr),
    };
    // La salida de DLSS en Metro es HDR lineal antes del tonemap y de los
    // post-efectos (flags de creacion: IsHDR), y el backbuffer es SDR con
    // todo aplicado. DLSS-G resta backbuffer - sinHUD para sacar la UI y la
    // pega en cada frame generado; con espacios de color distintos esa resta
    // es la escena entera mal restada: "todo blurry en movimiento" a 3x/4x.
    // Sin el tag el plugin usa el backbuffer completo (la UI se interpola) y
    // la imagen se ve bien (juzgado a ojo, Metro 2026-09-11, con 3.00 y 4.00
    // sostenidos). Por eso el defecto es sin tag; `hosthudless 1` lo vuelve
    // a poner para experimentar.
    const sl::Result rt = h_slSetTag(vp, tags, g_host_hudless ? 3 : 2, cl);
    host_pcl(sl::PCLMarker::eRenderSubmitEnd);
    if (n == 1 || n == 300) {
        log_num("  slSetConstants -> ", (unsigned)rc);
        log_num("  slSetTag -> ", (unsigned)rt);
    }

    if (!g_host_options_sent && n >= 3) {
        void *fn = nullptr;
        if (h_slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fn) == sl::Result::eOk)
            h_slDLSSGSetOptions = reinterpret_cast<PFun_slDLSSGSetOptions *>(fn);
        fn = nullptr;
        if (h_slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fn) == sl::Result::eOk)
            h_slDLSSGGetState = reinterpret_cast<PFun_slDLSSGGetState *>(fn);
        log_num("host: slDLSSGSetOptions resuelto (1 = si) ", (unsigned)(h_slDLSSGSetOptions ? 1 : 0));
        if (h_slDLSSGSetOptions != nullptr) {
            sl::DLSSGOptions o{};
            o.mode = sl::DLSSGMode::eOn;
            o.numFramesToGenerate = 1;
            o.mvecDepthWidth = w; o.mvecDepthHeight = h;
            o.colorWidth = ow; o.colorHeight = oh;
            const sl::Result ro = h_slDLSSGSetOptions(vp, o);
            log_num("host: slDLSSGSetOptions(eOn, 1) -> ", (unsigned)ro);
        }
        g_host_options_sent = true;
    }
    if (n % 600 == 0 && h_slDLSSGGetState != nullptr) {
        sl::DLSSGState st{};
        if (h_slDLSSGGetState(vp, st, nullptr) == sl::Result::eOk) {
            log_num("host: DLSSG status ", (unsigned)st.status);
            log_num("  numFramesActuallyPresented ", (unsigned)st.numFramesActuallyPresented);
        }
    }
    return r;
}

// Se llama desde el hilo del grabador: engancha NVSDK_NGX_D3D12_EvaluateFeature
// cuando _nvngx.dll ya esta mapeado.
static void host_poll(void) {
    if (!g_host_on || g_orig_ngx_evaluate != nullptr) return;
    HMODULE ngx = GetModuleHandleW(L"_nvngx.dll");
    if (ngx == nullptr) return;
    FARPROC p = GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature");
    if (p == nullptr) { log_line("host: _nvngx.dll sin NVSDK_NGX_D3D12_EvaluateFeature"); g_orig_ngx_evaluate = reinterpret_cast<PFN_NgxEvaluate>(1); return; }
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return;
    if (MH_CreateHook(reinterpret_cast<void *>(p), reinterpret_cast<void *>(&hk_ngx_evaluate),
                      reinterpret_cast<void **>(&g_orig_ngx_evaluate)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void *>(p)) == MH_OK)
        log_line("host: NVSDK_NGX_D3D12_EvaluateFeature enganchado");
    else
        log_line("host: no se pudo enganchar NVSDK_NGX_D3D12_EvaluateFeature");
}

// D3D12CreateDevice del juego -> el del interposer (proxy de device).
static PFN_HostD3D12CreateDevice g_orig_d3d12createdevice = nullptr;
static HRESULT WINAPI hk_d3d12createdevice(IUnknown *adapter, int level, REFIID riid, void **out) {
    if (!host_take()) return g_orig_d3d12createdevice(adapter, level, riid, out);
    const HRESULT hr = h_D3D12CreateDevice(adapter, level, riid, out);
    host_release();
    log_num("host: D3D12CreateDevice por el interposer -> ", (unsigned)hr);
    return hr;
}
static bool g_host_d3d12_armed = false;
static void host_arm_d3d12(void) {
    if (!g_host_on || g_host_d3d12_armed) return;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (d3d12 == nullptr) return;
    g_host_d3d12_armed = true;
    FARPROC p = GetProcAddress(d3d12, "D3D12CreateDevice");
    if (p == nullptr) return;
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return;
    if (MH_CreateHook(reinterpret_cast<void *>(p), reinterpret_cast<void *>(&hk_d3d12createdevice),
                      reinterpret_cast<void **>(&g_orig_d3d12createdevice)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void *>(p)) == MH_OK)
        log_line("host: D3D12CreateDevice enganchado");
}
