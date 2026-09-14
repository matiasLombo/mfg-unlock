// hooks.cpp -- enganche por vtable de D3D12 y DXGI.
//
// ENTRA: la fabrica DXGI que el proxy devuelve, y de ahi todo lo demas.
// SALE: llamadas al colector y al ejecutor; el juego ve el comportamiento
//   original salvo donde una accion este activa.
// DEPENDE DE: gate.h (el interruptor general), collector.h, vtbl.h.
//
// Por que por vtable y no por trampolin sobre la funcion exportada: los
// metodos COM no son exportados, y lo unico estable que hay es la posicion en
// la vtable -- que sale de vtbl.h, generado del header, y no de numeros
// escritos a mano.
//
// UNA TABLA DE VTABLES, NO "LA" VTABLE. Esta es la leccion cara de este
// repositorio (ver docs/present-por-vtable.diff): un juego puede tener mas de
// una vtable viva para la misma interfaz -- recrea el swapchain al cambiar de
// modo de pantalla, o hay otra capa enganchada antes que nosotros -- y guardar
// "el original" en una sola variable global hace que la segunda vtable llame
// al original de la primera. Eso es un crash, y uno que solo aparece cuando el
// usuario hace alt-tab. Aca cada metodo enganchado guarda su original POR
// vtable.
#define WIN32_LEAN_AND_MEAN
#include "hooks.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "collector.h"
#include "executor.h"
#include "gate.h"
#include "overlay.h"
#include "vtbl.h"

namespace gp {

namespace {

// Cada metodo que enganchamos. Un slot por metodo, y adentro hasta ocho
// vtables distintas con su original.
enum HookId {
    H_Factory_CreateSwapChain = 0,
    H_Factory_CreateSwapChainForHwnd,
    H_SwapChain_Present,
    H_SwapChain_Present1,
    H_SwapChain_ResizeBuffers,
    H_Device_CreateCommittedResource,
    H_Device_CreatePlacedResource,
    H_Device_CreateReservedResource,
    H_Device_CreateGraphicsPipelineState,
    H_Device_CreateComputePipelineState,
    H_Device_CreateRenderTargetView,
    H_Device_CreateDepthStencilView,
    H_Queue_ExecuteCommandLists,
    H_List_Close,
    H_List_Reset,
    H_List_DrawInstanced,
    H_List_DrawIndexedInstanced,
    H_List_Dispatch,
    H_List_CopyResource,
    H_List_CopyTextureRegion,
    H_List_RSSetViewports,
    H_List_SetPipelineState,
    H_List_ResourceBarrier,
    H_List_OMSetRenderTargets,
    H_Count,
};

constexpr unsigned kMaxVtables = 8;

struct Slot {
    int index = -1;
    std::atomic<unsigned> n{0};
    void **vtable[kMaxVtables] = {};
    void  *original[kMaxVtables] = {};
};

Slot       g_slots[H_Count];
std::mutex g_hook_mu;
HookStats  g_stats;

// El device y la queue con los que dibuja el overlay. Se llenan al armar; si
// no estan, no hay overlay y no pasa nada mas.
std::atomic<ID3D12Device *>       g_device{nullptr};
std::atomic<ID3D12CommandQueue *> g_queue{nullptr};

void **vtable_of(void *obj) { return *reinterpret_cast<void ***>(obj); }

// Devuelve el original de ESTE objeto para este metodo. Si no lo encontramos
// -- no deberia pasar nunca -- devuelve nullptr y quien llama tiene que
// apagarse en vez de saltar a cualquier lado.
void *original_for(HookId id, void *obj) {
    Slot &s = g_slots[id];
    void **vt = vtable_of(obj);
    const unsigned n = s.n.load(std::memory_order_acquire);
    for (unsigned i = 0; i < n && i < kMaxVtables; ++i)
        if (s.vtable[i] == vt) return s.original[i];
    return nullptr;
}

// Engancha un metodo. Idempotente: enganchar dos veces la misma vtable no
// hace nada (y no encadena el hook consigo mismo, que es como se cuelga un
// proceso en el primer frame).
bool install(HookId id, void *obj, int index, void *detour) {
    if (!obj) return false;
    std::lock_guard<std::mutex> lk(g_hook_mu);
    Slot &s = g_slots[id];
    s.index = index;
    void **vt = vtable_of(obj);
    const unsigned n = s.n.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < n; ++i)
        if (s.vtable[i] == vt) return true;   // ya enganchada
    if (n >= kMaxVtables) {
        gate_log("mas de %u vtables para el hook %d: no engancho la nueva",
                 kMaxVtables, static_cast<int>(id));
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(&vt[index], sizeof(void *), PAGE_READWRITE, &old))
        return false;
    s.vtable[n] = vt;
    s.original[n] = vt[index];
    vt[index] = detour;
    VirtualProtect(&vt[index], sizeof(void *), old, &old);
    s.n.store(n + 1, std::memory_order_release);
    return true;
}

template <class Fn>
Fn orig(HookId id, void *obj) {
    return reinterpret_cast<Fn>(original_for(id, obj));
}

// --- swapchain ---------------------------------------------------------

using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain1 *, UINT, UINT,
                                                const DXGI_PRESENT_PARAMETERS *);
using ResizeFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT, UINT,
                                              DXGI_FORMAT, UINT);

void arm_from_swapchain(IDXGISwapChain *sc);

// Lo que hay que hacer ANTES del Present real: dibujar el overlay. Despues ya
// es tarde -- el frame se fue.
void before_present(IDXGISwapChain *sc) {
    if (gate_phase() == Phase::Passthrough) arm_from_swapchain(sc);
    if (gate_phase() != Phase::Verified && gate_phase() != Phase::Armed) return;

    // F10: prender y apagar el overlay. Con el flanco, no con el nivel, o
    // parpadearia sesenta veces por segundo.
    static bool held = false;
    const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (down && !held) overlay_toggle();
    held = down;

    ID3D12Device *dev = g_device.load(std::memory_order_acquire);
    ID3D12CommandQueue *q = g_queue.load(std::memory_order_acquire);
    if (dev && q) overlay_draw(dev, q, sc);
}

void note_present(IDXGISwapChain *sc) {
    // La resolucion de salida sale del swapchain y no de la ventana: con DRS,
    // upscalers y modos raros, la ventana miente y el backbuffer no.
    DXGI_SWAP_CHAIN_DESC d{};
    if (SUCCEEDED(sc->GetDesc(&d)))
        collector().set_output(d.BufferDesc.Width, d.BufferDesc.Height);
    collector().on_present(sc);
    gate_verify();
}

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain *sc, UINT interval, UINT flags) {
    PresentFn real = orig<PresentFn>(H_SwapChain_Present, sc);
    if (!real) { gate_degrade("Present sin original para su vtable"); return E_FAIL; }
    if (gate_open()) {
        Reentry guard;
        if (guard.ok()) before_present(sc);
    }
    const HRESULT hr = real(sc, interval, flags);
    if (gate_open()) {
        Reentry guard;
        if (guard.ok()) note_present(sc);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1 *sc, UINT interval, UINT flags,
                                      const DXGI_PRESENT_PARAMETERS *params) {
    Present1Fn real = orig<Present1Fn>(H_SwapChain_Present1, sc);
    if (!real) { gate_degrade("Present1 sin original"); return E_FAIL; }
    if (gate_open()) {
        Reentry guard;
        if (guard.ok()) before_present(sc);
    }
    const HRESULT hr = real(sc, interval, flags, params);
    if (gate_open()) {
        Reentry guard;
        if (guard.ok()) note_present(sc);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain *sc, UINT count, UINT w,
                                           UINT h, DXGI_FORMAT fmt, UINT flags) {
    ResizeFn real = orig<ResizeFn>(H_SwapChain_ResizeBuffers, sc);
    if (!real) { gate_degrade("ResizeBuffers sin original"); return E_FAIL; }
    const HRESULT hr = real(sc, count, w, h, fmt, flags);
    if (SUCCEEDED(hr) && w && h) {
        collector().set_output(w, h);
        gate_log("swapchain redimensionado a %ux%u", w, h);
    }
    return hr;
}

// --- device ------------------------------------------------------------

using CreateCommittedFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D12Device *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *,
    REFIID, void **);
using CreatePlacedFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D12Device *, ID3D12Heap *, UINT64, const D3D12_RESOURCE_DESC *,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
using CreateReservedFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D12Device *, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE *, REFIID, void **);
using CreateGfxPsoFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D12Device *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **);
using CreateCmpPsoFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D12Device *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **);
using CreateRtvFn = void(STDMETHODCALLTYPE *)(ID3D12Device *, ID3D12Resource *,
                                              const D3D12_RENDER_TARGET_VIEW_DESC *,
                                              D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateDsvFn = void(STDMETHODCALLTYPE *)(ID3D12Device *, ID3D12Resource *,
                                              const D3D12_DEPTH_STENCIL_VIEW_DESC *,
                                              D3D12_CPU_DESCRIPTOR_HANDLE);

u64 ticks_ns() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<u64>(t.QuadPart) * 1000000000ull / static_cast<u64>(f.QuadPart);
}

HRESULT STDMETHODCALLTYPE hk_CreateCommittedResource(
    ID3D12Device *dev, const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS hflags,
    const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out) {
    CreateCommittedFn real = orig<CreateCommittedFn>(H_Device_CreateCommittedResource, dev);
    if (!real) { gate_degrade("CreateCommittedResource sin original"); return E_FAIL; }
    if (!gate_open() || !desc)
        return real(dev, heap, hflags, desc, state, clear, riid, out);

    Reentry guard;
    D3D12_RESOURCE_DESC local = *desc;
    CallsiteId site{};
    if (guard.ok()) {
        site = capture_callsite();
        const ResourceOverride ov = collector().want_override(
            local, heap ? heap->Type : D3D12_HEAP_TYPE_DEFAULT, site);
        if (ov.w && ov.h && local.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
            local.Width = ov.w;
            local.Height = ov.h;
        }
    }
    const HRESULT hr = real(dev, heap, hflags, &local, state, clear, riid, out);
    if (SUCCEEDED(hr) && out && *out && guard.ok())
        collector().on_resource(static_cast<ID3D12Resource *>(*out), local,
                                heap ? heap->Type : D3D12_HEAP_TYPE_DEFAULT, false,
                                site);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreatePlacedResource(
    ID3D12Device *dev, ID3D12Heap *hp, UINT64 off, const D3D12_RESOURCE_DESC *desc,
    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid,
    void **out) {
    CreatePlacedFn real = orig<CreatePlacedFn>(H_Device_CreatePlacedResource, dev);
    if (!real) { gate_degrade("CreatePlacedResource sin original"); return E_FAIL; }
    const HRESULT hr = real(dev, hp, off, desc, state, clear, riid, out);
    // Un recurso colocado NUNCA se escala: achicarlo deja un hueco y el que
    // viene atras en el heap sigue en su offset. Se registra como placed, que
    // es justo lo que hace que el gate lo rechace.
    if (SUCCEEDED(hr) && out && *out && desc && gate_open()) {
        Reentry guard;
        if (guard.ok())
            collector().on_resource(static_cast<ID3D12Resource *>(*out), *desc,
                                    D3D12_HEAP_TYPE_DEFAULT, true,
                                    capture_callsite());
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateReservedResource(
    ID3D12Device *dev, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out) {
    CreateReservedFn real = orig<CreateReservedFn>(H_Device_CreateReservedResource, dev);
    if (!real) { gate_degrade("CreateReservedResource sin original"); return E_FAIL; }
    const HRESULT hr = real(dev, desc, state, clear, riid, out);
    if (SUCCEEDED(hr) && out && *out && desc && gate_open()) {
        Reentry guard;
        if (guard.ok())
            collector().on_resource(static_cast<ID3D12Resource *>(*out), *desc,
                                    D3D12_HEAP_TYPE_CUSTOM, true, capture_callsite());
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateGraphicsPipelineState(
    ID3D12Device *dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid,
    void **out) {
    CreateGfxPsoFn real = orig<CreateGfxPsoFn>(H_Device_CreateGraphicsPipelineState, dev);
    if (!real) { gate_degrade("CreateGraphicsPipelineState sin original"); return E_FAIL; }
    const u64 t0 = ticks_ns();
    const HRESULT hr = real(dev, desc, riid, out);
    if (SUCCEEDED(hr) && desc && gate_open()) {
        Reentry guard;
        if (guard.ok()) {
            // Si esto esta pasando en el hilo que presenta y en pleno
            // gameplay, es un hitch. El analizador necesita las dos cosas: el
            // costo y el hilo.
            collector().on_graphics_pso(*desc,
                                        out ? static_cast<ID3D12PipelineState *>(*out)
                                            : nullptr,
                                        ticks_ns() - t0, true);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateComputePipelineState(
    ID3D12Device *dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid,
    void **out) {
    CreateCmpPsoFn real = orig<CreateCmpPsoFn>(H_Device_CreateComputePipelineState, dev);
    if (!real) { gate_degrade("CreateComputePipelineState sin original"); return E_FAIL; }
    const u64 t0 = ticks_ns();
    const HRESULT hr = real(dev, desc, riid, out);
    if (SUCCEEDED(hr) && desc && gate_open()) {
        Reentry guard;
        if (guard.ok())
            collector().on_compute_pso(*desc,
                                       out ? static_cast<ID3D12PipelineState *>(*out)
                                           : nullptr,
                                       ticks_ns() - t0, true);
    }
    return hr;
}

void STDMETHODCALLTYPE hk_CreateRenderTargetView(
    ID3D12Device *dev, ID3D12Resource *res, const D3D12_RENDER_TARGET_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    CreateRtvFn real = orig<CreateRtvFn>(H_Device_CreateRenderTargetView, dev);
    if (!real) { gate_degrade("CreateRenderTargetView sin original"); return; }
    real(dev, res, desc, handle);
    if (gate_open()) {
        Reentry guard;
        if (guard.ok()) collector().on_rtv(handle, res);
    }
}

void STDMETHODCALLTYPE hk_CreateDepthStencilView(
    ID3D12Device *dev, ID3D12Resource *res, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    CreateDsvFn real = orig<CreateDsvFn>(H_Device_CreateDepthStencilView, dev);
    if (!real) { gate_degrade("CreateDepthStencilView sin original"); return; }
    real(dev, res, desc, handle);
    if (gate_open()) {
        Reentry guard;
        if (guard.ok()) collector().on_dsv(handle, res);
    }
}


// --- command list ------------------------------------------------------

using CloseFn = HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *);
using ResetFn = HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *,
                                             ID3D12CommandAllocator *,
                                             ID3D12PipelineState *);
using DrawFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, UINT,
                                         UINT, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT,
                                                UINT, UINT, INT, UINT);
using DispatchFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, UINT,
                                             UINT);
using CopyResourceFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *,
                                                 ID3D12Resource *, ID3D12Resource *);
using CopyTexRegionFn = void(STDMETHODCALLTYPE *)(
    ID3D12GraphicsCommandList *, const D3D12_TEXTURE_COPY_LOCATION *, UINT, UINT,
    UINT, const D3D12_TEXTURE_COPY_LOCATION *, const D3D12_BOX *);
using ViewportsFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT,
                                              const D3D12_VIEWPORT *);
using SetPsoFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *,
                                           ID3D12PipelineState *);
using BarrierFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT,
                                            const D3D12_RESOURCE_BARRIER *);
using OmSetRtFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE *, BOOL,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE *);

HRESULT STDMETHODCALLTYPE hk_Close(ID3D12GraphicsCommandList *list) {
    CloseFn real = orig<CloseFn>(H_List_Close, list);
    if (!real) { gate_degrade("Close sin original"); return E_FAIL; }
    if (gate_open()) {
        Reentry guard;
        // El cierre de la lista es donde se resuelven los timestamps a su
        // readback: tiene que pasar ANTES del Close de verdad, porque despues
        // la lista ya no acepta comandos.
        if (guard.ok()) collector().cmd_close(list);
    }
    return real(list);
}

HRESULT STDMETHODCALLTYPE hk_Reset(ID3D12GraphicsCommandList *list,
                                   ID3D12CommandAllocator *alloc,
                                   ID3D12PipelineState *pso) {
    ResetFn real = orig<ResetFn>(H_List_Reset, list);
    if (!real) { gate_degrade("Reset sin original"); return E_FAIL; }
    const HRESULT hr = real(list, alloc, pso);
    if (SUCCEEDED(hr) && gate_open()) {
        Reentry guard;
        if (guard.ok()) collector().cmd_begin(list);
    }
    return hr;
}

void STDMETHODCALLTYPE hk_DrawInstanced(ID3D12GraphicsCommandList *list, UINT verts,
                                        UINT instances, UINT first_vert,
                                        UINT first_inst) {
    DrawFn real = orig<DrawFn>(H_List_DrawInstanced, list);
    if (!real) { gate_degrade("DrawInstanced sin original"); return; }
    bool execute = true;
    if (gate_open()) {
        Reentry guard;
        if (guard.ok()) execute = collector().cmd_draw(list, verts, instances);
    }
    if (execute) real(list, verts, instances, first_vert, first_inst);
}

void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D12GraphicsCommandList *list,
                                               UINT indices, UINT instances,
                                               UINT first_index, INT base_vertex,
                                               UINT first_inst) {
    DrawIndexedFn real = orig<DrawIndexedFn>(H_List_DrawIndexedInstanced, list);
    if (!real) { gate_degrade("DrawIndexedInstanced sin original"); return; }
    bool execute = true;
    if (gate_open()) {
        Reentry guard;
        if (guard.ok()) execute = collector().cmd_draw(list, indices, instances);
    }
    if (execute)
        real(list, indices, instances, first_index, base_vertex, first_inst);
}

void STDMETHODCALLTYPE hk_Dispatch(ID3D12GraphicsCommandList *list, UINT x, UINT y,
                                   UINT z) {
    DispatchFn real = orig<DispatchFn>(H_List_Dispatch, list);
    if (!real) { gate_degrade("Dispatch sin original"); return; }
    bool execute = true;
    if (gate_open()) {
        Reentry guard;
        if (guard.ok()) execute = collector().cmd_dispatch(list, x, y, z);
    }
    if (execute) real(list, x, y, z);
}

void STDMETHODCALLTYPE hk_CopyResource(ID3D12GraphicsCommandList *list,
                                       ID3D12Resource *dst, ID3D12Resource *src) {
    CopyResourceFn real = orig<CopyResourceFn>(H_List_CopyResource, list);
    if (!real) { gate_degrade("CopyResource sin original"); return; }
    if (gate_open()) {
        Reentry guard;
        if (guard.ok()) collector().cmd_copy(list, dst, src);
    }
    real(list, dst, src);
}

void STDMETHODCALLTYPE hk_CopyTextureRegion(
    ID3D12GraphicsCommandList *list, const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x,
    UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box) {
    CopyTexRegionFn real = orig<CopyTexRegionFn>(H_List_CopyTextureRegion, list);
    if (!real) { gate_degrade("CopyTextureRegion sin original"); return; }
    if (gate_open()) {
        Reentry guard;
        // Esta es LA razon por la que un recurso no se puede escalar: la copia
        // lleva extents fijos. Que quede anotado es lo que hace que el gate lo
        // rechace en vez de que el juego se corrompa tres minutos despues.
        if (guard.ok() && dst && src)
            collector().cmd_copy(list, dst->pResource, src->pResource);
    }
    real(list, dst, x, y, z, src, box);
}

void STDMETHODCALLTYPE hk_RSSetViewports(ID3D12GraphicsCommandList *list, UINT n,
                                         const D3D12_VIEWPORT *vps) {
    ViewportsFn real = orig<ViewportsFn>(H_List_RSSetViewports, list);
    if (!real) { gate_degrade("RSSetViewports sin original"); return; }
    if (gate_open() && vps && n && n <= 16) {
        Reentry guard;
        if (guard.ok()) {
            D3D12_VIEWPORT scaled[16];
            if (collector().cmd_viewports(list, n, vps, scaled)) {
                real(list, n, scaled);
                return;
            }
        }
    }
    real(list, n, vps);
}

void STDMETHODCALLTYPE hk_SetPipelineState(ID3D12GraphicsCommandList *list,
                                           ID3D12PipelineState *pso) {
    SetPsoFn real = orig<SetPsoFn>(H_List_SetPipelineState, list);
    if (!real) { gate_degrade("SetPipelineState sin original"); return; }
    real(list, pso);
    if (gate_open()) {
        Reentry guard;
        if (guard.ok()) collector().cmd_set_pso(list, pso);
    }
}

void STDMETHODCALLTYPE hk_ResourceBarrier(ID3D12GraphicsCommandList *list, UINT n,
                                          const D3D12_RESOURCE_BARRIER *barriers) {
    BarrierFn real = orig<BarrierFn>(H_List_ResourceBarrier, list);
    if (!real) { gate_degrade("ResourceBarrier sin original"); return; }
    if (gate_open() && barriers && n && n <= 64) {
        Reentry guard;
        if (guard.ok()) {
            D3D12_RESOURCE_BARRIER kept[64];
            const u32 k = collector().cmd_barrier(list, n, barriers, kept);
            // Filtrar TODOS los barriers de una llamada y no llamar al
            // original es valido: es exactamente lo que pide barrier_filter.
            if (k != n) {
                if (k) real(list, k, kept);
                return;
            }
        }
    }
    real(list, n, barriers);
}

void STDMETHODCALLTYPE hk_OMSetRenderTargets(
    ID3D12GraphicsCommandList *list, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs,
    BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE *dsv) {
    OmSetRtFn real = orig<OmSetRtFn>(H_List_OMSetRenderTargets, list);
    if (!real) { gate_degrade("OMSetRenderTargets sin original"); return; }
    real(list, n, rtvs, single, dsv);
    if (gate_open()) {
        Reentry guard;
        // El corte entre pasadas logicas. Va DESPUES del original para que, si
        // el juego cambia de targets y algo falla de nuestro lado, el estado
        // del juego ya este puesto.
        if (guard.ok()) collector().cmd_set_render_targets(list, n, rtvs, single, dsv);
    }
}

// --- command queue -----------------------------------------------------

using ExecuteFn = void(STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT,
                                            ID3D12CommandList *const *);

void attach_list(ID3D12GraphicsCommandList *list);

void STDMETHODCALLTYPE hk_ExecuteCommandLists(ID3D12CommandQueue *queue, UINT n,
                                              ID3D12CommandList *const *lists) {
    ExecuteFn real = orig<ExecuteFn>(H_Queue_ExecuteCommandLists, queue);
    if (!real) { gate_degrade("ExecuteCommandLists sin original"); return; }
    real(queue, n, lists);
    if (gate_open()) {
        Reentry guard;
        if (guard.ok()) {
            // Las listas se enganchan la primera vez que las vemos ejecutar:
            // es el unico momento garantizado en el que existen y en el que
            // nadie las esta grabando.
            for (UINT i = 0; i < n && lists; ++i)
                attach_list(reinterpret_cast<ID3D12GraphicsCommandList *>(lists[i]));
            collector().on_execute(queue, n, lists);
        }
    }
}

// --- enganches ---------------------------------------------------------

void attach_list(ID3D12GraphicsCommandList *list) {
    if (!list) return;
    const unsigned before = g_slots[H_List_Close].n.load(std::memory_order_acquire);
    install(H_List_Close, list, vtbl::GraphicsCommandList::Close,
            reinterpret_cast<void *>(&hk_Close));
    install(H_List_Reset, list, vtbl::GraphicsCommandList::Reset,
            reinterpret_cast<void *>(&hk_Reset));
    install(H_List_DrawInstanced, list, vtbl::GraphicsCommandList::DrawInstanced,
            reinterpret_cast<void *>(&hk_DrawInstanced));
    install(H_List_DrawIndexedInstanced,
            list, vtbl::GraphicsCommandList::DrawIndexedInstanced,
            reinterpret_cast<void *>(&hk_DrawIndexedInstanced));
    install(H_List_Dispatch, list, vtbl::GraphicsCommandList::Dispatch,
            reinterpret_cast<void *>(&hk_Dispatch));
    install(H_List_CopyResource, list, vtbl::GraphicsCommandList::CopyResource,
            reinterpret_cast<void *>(&hk_CopyResource));
    install(H_List_CopyTextureRegion,
            list, vtbl::GraphicsCommandList::CopyTextureRegion,
            reinterpret_cast<void *>(&hk_CopyTextureRegion));
    install(H_List_RSSetViewports, list, vtbl::GraphicsCommandList::RSSetViewports,
            reinterpret_cast<void *>(&hk_RSSetViewports));
    install(H_List_SetPipelineState,
            list, vtbl::GraphicsCommandList::SetPipelineState,
            reinterpret_cast<void *>(&hk_SetPipelineState));
    install(H_List_ResourceBarrier, list, vtbl::GraphicsCommandList::ResourceBarrier,
            reinterpret_cast<void *>(&hk_ResourceBarrier));
    install(H_List_OMSetRenderTargets,
            list, vtbl::GraphicsCommandList::OMSetRenderTargets,
            reinterpret_cast<void *>(&hk_OMSetRenderTargets));
    const unsigned after = g_slots[H_List_Close].n.load(std::memory_order_acquire);
    if (after != before) {
        g_stats.lists = after;
        gate_log("command list: vtable nueva enganchada (van %u)", after);
    }
}

void attach_device(ID3D12Device *dev) {
    if (!dev) return;
    const unsigned before =
        g_slots[H_Device_CreateCommittedResource].n.load(std::memory_order_acquire);
    install(H_Device_CreateCommittedResource, dev,
            vtbl::Device::CreateCommittedResource,
            reinterpret_cast<void *>(&hk_CreateCommittedResource));
    install(H_Device_CreatePlacedResource, dev, vtbl::Device::CreatePlacedResource,
            reinterpret_cast<void *>(&hk_CreatePlacedResource));
    install(H_Device_CreateReservedResource, dev, vtbl::Device::CreateReservedResource,
            reinterpret_cast<void *>(&hk_CreateReservedResource));
    install(H_Device_CreateGraphicsPipelineState, dev,
            vtbl::Device::CreateGraphicsPipelineState,
            reinterpret_cast<void *>(&hk_CreateGraphicsPipelineState));
    install(H_Device_CreateComputePipelineState, dev,
            vtbl::Device::CreateComputePipelineState,
            reinterpret_cast<void *>(&hk_CreateComputePipelineState));
    install(H_Device_CreateRenderTargetView, dev, vtbl::Device::CreateRenderTargetView,
            reinterpret_cast<void *>(&hk_CreateRenderTargetView));
    install(H_Device_CreateDepthStencilView, dev, vtbl::Device::CreateDepthStencilView,
            reinterpret_cast<void *>(&hk_CreateDepthStencilView));
    const unsigned after =
        g_slots[H_Device_CreateCommittedResource].n.load(std::memory_order_acquire);
    g_stats.devices = after;
    if (after != before) gate_log("device: vtable enganchada (van %u)", after);
}

void attach_queue(ID3D12CommandQueue *queue) {
    if (!queue) return;
    // La primera queue directa que vemos es con la que dibuja el overlay.
    if (queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ID3D12CommandQueue *expected = nullptr;
        g_queue.compare_exchange_strong(expected, queue);
    }
    install(H_Queue_ExecuteCommandLists, queue, vtbl::CommandQueue::ExecuteCommandLists,
            reinterpret_cast<void *>(&hk_ExecuteCommandLists));
    g_stats.queues =
        g_slots[H_Queue_ExecuteCommandLists].n.load(std::memory_order_acquire);
}

}  // namespace

namespace {

// Arma colector y ejecutor sobre el device del juego. Es idempotente: gate_arm
// solo deja pasar al primero.
void arm_with_device(ID3D12Device *device, IDXGISwapChain *sc) {
    if (!device) return;
    char exe[MAX_PATH] = "desconocido.exe";
    char full[MAX_PATH];
    if (GetModuleFileNameA(nullptr, full, MAX_PATH)) {
        const char *slash = std::strrchr(full, '\\');
        std::snprintf(exe, sizeof exe, "%s", slash ? slash + 1 : full);
    }
    if (!gate_arm(device, exe)) return;
    g_device.store(device, std::memory_order_release);

    OutputInfo out{};
    if (sc) {
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d))) {
            out.w = d.BufferDesc.Width;
            out.h = d.BufferDesc.Height;
        }
    }
    collector().set_output(out.w, out.h);

    // El ejecutor primero: el colector le consulta desde el primer recurso.
    if (executor().init(exe, out)) collector().set_actions(&executor());

    CollectorConfig cfg;
    cfg.exe_name = exe;
    cfg.deep_every = executor().core().profile().deep_every;
    cfg.query_budget = executor().core().profile().query_budget;

    // CreateDXGIFactory1 se resuelve a mano y no por import: un import
    // estatico a dxgi.dll haria que gpuprobe CARGUE dxgi en cualquier proceso
    // donde este el dll, aunque el juego no use D3D12. La regla es no
    // perturbar nada hasta tener algo que medir.
    IDXGIAdapter3 *adapter = nullptr;
    IDXGIFactory4 *factory = nullptr;
    using CreateFactory1Fn = HRESULT(WINAPI *)(REFIID, void **);
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    auto p_factory1 = dxgi ? reinterpret_cast<CreateFactory1Fn>(
                                 reinterpret_cast<void *>(
                                     GetProcAddress(dxgi, "CreateDXGIFactory1")))
                           : nullptr;
    if (p_factory1 && SUCCEEDED(p_factory1(IID_PPV_ARGS(&factory))) && factory) {
        IDXGIAdapter1 *a1 = nullptr;
        if (SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),
                                                 IID_PPV_ARGS(&a1))) && a1) {
            a1->QueryInterface(IID_PPV_ARGS(&adapter));
            a1->Release();
        }
        factory->Release();
    }
    if (adapter) collector().set_adapter(adapter);

    if (!collector().init(device, cfg))
        gate_degrade("el colector no arranco");
    else
        gate_log("colector armado sobre %s: deep_every=%u query_budget=%u", exe,
                 cfg.deep_every, cfg.query_budget);
}

void arm_from_swapchain(IDXGISwapChain *sc) {
    if (!sc) return;
    // Un swapchain de D3D12 se crea con la COMMAND QUEUE, no con el device, y
    // GetDevice devuelve una cosa o la otra segun el runtime. Se prueban las
    // dos en vez de suponer.
    ID3D12Device *device = nullptr;
    if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&device))) && device) {
        arm_with_device(device, sc);
        device->Release();
        return;
    }
    ID3D12CommandQueue *queue = nullptr;
    if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&queue))) && queue) {
        attach_queue(queue);
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && device) {
            arm_with_device(device, sc);
            device->Release();
        }
        queue->Release();
    }
}

}  // namespace

void hooks_attach_swapchain(IDXGISwapChain *sc, IUnknown *device_or_queue) {
    if (!sc) return;
    install(H_SwapChain_Present, sc, vtbl::SwapChain::Present,
            reinterpret_cast<void *>(&hk_Present));
    install(H_SwapChain_ResizeBuffers, sc, vtbl::SwapChain::ResizeBuffers,
            reinterpret_cast<void *>(&hk_ResizeBuffers));
    {
        IDXGISwapChain1 *sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
            install(H_SwapChain_Present1, sc1, vtbl::SwapChain1::Present1,
                    reinterpret_cast<void *>(&hk_Present1));
            sc1->Release();
        }
    }
    g_stats.swapchains = g_slots[H_SwapChain_Present].n.load(std::memory_order_acquire);

    // En D3D12, CreateSwapChainForHwnd recibe la COMMAND QUEUE, no el device.
    ID3D12CommandQueue *queue = nullptr;
    ID3D12Device *device = nullptr;
    if (device_or_queue &&
        SUCCEEDED(device_or_queue->QueryInterface(IID_PPV_ARGS(&queue))) && queue) {
        attach_queue(queue);
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && device) {
            attach_device(device);
            arm_with_device(device, sc);
            device->Release();
        }
        queue->Release();
    }
}

namespace {

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory *, IUnknown *,
                                                       DXGI_SWAP_CHAIN_DESC *,
                                                       IDXGISwapChain **);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE *)(
    IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);

HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory *factory, IUnknown *dev,
                                             DXGI_SWAP_CHAIN_DESC *desc,
                                             IDXGISwapChain **out) {
    CreateSwapChainFn real = orig<CreateSwapChainFn>(H_Factory_CreateSwapChain, factory);
    if (!real) { gate_degrade("CreateSwapChain sin original"); return E_FAIL; }
    const HRESULT hr = real(factory, dev, desc, out);
    if (SUCCEEDED(hr) && out && *out && !reentrant()) {
        Reentry guard;
        if (guard.ok()) hooks_attach_swapchain(*out, dev);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(
    IDXGIFactory2 *factory, IUnknown *dev, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs,
    IDXGIOutput *output, IDXGISwapChain1 **out) {
    CreateSwapChainForHwndFn real =
        orig<CreateSwapChainForHwndFn>(H_Factory_CreateSwapChainForHwnd, factory);
    if (!real) { gate_degrade("CreateSwapChainForHwnd sin original"); return E_FAIL; }
    const HRESULT hr = real(factory, dev, hwnd, desc, fs, output, out);
    if (SUCCEEDED(hr) && out && *out && !reentrant()) {
        Reentry guard;
        if (guard.ok()) hooks_attach_swapchain(*out, dev);
    }
    return hr;
}

}  // namespace

void hooks_attach_factory(IDXGIFactory *factory) {
    if (!factory) return;
    install(H_Factory_CreateSwapChain, factory, vtbl::Factory::CreateSwapChain,
            reinterpret_cast<void *>(&hk_CreateSwapChain));
    IDXGIFactory2 *f2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f2))) && f2) {
        install(H_Factory_CreateSwapChainForHwnd, f2,
                vtbl::Factory2::CreateSwapChainForHwnd,
                reinterpret_cast<void *>(&hk_CreateSwapChainForHwnd));
        f2->Release();
    }
    g_stats.factories =
        g_slots[H_Factory_CreateSwapChain].n.load(std::memory_order_acquire);
    gate_log("fabrica DXGI enganchada (van %u)", g_stats.factories);
}

bool hooks_steal_vtables() {
    // Orden importante: la fabrica se engancha AL FINAL. Si se enganchara
    // primero, crear nuestro propio swapchain entraria por nuestro propio hook
    // y armaria el colector sobre el device descartable.
    HMODULE d3d12 = GetModuleHandleA("d3d12.dll");
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    if (!d3d12 || !dxgi) return false;

    // GetProcAddress devuelve FARPROC y el compilador se queja con razon de
    // castear entre tipos de funcion distintos; el puente por void* deja el
    // cast explicito y en un solo lugar.
    using CreateFactory2Fn = HRESULT(WINAPI *)(UINT, REFIID, void **);
    auto p_create_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        reinterpret_cast<void *>(GetProcAddress(d3d12, "D3D12CreateDevice")));
    auto p_create_factory = reinterpret_cast<CreateFactory2Fn>(
        reinterpret_cast<void *>(GetProcAddress(dxgi, "CreateDXGIFactory2")));
    if (!p_create_device || !p_create_factory) return false;

    IDXGIFactory2 *factory = nullptr;
    if (FAILED(p_create_factory(0, IID_PPV_ARGS(&factory))) || !factory) return false;

    ID3D12Device *device = nullptr;
    if (FAILED(p_create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                               IID_PPV_ARGS(&device))) || !device) {
        factory->Release();
        return false;
    }
    attach_device(device);

    ID3D12CommandQueue *queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && queue) {
        attach_queue(queue);

        ID3D12CommandAllocator *alloc = nullptr;
        ID3D12GraphicsCommandList *list = nullptr;
        if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&alloc))) &&
            SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                alloc, nullptr,
                                                IID_PPV_ARGS(&list))) && list) {
            attach_list(list);
            list->Close();
            list->Release();
        }
        if (alloc) alloc->Release();

        // Un swapchain descartable sobre una ventana oculta: es la unica forma
        // de tener en la mano la vtable de IDXGISwapChain, que es la que trae
        // Present.
        WNDCLASSEXA wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "gpuprobe_dummy";
        RegisterClassExA(&wc);
        HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "gpuprobe", WS_OVERLAPPED,
                                    0, 0, 16, 16, nullptr, nullptr, wc.hInstance,
                                    nullptr);
        if (hwnd) {
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = 16;
            sd.Height = 16;
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            IDXGISwapChain1 *sc = nullptr;
            if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr,
                                                          nullptr, &sc)) && sc) {
                install(H_SwapChain_Present, sc, vtbl::SwapChain::Present,
                        reinterpret_cast<void *>(&hk_Present));
                install(H_SwapChain_ResizeBuffers, sc, vtbl::SwapChain::ResizeBuffers,
                        reinterpret_cast<void *>(&hk_ResizeBuffers));
                install(H_SwapChain_Present1, sc, vtbl::SwapChain1::Present1,
                        reinterpret_cast<void *>(&hk_Present1));
                g_stats.swapchains =
                    g_slots[H_SwapChain_Present].n.load(std::memory_order_acquire);
                sc->Release();
            }
            DestroyWindow(hwnd);
        }
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        queue->Release();
    }

    hooks_attach_factory(factory);
    factory->Release();
    device->Release();

    gate_log("vtables robadas: fabrica %u, swapchain %u, device %u, queue %u, "
             "listas %u", g_stats.factories, g_stats.swapchains, g_stats.devices,
             g_stats.queues, g_stats.lists);
    // Si no enganchamos ni Present ni ExecuteCommandLists no hay nada que
    // hacer: mejor decirlo que fingir que estamos midiendo.
    return g_stats.swapchains > 0 && g_stats.queues > 0;
}

HookStats hooks_stats() { return g_stats; }

}  // namespace gp
