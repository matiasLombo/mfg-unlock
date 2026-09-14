// testbed.cpp -- una app D3D12 propia con pasadas deliberadamente mal
// dimensionadas, para probar el pipeline completo sin depender de un juego.
//
//   gpuprobe-testbed.exe [--frames N] [--warp] [--out ruta.jsonl] [--hw]
//
// Corre sin ventana (headless): renderiza a targets offscreen y le avisa al
// colector en cada frame. Asi puede correr en el runner de CI sobre WARP, que
// es el unico software rasterizer que soporta timestamps de verdad.
//
// Los defectos son a proposito y son exactamente los que el analizador tiene
// que encontrar:
//   1. shadow atlas D32 de 4096x4096 en array de 4, caro
//   2. SSAO y SSR a resolucion completa
//   3. un RT que se escribe todos los frames y nunca se lee
//   4. barriers redundantes: ida y vuelta por COMMON, y uno al mismo estado
//   5. un PSO compilado en el hilo de render a mitad del gameplay
//   6. una pasada de compute en otra queue que corre tapada por las sombras
//
// Ademas mete dos casos HOSTILES para el gate de seguridad del ejecutor: un RT
// que es origen de CopyTextureRegion y uno colocado en un heap compartido. El
// ejecutor tiene que RECHAZAR escalar esos dos, y el test de CI lo verifica.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "d3d12/collector.h"

using namespace gp;

namespace {

// --- utilidades minimas -------------------------------------------------

template <class T>
struct Com {
    T *p = nullptr;
    ~Com() { if (p) p->Release(); }
    T **operator&() { return &p; }
    T *operator->() const { return p; }
    operator T *() const { return p; }
    T *get() const { return p; }
};

bool check(HRESULT hr, const char *what) {
    if (SUCCEEDED(hr)) return true;
    std::fprintf(stderr, "testbed: %s fallo (hr=0x%08lx)\n", what,
                 static_cast<unsigned long>(hr));
    return false;
}

// --- shaders ------------------------------------------------------------
//
// Un solo VS de triangulo fullscreen y un PS con un lazo de costo ajustable
// por root constant. No se trata de que la imagen se vea bien: se trata de que
// cada pasada tenga un costo distinto y estable, para que el analizador tenga
// que ordenarlas.

const char kShaders[] = R"(
cbuffer Params : register(b0) {
    uint  iters;
    float seed;
    float pad0, pad1;
};

struct VsOut { float4 pos : SV_Position; };

VsOut VsMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    // Triangulo que cubre la pantalla, desplazado un poco por instancia para
    // que 900 draws no se descarten por early-z trivial.
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VsOut o;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
    o.pos.xy += float2(iid % 7, iid % 5) * 0.0001;
    return o;
}

float4 PsMain(VsOut i) : SV_Target {
    float acc = seed;
    for (uint k = 0; k < iters; ++k)
        acc = mad(acc, 1.0001, sin(acc + i.pos.x * 0.001));
    return float4(acc * 0.0001, i.pos.x * 0.0005, i.pos.y * 0.0005, 1);
}

void PsDepthOnly(VsOut i) {
    // Sin salida de color: una pasada de sombras escribe solo profundidad.
}

RWTexture3D<float4> Vol : register(u0);

[numthreads(8, 8, 1)]
void CsMain(uint3 id : SV_DispatchThreadID) {
    float acc = seed + id.x * 0.01;
    for (uint k = 0; k < iters; ++k)
        acc = mad(acc, 1.0001, sin(acc));
    Vol[id] = float4(acc, id.y * 0.01, id.z * 0.01, 1);
}
)";

ID3DBlob *compile(const char *entry, const char *target) {
    ID3DBlob *code = nullptr, *err = nullptr;
    const HRESULT hr = D3DCompile(kShaders, sizeof(kShaders) - 1, "testbed.hlsl",
                                  nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        std::fprintf(stderr, "testbed: no compilo %s: %s\n", entry,
                     err ? static_cast<const char *>(err->GetBufferPointer()) : "?");
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    return code;
}

// --- la escena ----------------------------------------------------------

struct Target {
    const char     *name;
    ID3D12Resource *res = nullptr;
    DXGI_FORMAT     fmt = DXGI_FORMAT_UNKNOWN;
    UINT            w = 0, h = 0, array = 1;
    bool            depth = false;
    D3D12_CPU_DESCRIPTOR_HANDLE view{};
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct PassDef {
    const char *name;
    int         target;      // indice en targets
    int         depth;       // indice del depth, o -1
    UINT        draws;
    UINT        instances;
    UINT        iters;       // costo del PS
};

constexpr UINT kOutW = 1920;
constexpr UINT kOutH = 1080;

// En CI no hay GPU: corre WARP, que rasteriza por CPU. Un triangulo que cubre
// un target de 4096x4096 ahi cuesta decimas de segundo, asi que el testbed
// puede achicar el AREA que dibuja y el costo del shader sin tocar los
// DESCRIPTORES. Eso importa: el analizador razona sobre el descriptor -- un
// shadow map sigue siendo un D32 de 4096 en array de 4 aunque se dibuje en una
// esquina -- asi que las reglas se ejercitan igual y el CI termina en minutos.
float g_viewport_scale = 1.0f;
float g_cost_scale = 1.0f;

UINT scaled_cost(UINT iters) {
    const UINT v = static_cast<UINT>(static_cast<float>(iters) * g_cost_scale);
    return v ? v : 1;
}

}  // namespace

int main(int argc, char **argv) {
    UINT frames = 900;
    bool force_warp = true;   // en CI no hay GPU: WARP por defecto
    bool want_window = true;  // con swapchain de verdad; cae a headless solo
    const char *out_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = static_cast<UINT>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--warp")) force_warp = true;
        else if (!std::strcmp(argv[i], "--hw")) force_warp = false;
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!std::strcmp(argv[i], "--viewport-scale") && i + 1 < argc)
            g_viewport_scale = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--cost-scale") && i + 1 < argc)
            g_cost_scale = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--headless")) want_window = false;
    }

    Com<IDXGIFactory4> factory;
    if (!check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2"))
        return 1;

    Com<IDXGIAdapter1> adapter;
    if (force_warp) {
        if (!check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter"))
            return 1;
    } else {
        if (!check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1")) return 1;
    }

    Com<ID3D12Device> device;
    if (!check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&device)), "D3D12CreateDevice"))
        return 1;

    // --- colector ---------------------------------------------------------
    CollectorConfig cfg;
    cfg.exe_name = "gpuprobe-testbed.exe";
    cfg.deep_every = 120;
    cfg.query_budget = 64;
    std::wstring wout;
    if (out_path) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, out_path, -1, nullptr, 0);
        wout.resize(static_cast<size_t>(n));
        MultiByteToWideChar(CP_UTF8, 0, out_path, -1, wout.data(), n);
        cfg.session_path = wout.c_str();
    }
    Collector &col = collector();
    col.set_output(kOutW, kOutH);
    {
        IDXGIAdapter3 *a3 = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&a3)))) col.set_adapter(a3);
    }
    if (!col.init(device.get(), cfg)) {
        std::fprintf(stderr, "testbed: el colector no arranco\n");
        return 1;
    }

    // --- queues, allocators, listas --------------------------------------
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Com<ID3D12CommandQueue> gfx_queue;
    if (!check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&gfx_queue)), "queue gfx"))
        return 1;
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    Com<ID3D12CommandQueue> cmp_queue;
    if (!check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&cmp_queue)), "queue compute"))
        return 1;

    // --- swapchain de verdad ---------------------------------------------
    // No es decoracion: el backbuffer es el unico recurso que se escribe todos
    // los frames y NUNCA se transiciona a lectura, asi que sin uno no se puede
    // probar que el analizador no lo confunda con un recurso huerfano. Si no
    // se puede crear (sesion sin escritorio), se sigue headless y el test lo
    // nota solo.
    HWND hwnd = nullptr;
    Com<IDXGISwapChain1> swapchain;
    UINT swap_count = 2;
    if (want_window) {
        WNDCLASSEXA wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "gpuprobe_testbed";
        RegisterClassExA(&wc);
        hwnd = CreateWindowExA(0, wc.lpszClassName, "gpuprobe testbed",
                               WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr,
                               nullptr, wc.hInstance, nullptr);
        if (hwnd) {
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = kOutW;
            sd.Height = kOutH;
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = swap_count;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            if (FAILED(factory->CreateSwapChainForHwnd(gfx_queue.get(), hwnd, &sd,
                                                       nullptr, nullptr,
                                                       &swapchain))) {
                std::printf("testbed: sin swapchain (se sigue headless)\n");
                DestroyWindow(hwnd);
                hwnd = nullptr;
            }
        }
    }
    if (swapchain.get()) std::printf("testbed: swapchain de %u buffers\n", swap_count);

    Com<ID3D12CommandAllocator> gfx_alloc, cmp_alloc;
    if (!check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&gfx_alloc)), "alloc gfx"))
        return 1;
    if (!check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                              IID_PPV_ARGS(&cmp_alloc)), "alloc compute"))
        return 1;

    Com<ID3D12GraphicsCommandList> gfx_list, cmp_list;
    if (!check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         gfx_alloc.get(), nullptr,
                                         IID_PPV_ARGS(&gfx_list)), "list gfx"))
        return 1;
    gfx_list->Close();
    if (!check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                         cmp_alloc.get(), nullptr,
                                         IID_PPV_ARGS(&cmp_list)), "list compute"))
        return 1;
    cmp_list->Close();

    // Una fence por queue. Con una sola fence compartida, si la queue de
    // compute senaliza N+1 antes de que la de graficos senalice N, el valor de
    // la fence va para atras -- que D3D12 no admite y termina en device
    // removed. Es un error facil de escribir y dificil de ver.
    Com<ID3D12Fence> gfx_fence, cmp_fence;
    if (!check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gfx_fence)),
               "fence gfx"))
        return 1;
    if (!check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&cmp_fence)),
               "fence compute"))
        return 1;
    HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fence_value = 0;

    // --- descriptor heaps -------------------------------------------------
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 24;
    Com<ID3D12DescriptorHeap> rtv_heap;
    if (!check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)), "rtv heap"))
        return 1;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 8;
    Com<ID3D12DescriptorHeap> dsv_heap;
    if (!check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap)), "dsv heap"))
        return 1;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 8;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Com<ID3D12DescriptorHeap> uav_heap;
    if (!check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&uav_heap)), "uav heap"))
        return 1;

    const UINT rtv_step = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT dsv_step = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    UINT next_rtv = 0, next_dsv = 0;

    // --- los targets ------------------------------------------------------
    std::vector<Target> targets;
    auto add_target = [&](const char *name, DXGI_FORMAT fmt, UINT w, UINT h,
                          UINT array, bool depth, bool uav = false,
                          ID3D12Heap *placed_heap = nullptr,
                          UINT64 heap_off = 0) -> int {
        Target t;
        t.name = name;
        t.fmt = fmt;
        t.w = w;
        t.h = h;
        t.array = array;
        t.depth = depth;

        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = static_cast<UINT16>(array);
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                         : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (uav) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12_CLEAR_VALUE cv{};
        cv.Format = fmt;
        if (depth) cv.DepthStencil.Depth = 1.0f;

        t.state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                        : D3D12_RESOURCE_STATE_RENDER_TARGET;

        // El colector se entera ANTES de crear (por si el ejecutor quiere
        // cambiar el tamano) y DESPUES (con el recurso ya creado). Es el mismo
        // par de llamadas que hace el hook adentro de un juego.
        const CallsiteId site = capture_callsite();
        (void)col.want_override(rd, D3D12_HEAP_TYPE_DEFAULT, site);

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        HRESULT hr;
        if (placed_heap) {
            hr = device->CreatePlacedResource(placed_heap, heap_off, &rd, t.state,
                                              &cv, IID_PPV_ARGS(&t.res));
        } else {
            hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                 t.state, &cv, IID_PPV_ARGS(&t.res));
        }
        if (!check(hr, name)) return -1;
        col.on_resource(t.res, rd, D3D12_HEAP_TYPE_DEFAULT, placed_heap != nullptr,
                        site);

        if (depth) {
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
            dv.Format = fmt;
            dv.ViewDimension = array > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DARRAY
                                         : D3D12_DSV_DIMENSION_TEXTURE2D;
            if (array > 1) dv.Texture2DArray.ArraySize = array;
            t.view = dsv_heap->GetCPUDescriptorHandleForHeapStart();
            t.view.ptr += static_cast<SIZE_T>(next_dsv++) * dsv_step;
            device->CreateDepthStencilView(t.res, &dv, t.view);
            col.on_dsv(t.view, t.res);
        } else {
            t.view = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            t.view.ptr += static_cast<SIZE_T>(next_rtv++) * rtv_step;
            device->CreateRenderTargetView(t.res, nullptr, t.view);
            col.on_rtv(t.view, t.res);
        }
        targets.push_back(t);
        return static_cast<int>(targets.size()) - 1;
    };

    // Un heap compartido para el caso hostil: dos recursos colocados uno al
    // lado del otro. Escalar el primero pisaria al segundo, y el gate del
    // ejecutor tiene que rechazarlo.
    D3D12_HEAP_DESC shared_heap_desc{};
    shared_heap_desc.SizeInBytes = 64ull * 1024 * 1024;
    shared_heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    shared_heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    shared_heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    Com<ID3D12Heap> shared_heap;
    if (!check(device->CreateHeap(&shared_heap_desc, IID_PPV_ARGS(&shared_heap)),
               "heap compartido"))
        return 1;

    const int T_SHADOW = add_target("shadow_atlas", DXGI_FORMAT_D32_FLOAT, 4096, 4096, 4, true);
    const int T_DEPTH  = add_target("scene_depth", DXGI_FORMAT_D32_FLOAT, kOutW, kOutH, 1, true);
    const int T_ALBEDO = add_target("gbuffer_albedo", DXGI_FORMAT_R8G8B8A8_UNORM, kOutW, kOutH, 1, false);
    const int T_NORMAL = add_target("gbuffer_normal", DXGI_FORMAT_R16G16_FLOAT, kOutW, kOutH, 1, false);
    const int T_SSAO   = add_target("ssao", DXGI_FORMAT_R8_UNORM, kOutW, kOutH, 1, false);
    const int T_SSR    = add_target("ssr", DXGI_FORMAT_R16G16B16A16_FLOAT, kOutW, kOutH, 1, false);
    const int T_HDR    = add_target("hdr_color", DXGI_FORMAT_R16G16B16A16_FLOAT, kOutW, kOutH, 1, false);
    const int T_BLOOM  = add_target("bloom_half", DXGI_FORMAT_R11G11B10_FLOAT, kOutW / 2, kOutH / 2, 1, false);
    const int T_UI     = add_target("ui", DXGI_FORMAT_R8G8B8A8_UNORM, kOutW, kOutH, 1, false);
    const int T_ORPHAN = add_target("orphan_rt", DXGI_FORMAT_R16G16B16A16_FLOAT, kOutW, kOutH, 1, false);
    const int T_COPIED = add_target("copied_rt", DXGI_FORMAT_R8G8B8A8_UNORM, kOutW, kOutH, 1, false);
    const int T_COPYDST= add_target("copy_dst", DXGI_FORMAT_R8G8B8A8_UNORM, kOutW, kOutH, 1, false);
    const int T_PLACED = add_target("placed_rt", DXGI_FORMAT_R8G8B8A8_UNORM, 1024, 1024, 1, false,
                                    false, shared_heap.get(), 0);
    if (T_SHADOW < 0 || T_PLACED < 0 || T_COPYDST < 0) return 1;

    // Los backbuffers: sus RTVs salen del mismo heap. El colector los marca
    // aparte (note_backbuffers) para que no entren como RTs comunes.
    std::vector<ID3D12Resource *> backbuffers;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> back_rtv;
    std::vector<D3D12_RESOURCE_STATES> back_state;
    if (swapchain.get()) {
        for (UINT i = 0; i < swap_count; ++i) {
            ID3D12Resource *buf = nullptr;
            if (FAILED(swapchain->GetBuffer(i, IID_PPV_ARGS(&buf))) || !buf) break;
            D3D12_CPU_DESCRIPTOR_HANDLE h =
                rtv_heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(next_rtv++) * rtv_step;
            device->CreateRenderTargetView(buf, nullptr, h);
            col.on_rtv(h, buf);
            backbuffers.push_back(buf);
            back_rtv.push_back(h);
            back_state.push_back(D3D12_RESOURCE_STATE_PRESENT);
        }
        col.note_backbuffers(swapchain.get());
        // note_backbuffers registra los recursos; on_rtv necesita que ya esten
        // registrados, asi que se vuelve a asociar la vista ahora.
        for (size_t i = 0; i < backbuffers.size(); ++i)
            col.on_rtv(back_rtv[i], backbuffers[i]);
    }

    // La textura 3D de la niebla volumetrica: el unico recurso con UAV, y el
    // unico que se toca desde la queue de compute.
    Com<ID3D12Resource> volume;
    {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        rd.Width = 160;
        rd.Height = 96;
        rd.DepthOrArraySize = 64;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        const CallsiteId site = capture_callsite();
        if (!check(device->CreateCommittedResource(
                       &hp, D3D12_HEAP_FLAG_NONE, &rd,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                       IID_PPV_ARGS(&volume)), "volume"))
            return 1;
        col.on_resource(volume.get(), rd, D3D12_HEAP_TYPE_DEFAULT, false, site);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = rd.Format;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uv.Texture3D.WSize = 64;
        device->CreateUnorderedAccessView(volume.get(), nullptr, &uv,
                                          uav_heap->GetCPUDescriptorHandleForHeapStart());
    }

    // --- root signatures y PSOs -------------------------------------------
    Com<ID3D12RootSignature> rs_gfx, rs_cmp;
    {
        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp.Constants.Num32BitValues = 4;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1;
        rsd.pParameters = &rp;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob *blob = nullptr, *err = nullptr;
        if (!check(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                               &blob, &err), "root gfx"))
            return 1;
        if (err) err->Release();
        if (!check(device->CreateRootSignature(0, blob->GetBufferPointer(),
                                               blob->GetBufferSize(),
                                               IID_PPV_ARGS(&rs_gfx)), "rs gfx"))
            return 1;
        blob->Release();

        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER cp[2]{};
        cp[0] = rp;
        cp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        cp[1].DescriptorTable.NumDescriptorRanges = 1;
        cp[1].DescriptorTable.pDescriptorRanges = &range;
        D3D12_ROOT_SIGNATURE_DESC crsd{};
        crsd.NumParameters = 2;
        crsd.pParameters = cp;
        if (!check(D3D12SerializeRootSignature(&crsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                               &blob, &err), "root compute"))
            return 1;
        if (err) err->Release();
        if (!check(device->CreateRootSignature(0, blob->GetBufferPointer(),
                                               blob->GetBufferSize(),
                                               IID_PPV_ARGS(&rs_cmp)), "rs compute"))
            return 1;
        blob->Release();
    }

    ID3DBlob *vs = compile("VsMain", "vs_5_0");
    ID3DBlob *ps = compile("PsMain", "ps_5_0");
    ID3DBlob *ps_depth = compile("PsDepthOnly", "ps_5_0");
    ID3DBlob *cs = compile("CsMain", "cs_5_0");
    if (!vs || !ps || !ps_depth || !cs) return 1;

    // Un PSO por combinacion de formatos de salida: D3D12 exige que el PSO
    // declare los formatos exactos de los RTs a los que va a escribir.
    auto make_pso = [&](const DXGI_FORMAT *fmts, UINT n, DXGI_FORMAT dsv_fmt,
                        bool depth_only, u64 *compile_ns_out,
                        bool render_thread) -> ID3D12PipelineState * {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs_gfx.get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        if (!depth_only) pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        else             pd.PS = {ps_depth->GetBufferPointer(), ps_depth->GetBufferSize()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
        for (UINT i = 1; i < 8; ++i) pd.BlendState.RenderTarget[i] = pd.BlendState.RenderTarget[0];
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = depth_only ? TRUE : FALSE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = n;
        for (UINT i = 0; i < n; ++i) pd.RTVFormats[i] = fmts[i];
        pd.DSVFormat = dsv_fmt;
        pd.SampleDesc.Count = 1;

        const LARGE_INTEGER t0 = [] { LARGE_INTEGER x; QueryPerformanceCounter(&x); return x; }();
        ID3D12PipelineState *pso = nullptr;
        if (!check(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)), "pso"))
            return nullptr;
        LARGE_INTEGER t1, f;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        const u64 ns = static_cast<u64>((t1.QuadPart - t0.QuadPart) * 1000000000ll /
                                        f.QuadPart);
        if (compile_ns_out) *compile_ns_out = ns;
        col.on_graphics_pso(pd, pso, ns, render_thread);
        return pso;
    };

    const DXGI_FORMAT f_rgba8[1] = {DXGI_FORMAT_R8G8B8A8_UNORM};
    const DXGI_FORMAT f_gbuf[2]  = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16_FLOAT};
    const DXGI_FORMAT f_r8[1]    = {DXGI_FORMAT_R8_UNORM};
    const DXGI_FORMAT f_rgba16[1]= {DXGI_FORMAT_R16G16B16A16_FLOAT};
    const DXGI_FORMAT f_r11[1]   = {DXGI_FORMAT_R11G11B10_FLOAT};

    ID3D12PipelineState *pso_shadow = make_pso(nullptr, 0, DXGI_FORMAT_D32_FLOAT, true, nullptr, false);
    ID3D12PipelineState *pso_gbuf   = make_pso(f_gbuf, 2, DXGI_FORMAT_UNKNOWN, false, nullptr, false);
    ID3D12PipelineState *pso_rgba8  = make_pso(f_rgba8, 1, DXGI_FORMAT_UNKNOWN, false, nullptr, false);
    ID3D12PipelineState *pso_r8     = make_pso(f_r8, 1, DXGI_FORMAT_UNKNOWN, false, nullptr, false);
    ID3D12PipelineState *pso_rgba16 = make_pso(f_rgba16, 1, DXGI_FORMAT_UNKNOWN, false, nullptr, false);
    ID3D12PipelineState *pso_r11    = make_pso(f_r11, 1, DXGI_FORMAT_UNKNOWN, false, nullptr, false);
    if (!pso_shadow || !pso_gbuf || !pso_rgba8 || !pso_r8 || !pso_rgba16 || !pso_r11)
        return 1;

    Com<ID3D12PipelineState> pso_compute;
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
        cd.pRootSignature = rs_cmp.get();
        cd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        if (!check(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso_compute)),
                   "pso compute"))
            return 1;
        col.on_compute_pso(cd, pso_compute.get(), 0, false);
    }

    // --- helpers del frame -------------------------------------------------
    auto transition = [&](ID3D12GraphicsCommandList *list, Target &t,
                          D3D12_RESOURCE_STATES to) {
        if (t.state == to) return;   // el runtime rechaza una transicion nula
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t.res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = t.state;
        b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
        (void)col.cmd_barrier(list, 1, &b, nullptr);
        t.state = to;
    };

    auto set_viewport = [&](ID3D12GraphicsCommandList *list, UINT w, UINT h) {
        if (g_viewport_scale < 1.0f) {
            w = static_cast<UINT>(static_cast<float>(w) * g_viewport_scale);
            h = static_cast<UINT>(static_cast<float>(h) * g_viewport_scale);
            if (w < 8) w = 8;
            if (h < 8) h = 8;
        }
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h),
                          0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
        D3D12_VIEWPORT scaled_vp;
        if (col.cmd_viewports(list, 1, &vp, &scaled_vp)) vp = scaled_vp;
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
    };

    struct Constants { UINT iters; float seed; float pad0, pad1; };

    auto color_pass = [&](ID3D12GraphicsCommandList *list, int target,
                          ID3D12PipelineState *pso, UINT draws, UINT instances,
                          UINT iters, float seed) {
        Target &t = targets[static_cast<size_t>(target)];
        transition(list, t, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->OMSetRenderTargets(1, &t.view, FALSE, nullptr);
        col.cmd_set_render_targets(list, 1, &t.view, FALSE, nullptr);
        set_viewport(list, t.w, t.h);
        list->SetPipelineState(pso);
        col.cmd_set_pso(list, pso);
        const Constants c{scaled_cost(iters), seed, 0.0f, 0.0f};
        list->SetGraphicsRoot32BitConstants(0, 4, &c, 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (UINT d = 0; d < draws; ++d) {
            list->DrawInstanced(3, instances, 0, 0);
            (void)col.cmd_draw(list, 3, instances);
        }
    };

    std::printf("testbed: %s, %u frames a %ux%u (viewport x%.2f, costo x%.2f)\n",
                force_warp ? "WARP" : "hardware", frames, kOutW, kOutH,
                static_cast<double>(g_viewport_scale),
                static_cast<double>(g_cost_scale));

    ID3D12PipelineState *late_pso = nullptr;
    const UINT late_frame = frames > 400 ? 300 : frames / 2;

    for (UINT f = 0; f < frames; ++f) {
        gfx_alloc->Reset();
        gfx_list->Reset(gfx_alloc.get(), nullptr);
        col.cmd_begin(gfx_list.get());
        gfx_list->SetGraphicsRootSignature(rs_gfx.get());

        // 1. sombras: 900 draws sobre un atlas de 4096 en array de 4
        {
            Target &t = targets[static_cast<size_t>(T_SHADOW)];
            transition(gfx_list.get(), t, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            gfx_list->OMSetRenderTargets(0, nullptr, FALSE, &t.view);
            col.cmd_set_render_targets(gfx_list.get(), 0, nullptr, FALSE, &t.view);
            set_viewport(gfx_list.get(), t.w, t.h);
            gfx_list->SetPipelineState(pso_shadow);
            col.cmd_set_pso(gfx_list.get(), pso_shadow);
            const Constants c{scaled_cost(2), 0.5f, 0.0f, 0.0f};
            gfx_list->SetGraphicsRoot32BitConstants(0, 4, &c, 0);
            gfx_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const UINT shadow_draws = g_cost_scale < 1.0f
                ? static_cast<UINT>(300 * g_cost_scale) + 8 : 300;
            for (UINT d = 0; d < shadow_draws; ++d) {
                gfx_list->DrawInstanced(3, 3, 0, 0);
                (void)col.cmd_draw(gfx_list.get(), 3, 3);
            }
            // Se lee en la pasada de iluminacion: transicion legitima.
            transition(gfx_list.get(), t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        // 1b. prepass de profundidad sobre el depth de la escena. Existe para
        //     que la sesion tenga un depthbuffer a resolucion de salida: es lo
        //     que distingue "shadow map" de "depth de la escena" en la
        //     clasificacion por descriptor, y sin uno de los dos esa regla no
        //     estaria ejercitada.
        {
            Target &t = targets[static_cast<size_t>(T_DEPTH)];
            transition(gfx_list.get(), t, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            gfx_list->OMSetRenderTargets(0, nullptr, FALSE, &t.view);
            col.cmd_set_render_targets(gfx_list.get(), 0, nullptr, FALSE, &t.view);
            set_viewport(gfx_list.get(), t.w, t.h);
            gfx_list->SetPipelineState(pso_shadow);
            col.cmd_set_pso(gfx_list.get(), pso_shadow);
            const Constants c{scaled_cost(2), 0.15f, 0.0f, 0.0f};
            gfx_list->SetGraphicsRoot32BitConstants(0, 4, &c, 0);
            gfx_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (UINT d = 0; d < 40; ++d) {
                gfx_list->DrawInstanced(3, 2, 0, 0);
                (void)col.cmd_draw(gfx_list.get(), 3, 2);
            }
            transition(gfx_list.get(), t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        // 2. gbuffer: dos RTs full-res, mucha geometria
        {
            Target &a = targets[static_cast<size_t>(T_ALBEDO)];
            Target &n = targets[static_cast<size_t>(T_NORMAL)];
            transition(gfx_list.get(), a, D3D12_RESOURCE_STATE_RENDER_TARGET);
            transition(gfx_list.get(), n, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const D3D12_CPU_DESCRIPTOR_HANDLE rts[2] = {a.view, n.view};
            gfx_list->OMSetRenderTargets(2, rts, FALSE, nullptr);
            col.cmd_set_render_targets(gfx_list.get(), 2, rts, FALSE, nullptr);
            set_viewport(gfx_list.get(), a.w, a.h);
            gfx_list->SetPipelineState(pso_gbuf);
            col.cmd_set_pso(gfx_list.get(), pso_gbuf);
            const Constants c{scaled_cost(6), 0.25f, 0.0f, 0.0f};
            gfx_list->SetGraphicsRoot32BitConstants(0, 4, &c, 0);
            gfx_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const UINT gbuf_draws = g_cost_scale < 1.0f
                ? static_cast<UINT>(120 * g_cost_scale) + 4 : 120;
            for (UINT d = 0; d < gbuf_draws; ++d) {
                gfx_list->DrawInstanced(3, 2, 0, 0);
                (void)col.cmd_draw(gfx_list.get(), 3, 2);
            }
            // Lectura legitima, y despues la vuelta por COMMON que no hace
            // falta: leer -> COMMON -> escribir, cuando leer -> escribir
            // alcanzaba. Dos barriers donde va uno, todos los frames. Es el
            // patron que el analizador tiene que marcar.
            transition(gfx_list.get(), a, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transition(gfx_list.get(), a, D3D12_RESOURCE_STATE_COMMON);
            transition(gfx_list.get(), a, D3D12_RESOURCE_STATE_RENDER_TARGET);
            transition(gfx_list.get(), n, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        // 3. SSAO a resolucion completa (deberia ser media)
        color_pass(gfx_list.get(), T_SSAO, pso_r8, 1, 1, 900, 0.3f);
        transition(gfx_list.get(), targets[static_cast<size_t>(T_SSAO)],
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // 4. SSR a resolucion completa (deberia ser media)
        color_pass(gfx_list.get(), T_SSR, pso_rgba16, 1, 1, 1400, 0.7f);
        transition(gfx_list.get(), targets[static_cast<size_t>(T_SSR)],
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // 5. iluminacion: full-res y legitima (no es candidata)
        color_pass(gfx_list.get(), T_HDR, pso_rgba16, 4, 1, 500, 0.1f);
        transition(gfx_list.get(), targets[static_cast<size_t>(T_HDR)],
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // 6. bloom a media resolucion (asi tiene que ser: control positivo)
        color_pass(gfx_list.get(), T_BLOOM, pso_r11, 3, 1, 200, 0.9f);
        transition(gfx_list.get(), targets[static_cast<size_t>(T_BLOOM)],
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // 7. tonemap + UI sobre el mismo RT final
        color_pass(gfx_list.get(), T_UI, pso_rgba8, 1, 1, 150, 0.4f);
        color_pass(gfx_list.get(), T_UI, pso_rgba8, 20, 1, 2, 0.6f);

        // 8. el huerfano: se escribe todos los frames y nadie lo lee nunca
        color_pass(gfx_list.get(), T_ORPHAN, pso_rgba16, 1, 1, 60, 0.2f);

        // 9. hostil A: un RT que ademas se copia. El gate del ejecutor tiene
        //    que rechazar escalarlo -- CopyTextureRegion lleva extents fijos.
        color_pass(gfx_list.get(), T_COPIED, pso_rgba8, 1, 1, 30, 0.8f);
        {
            Target &src = targets[static_cast<size_t>(T_COPIED)];
            Target &dst = targets[static_cast<size_t>(T_COPYDST)];
            transition(gfx_list.get(), src, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(gfx_list.get(), dst, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION s{}, d{};
            s.pResource = src.res;
            s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            d.pResource = dst.res;
            d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            gfx_list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            // El colector se entera de la copia: es el hecho que despues hace
            // que el gate del ejecutor RECHACE escalar este recurso.
            col.cmd_copy(gfx_list.get(), dst.res, src.res);
        }

        // 10. hostil B: un RT colocado en un heap compartido. Escalarlo
        //     cambiaria el offset del que viene atras.
        color_pass(gfx_list.get(), T_PLACED, pso_rgba8, 1, 1, 30, 0.5f);

        // 11. el PSO que compila en pleno gameplay, en el hilo de render
        if (f == late_frame && !late_pso) {
            u64 ns = 0;
            late_pso = make_pso(f_rgba16, 1, DXGI_FORMAT_UNKNOWN, false, &ns, true);
            std::printf("testbed: PSO compilado en el frame %u (%.1f ms)\n", f,
                        static_cast<double>(ns) / 1e6);
        }

        // 12. la pasada final: al backbuffer. Es la que hace que la sesion
        //     tenga un recurso presentado, que se escribe todos los frames y
        //     que nadie lee con un shader.
        UINT back_index = 0;
        if (swapchain.get() && !backbuffers.empty()) {
            IDXGISwapChain3 *sc3 = nullptr;
            if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
                back_index = sc3->GetCurrentBackBufferIndex();
                sc3->Release();
            }
            if (back_index < backbuffers.size()) {
                D3D12_RESOURCE_BARRIER bb{};
                bb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bb.Transition.pResource = backbuffers[back_index];
                bb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bb.Transition.StateBefore = back_state[back_index];
                bb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                if (bb.Transition.StateBefore != bb.Transition.StateAfter) {
                    gfx_list->ResourceBarrier(1, &bb);
                    (void)col.cmd_barrier(gfx_list.get(), 1, &bb, nullptr);
                    back_state[back_index] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                }
                gfx_list->OMSetRenderTargets(1, &back_rtv[back_index], FALSE, nullptr);
                col.cmd_set_render_targets(gfx_list.get(), 1, &back_rtv[back_index],
                                           FALSE, nullptr);
                set_viewport(gfx_list.get(), kOutW, kOutH);
                gfx_list->SetPipelineState(pso_rgba8);
                col.cmd_set_pso(gfx_list.get(), pso_rgba8);
                const Constants c{scaled_cost(40), 0.95f, 0.0f, 0.0f};
                gfx_list->SetGraphicsRoot32BitConstants(0, 4, &c, 0);
                gfx_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                gfx_list->DrawInstanced(3, 1, 0, 0);
                (void)col.cmd_draw(gfx_list.get(), 3, 1);

                std::swap(bb.Transition.StateBefore, bb.Transition.StateAfter);
                gfx_list->ResourceBarrier(1, &bb);
                (void)col.cmd_barrier(gfx_list.get(), 1, &bb, nullptr);
                back_state[back_index] = D3D12_RESOURCE_STATE_PRESENT;
            }
        }

        col.cmd_close(gfx_list.get());
        gfx_list->Close();
        ID3D12CommandList *lists[] = {gfx_list.get()};
        gfx_queue->ExecuteCommandLists(1, lists);
        col.on_execute(gfx_queue.get(), 1, lists);

        // --- la queue de compute: la niebla volumetrica, en paralelo -------
        cmp_alloc->Reset();
        cmp_list->Reset(cmp_alloc.get(), nullptr);
        col.cmd_begin(cmp_list.get());
        cmp_list->SetComputeRootSignature(rs_cmp.get());
        ID3D12DescriptorHeap *heaps[] = {uav_heap.get()};
        cmp_list->SetDescriptorHeaps(1, heaps);
        cmp_list->SetComputeRootDescriptorTable(
            1, uav_heap->GetGPUDescriptorHandleForHeapStart());
        cmp_list->SetPipelineState(pso_compute.get());
        col.cmd_set_pso(cmp_list.get(), pso_compute.get());
        const Constants cc{scaled_cost(40), 0.33f, 0.0f, 0.0f};
        cmp_list->SetComputeRoot32BitConstants(0, 4, &cc, 0);
        cmp_list->Dispatch(20, 12, 64);
        (void)col.cmd_dispatch(cmp_list.get(), 20, 12, 64);
        col.cmd_close(cmp_list.get());
        cmp_list->Close();
        ID3D12CommandList *clists[] = {cmp_list.get()};
        cmp_queue->ExecuteCommandLists(1, clists);
        col.on_execute(cmp_queue.get(), 1, clists);

        // Esperar a las dos queues. El testbed no busca fps: busca que cada
        // frame sea comparable con el anterior.
        ++fence_value;
        gfx_queue->Signal(gfx_fence.get(), fence_value);
        cmp_queue->Signal(cmp_fence.get(), fence_value);
        for (ID3D12Fence *fe : {gfx_fence.get(), cmp_fence.get()}) {
            if (fe->GetCompletedValue() >= fence_value) continue;
            if (FAILED(fe->SetEventOnCompletion(fence_value, fence_event))) {
                std::fprintf(stderr, "testbed: SetEventOnCompletion fallo en el "
                                     "frame %u\n", f);
                return 1;
            }
            if (WaitForSingleObject(fence_event, 30000) != WAIT_OBJECT_0) {
                std::fprintf(stderr, "testbed: la GPU no termino el frame %u en "
                                     "30 s (device removed 0x%08lx)\n", f,
                             static_cast<unsigned long>(device->GetDeviceRemovedReason()));
                return 1;
            }
        }

        if (swapchain.get()) swapchain->Present(0, 0);
        col.on_present(swapchain.get());

        if ((f % 100) == 0) {
            std::printf("  frame %u\n", f);
            std::fflush(stdout);
        }
    }

    std::printf("testbed: %u frames corridos, cerrando\n", frames);
    std::fflush(stdout);

    // Cerrar prolijo: el escritor tiene que drenar lo que quede en los rings.
    col.shutdown();
    std::printf("testbed: colector cerrado\n");
    std::fflush(stdout);
    for (ID3D12Resource *b : backbuffers) if (b) b->Release();
    if (hwnd) DestroyWindow(hwnd);
    for (Target &t : targets) if (t.res) t.res->Release();
    if (late_pso) late_pso->Release();
    pso_shadow->Release();
    pso_gbuf->Release();
    pso_rgba8->Release();
    pso_r8->Release();
    pso_rgba16->Release();
    pso_r11->Release();
    vs->Release();
    ps->Release();
    ps_depth->Release();
    cs->Release();
    CloseHandle(fence_event);
    std::printf("testbed: listo\n");
    return 0;

}
