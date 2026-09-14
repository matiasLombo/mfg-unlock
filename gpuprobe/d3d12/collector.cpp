// collector.cpp -- ver collector.h.
//
// Orden del archivo: estado global, escritor, tablas, queries, y recien
// despues la API publica. Lo de arriba es lo que hace que lo de abajo pueda
// cumplir la regla de no alocar ni bloquear en el hot path.
#define WIN32_LEAN_AND_MEAN
#include "collector.h"

#include <windows.h>

#include <atomic>
#include <new>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "capture.h"
#include "core/hash.h"
#include "core/jsonl.h"

// CaptureStackBackTrace es la macro que winbase.h mapea a
// RtlCaptureStackBackTrace. Usar la macro en vez de declarar el simbolo a mano
// evita tener que linkear ntdll y funciona igual con MSVC y con mingw.

namespace gp {

namespace {

constexpr u32 kSlots = 4;          // frames en vuelo antes de cobrar timestamps
constexpr u32 kMaxQueries = 1024;  // por slot; cada pasada usa 2
constexpr u32 kQueryBlock = 32;    // lo que reserva una lista de una vez
constexpr u32 kMaxQueues = 8;
constexpr u32 kRtvTableBits = 13;  // 8192 entradas
constexpr u32 kRtvTableSize = 1u << kRtvTableBits;
constexpr u32 kMaxRings = 64;

// --- reloj -------------------------------------------------------------

u64 qpc_freq() {
    static u64 f = [] {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        return static_cast<u64>(li.QuadPart);
    }();
    return f;
}

u64 now_ns() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return static_cast<u64>(li.QuadPart) * 1000000000ull / qpc_freq();
}

// --- rings por hilo ----------------------------------------------------
//
// Un ring por hilo que graba. El escritor los recorre todos; los productores
// no se ven entre si y nadie toma un lock para empujar un evento.

struct RingSlot {
    std::atomic<Ring *> ring{nullptr};
};

RingSlot g_rings[kMaxRings];
std::atomic<u32> g_ring_count{0};
std::atomic<u64> g_dropped_this_frame{0};

Ring *thread_ring() {
    static thread_local Ring *mine = nullptr;
    if (mine) return mine;
    const u32 idx = g_ring_count.fetch_add(1, std::memory_order_relaxed);
    if (idx >= kMaxRings) {
        // Mas de 64 hilos grabando: el hilo 65 no mide. Preferimos perder
        // eventos de un hilo a crecer sin limite adentro del juego.
        return nullptr;
    }
    static thread_local Ring storage;
    mine = &storage;
    g_rings[idx].ring.store(mine, std::memory_order_release);
    return mine;
}

void push(const Event &e) {
    Ring *r = thread_ring();
    if (!r) return;
    if (!r->push(e)) g_dropped_this_frame.fetch_add(1, std::memory_order_relaxed);
}

// --- escritor ----------------------------------------------------------

class Writer {
public:
    bool open(const wchar_t *path, const char *exe, const char *adapter,
              OutputInfo out, u64 budget) {
        file_ = _wfopen(path, L"wb");
        if (!file_) return false;
        char line[kMaxLine];
        const size_t n = format_header(line, sizeof line, exe, adapter, out,
                                       budget, "gpuprobe 0.1.0");
        fwrite(line, 1, n, file_);
        fputc('\n', file_);
        out_ = out;
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void set_output(OutputInfo o) { out_ = o; }

    void close() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
        drain_once();
        if (file_) {
            fflush(file_);
            fclose(file_);
            file_ = nullptr;
        }
    }

private:
    void loop() {
        while (running_.load(std::memory_order_acquire)) {
            const size_t n = drain_once();
            // Sin nada que hacer, dormir. Con trabajo, seguir: en un frame
            // pesado el ring se llena en milisegundos.
            if (n == 0) Sleep(2);
        }
    }

    size_t drain_once() {
        Event buf[256];
        char line[kMaxLine];
        size_t total = 0;
        const u32 count = g_ring_count.load(std::memory_order_acquire);
        for (u32 i = 0; i < count && i < kMaxRings; ++i) {
            Ring *r = g_rings[i].ring.load(std::memory_order_acquire);
            if (!r) continue;
            for (;;) {
                const size_t n = r->drain(buf, 256);
                if (n == 0) break;
                total += n;
                for (size_t k = 0; k < n; ++k) {
                    const size_t len = format_event(line, sizeof line, buf[k], out_);
                    if (len && file_) {
                        fwrite(line, 1, len, file_);
                        fputc('\n', file_);
                    }
                }
            }
        }
        if (total && file_) fflush(file_);
        return total;
    }

    FILE       *file_ = nullptr;
    OutputInfo  out_{};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// --- tabla de vistas ---------------------------------------------------
//
// handle.ptr -> recurso. Se escribe cuando el juego crea un RTV/DSV (raro) y
// se lee en cada OMSetRenderTargets (hot path), asi que la lectura es sin
// lock: open addressing, publicacion con release y lectura con acquire.

struct RtvEntry {
    std::atomic<SIZE_T> handle{0};
    ResKey  res{};
    DescKey dkey{};
    u32     w = 0, h = 0;
    u64     bytes = 0;
    // Si el ejecutor creo este recurso mas chico, cuanto. Vive aca y no en un
    // mapa para que OMSetRenderTargets no tenga que tomar un lock: el viewport
    // de la pasada depende de este numero y se consulta en cada pasada.
    float   scale = 1.0f;
};

RtvEntry g_rtv[kRtvTableSize];

u32 rtv_slot(SIZE_T handle) {
    return static_cast<u32>(hash_bytes(&handle, sizeof handle) & (kRtvTableSize - 1));
}

void rtv_put(SIZE_T handle, ResKey res, DescKey dkey, u32 w, u32 h, u64 bytes,
             float scale) {
    u32 i = rtv_slot(handle);
    for (u32 probe = 0; probe < 64; ++probe, i = (i + 1) & (kRtvTableSize - 1)) {
        const SIZE_T cur = g_rtv[i].handle.load(std::memory_order_acquire);
        if (cur == 0 || cur == handle) {
            g_rtv[i].res = res;
            g_rtv[i].dkey = dkey;
            g_rtv[i].w = w;
            g_rtv[i].h = h;
            g_rtv[i].bytes = bytes;
            g_rtv[i].scale = scale;
            g_rtv[i].handle.store(handle, std::memory_order_release);
            return;
        }
    }
    // Tabla llena en esa zona: se pierde la asociacion de ESA vista. La pasada
    // se registra igual, con el RT en cero, y eso se ve en el reporte.
}

const RtvEntry *rtv_get(SIZE_T handle) {
    u32 i = rtv_slot(handle);
    for (u32 probe = 0; probe < 64; ++probe, i = (i + 1) & (kRtvTableSize - 1)) {
        const SIZE_T cur = g_rtv[i].handle.load(std::memory_order_acquire);
        if (cur == handle) return &g_rtv[i];
        if (cur == 0) return nullptr;
    }
    return nullptr;
}

// --- conversion de descriptores ---------------------------------------

ResourceDesc from_d3d12(const D3D12_RESOURCE_DESC &d, D3D12_HEAP_TYPE heap,
                        bool placed) {
    ResourceDesc r;
    switch (d.Dimension) {
        case D3D12_RESOURCE_DIMENSION_BUFFER:    r.dim = Dim::Buffer; break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D: r.dim = Dim::Texture1D; break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D: r.dim = Dim::Texture2D; break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D: r.dim = Dim::Texture3D; break;
        default: r.dim = Dim::Unknown; break;
    }
    // Los valores de Format y de los flags son los mismos que en DXGI/D3D12:
    // ver core/types.h. Por eso esto es un cast y no una tabla que mantener.
    r.fmt = static_cast<Format>(d.Format);
    r.w = static_cast<u32>(d.Width);
    r.h = d.Height;
    r.depth = d.DepthOrArraySize;
    r.mips = d.MipLevels;
    r.samples = d.SampleDesc.Count;
    r.flags = static_cast<u32>(d.Flags);
    r.heap = placed ? HeapKind::Placed
                    : (heap == D3D12_HEAP_TYPE_DEFAULT)  ? HeapKind::Default
                    : (heap == D3D12_HEAP_TYPE_UPLOAD)   ? HeapKind::Upload
                    : (heap == D3D12_HEAP_TYPE_READBACK) ? HeapKind::Readback
                    : (heap == D3D12_HEAP_TYPE_CUSTOM)   ? HeapKind::Custom
                                                         : HeapKind::Unknown;
    r.bytes = (r.dim == Dim::Buffer)
                  ? d.Width
                  : texture_bytes(r.fmt, r.w, r.h,
                                  r.dim == Dim::Texture3D ? r.depth : 1, r.mips,
                                  r.dim == Dim::Texture3D ? 1 : r.depth);
    return r;
}

}  // namespace

// --- el estado grande --------------------------------------------------

struct PassRecord {
    PassKey  key;
    RtSetKey rts;
    PsoKey   pso;
    ResKey   rt_key;
    DescKey  rt_dkey;
    u32      q_begin = 0, q_end = 0;
    u32      draws = 0;
    u32      ordinal = 0;
    u32      queue = 0;
    u32      rt_w = 0, rt_h = 0;
    u64      rt_bytes = 0;
    u64      frame = 0;
};

struct ListState {
    ID3D12GraphicsCommandList *list = nullptr;
    u32      slot = 0;
    u32      queue = 0;
    PsoKey   cur_pso{};
    RtSetKey cur_rts{};
    ResKey   rt_key{};
    DescKey  rt_dkey{};
    u32      rt_w = 0, rt_h = 0;
    u64      rt_bytes = 0;
    float    rt_scale = 1.0f;
    bool     pass_open = false;
    bool     pass_skipped = false;  // skip_pass activo en la pasada actual
    u32      q_begin = 0;
    u32      draws = 0;
    u32      ordinal = 0;
    PassKey  cur_pass{};
    u64      frame = 0;
    // Reservado una vez y reusado: despues del primer frame no hay allocs.
    std::vector<PassRecord> passes;
    // Bloque de queries reservado del slot.
    u32      q_next = 0, q_limit = 0;
};

struct QueueInfo {
    ID3D12CommandQueue *queue = nullptr;
    ID3D12Fence        *fence = nullptr;
    u64                 next_value = 0;
    u64                 freq = 0;
    u32                 index = 0;
};

struct SlotState {
    ID3D12QueryHeap *heap = nullptr;
    ID3D12Resource  *readback = nullptr;
    std::atomic<u32> next_query{0};
    u64              pending[kMaxQueues] = {};
    std::vector<PassRecord> passes;
    u64              frame = 0;
    bool             in_flight = false;
};

struct Collector::Impl {
    ID3D12Device  *device = nullptr;
    IDXGIAdapter3 *adapter = nullptr;
    CollectorConfig cfg;
    Writer writer;

    SlotState slots[kSlots];
    u32       cur_slot = 0;

    QueueInfo queues[kMaxQueues];
    u32       nqueues = 0;

    std::mutex          tables;      // recursos, listas, queues: fuera del hot path
    std::unordered_map<void *, ResourceDesc> resources;
    std::unordered_map<void *, ListState *>  lists;
    std::vector<ListState *>                 list_pool;
    std::unordered_map<void *, PsoKey>       psos;
    // dkey del recurso YA escalado -> escala que se le aplico. Se consulta al
    // crear la vista (raro), no al grabar (hot path).
    std::unordered_map<u64, float>           scaled;

    // Tabla viva para el overlay. El costo se suaviza con una media movil
    // exponencial: sin eso la tabla parpadea y no se puede leer.
    std::unordered_map<u64, Collector::PassLive> live;
    mutable std::mutex live_mu;
    double frame_ms_ema = 0.0;
    u64    vram_budget = 0, vram_usage = 0;

    u64 last_present_ns = 0;
    // La ruta de la sesion sin extension: las capturas del harness van al lado
    // del JSONL y con el mismo prefijo, que es como el analizador las encuentra.
    char session_base[MAX_PATH * 2] = {};
};

namespace {

Collector g_collector;

}  // namespace

Collector &collector() { return g_collector; }

CallsiteId capture_callsite() {
    // Hasta 12 marcos, salteando los dos de adentro de gpuprobe. Las
    // direcciones se normalizan contra la base del modulo principal: con ASLR,
    // hashear direcciones crudas daria un callsite distinto en cada corrida y
    // dos sesiones del mismo juego no se podrian comparar.
    void *frames[12];
    const USHORT n = CaptureStackBackTrace(2, 12, frames, nullptr);
    if (n == 0) return CallsiteId{0};
    static u64 base = reinterpret_cast<u64>(GetModuleHandleW(nullptr));
    u64 h = 0x63616C6C736974ull;  // "callsit"
    for (USHORT i = 0; i < n; ++i) {
        const u64 a = reinterpret_cast<u64>(frames[i]);
        h = hash_combine(h, (a > base && a - base < (1ull << 32)) ? a - base : a);
    }
    return CallsiteId{h};
}

// --- init / shutdown ---------------------------------------------------

bool Collector::init(ID3D12Device *device, const CollectorConfig &cfg) {
    if (armed_ || !device) return false;
    Impl *im = new (std::nothrow) Impl();
    if (!im) return false;
    im->device = device;
    im->cfg = cfg;

    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kMaxQueries;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = kMaxQueries * sizeof(u64);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (u32 i = 0; i < kSlots; ++i) {
        if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&im->slots[i].heap)))) {
            delete im;
            return false;
        }
        if (FAILED(device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&im->slots[i].readback)))) {
            delete im;
            return false;
        }
        im->slots[i].passes.reserve(512);
    }

    wchar_t path[MAX_PATH * 2];
    if (cfg.session_path && cfg.session_path[0]) {
        wcsncpy_s(path, cfg.session_path, _TRUNCATE);
    } else {
        wchar_t local[MAX_PATH];
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
            delete im;
            return false;
        }
        // Nunca se escribe en la carpeta del juego: es una de las
        // restricciones duras del proyecto.
        swprintf_s(path, L"%s\\gpuprobe", local);
        CreateDirectoryW(path, nullptr);
        swprintf_s(path, L"%s\\gpuprobe\\sessions", local);
        CreateDirectoryW(path, nullptr);
        SYSTEMTIME st;
        GetLocalTime(&st);
        swprintf_s(path, L"%s\\gpuprobe\\sessions\\%04d%02d%02d-%02d%02d%02d.jsonl",
                   local, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                   st.wSecond);
    }

    // El adaptador se setea aparte (set_adapter), que puede pasar antes o
    // despues de init. Si no vino, el header queda con "desconocido" y el
    // analizador lo dice en vez de inventar un nombre.
    char adapter_name[256] = "desconocido";
    u64 budget = 0;
    if (im->adapter) {
        DXGI_ADAPTER_DESC1 ad{};
        if (SUCCEEDED(im->adapter->GetDesc1(&ad)))
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, adapter_name,
                                sizeof adapter_name, nullptr, nullptr);
        DXGI_QUERY_VIDEO_MEMORY_INFO vm{};
        if (SUCCEEDED(im->adapter->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)))
            budget = vm.Budget;
    }
    if (!im->writer.open(path, cfg.exe_name, adapter_name, out_, budget)) {
        delete im;
        return false;
    }

    // La base para las capturas: la misma ruta sin ".jsonl".
    {
        char narrow[MAX_PATH * 2];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof narrow, nullptr,
                            nullptr);
        size_t n = strlen(narrow);
        if (n > 6 && strcmp(narrow + n - 6, ".jsonl") == 0) narrow[n - 6] = 0;
        std::snprintf(im->session_base, sizeof im->session_base, "%s", narrow);
    }

    im->last_present_ns = now_ns();
    impl_ = im;
    armed_ = true;
    return true;
}

void Collector::shutdown() {
    if (!impl_) return;
    armed_ = false;
    impl_->writer.close();
    for (u32 i = 0; i < kSlots; ++i) {
        if (impl_->slots[i].heap) impl_->slots[i].heap->Release();
        if (impl_->slots[i].readback) impl_->slots[i].readback->Release();
    }
    for (u32 i = 0; i < impl_->nqueues; ++i)
        if (impl_->queues[i].fence) impl_->queues[i].fence->Release();
    for (ListState *ls : impl_->list_pool) delete ls;
    delete impl_;
    impl_ = nullptr;
}

void Collector::set_output(u32 w, u32 h) {
    out_.w = w;
    out_.h = h;
    if (impl_) impl_->writer.set_output(out_);
}

void Collector::set_adapter(IDXGIAdapter3 *adapter) {
    if (impl_) impl_->adapter = adapter;
}

// --- recursos ----------------------------------------------------------

ResourceOverride Collector::want_override(const D3D12_RESOURCE_DESC &desc,
                                          D3D12_HEAP_TYPE heap,
                                          CallsiteId site) {
    if (!actions_ || !armed_) return ResourceOverride{};
    const ResourceDesc rd = from_d3d12(desc, heap, false);
    const ResourceOverride ov = actions_->on_create(rd, site);
    if (ov.w && ov.h && (ov.w != rd.w || ov.h != rd.h)) {
        // Se anota la escala contra la clave del recurso COMO VA A QUEDAR: es
        // la que va a tener cuando se cree y la unica con la que despues se lo
        // puede reconocer.
        ResourceDesc scaled = rd;
        scaled.w = ov.w;
        scaled.h = ov.h;
        const float s = rd.w ? static_cast<float>(ov.w) / static_cast<float>(rd.w)
                             : 1.0f;
        std::lock_guard<std::mutex> lk(impl_->tables);
        impl_->scaled[desc_key(scaled, out_).v] = s;
    }
    return ov;
}

void Collector::on_resource(ID3D12Resource *res, const D3D12_RESOURCE_DESC &desc,
                            D3D12_HEAP_TYPE heap, bool placed, CallsiteId site) {
    if (!armed_ || !res) return;
    ResourceDesc rd = from_d3d12(desc, heap, placed);
    // El device sabe el tamano real con alineado; el nuestro es el piso.
    if (impl_->device && rd.dim != Dim::Buffer) {
        const D3D12_RESOURCE_ALLOCATION_INFO ai =
            impl_->device->GetResourceAllocationInfo(0, 1, &desc);
        if (ai.SizeInBytes && ai.SizeInBytes != UINT64_MAX) rd.bytes = ai.SizeInBytes;
    }

    {
        std::lock_guard<std::mutex> lk(impl_->tables);
        impl_->resources[res] = rd;
    }
    if (actions_)
        actions_->note_resource(desc_key(rd, out_), rd, placed,
                                rd.heap == HeapKind::Reserved);

    Event e;
    e.kind = EventKind::ResourceCreate;
    e.frame = frame_;
    e.cpu_ns = now_ns();
    // La clave es el puntero. Un puntero se puede reusar despues de liberar el
    // recurso, y por eso sale tambien res_free: el analizador da la clave por
    // viva entre create y free, no para siempre.
    e.resource.key = ResKey{reinterpret_cast<u64>(res)};
    e.resource.dkey = desc_key(rd, out_);
    e.resource.site = site;
    e.resource.desc = rd;
    push(e);
}

void Collector::on_resource_release(ID3D12Resource *res) {
    if (!armed_ || !res) return;
    {
        std::lock_guard<std::mutex> lk(impl_->tables);
        impl_->resources.erase(res);
    }
    Event e;
    e.kind = EventKind::ResourceDestroy;
    e.frame = frame_;
    e.resource.key = ResKey{reinterpret_cast<u64>(res)};
    push(e);
}

void Collector::note_backbuffers(IDXGISwapChain *swapchain) {
    if (!armed_ || !swapchain) return;
    DXGI_SWAP_CHAIN_DESC sd{};
    if (FAILED(swapchain->GetDesc(&sd))) return;
    const UINT count = sd.BufferCount ? sd.BufferCount : 2;
    for (UINT i = 0; i < count; ++i) {
        ID3D12Resource *buf = nullptr;
        if (FAILED(swapchain->GetBuffer(i, IID_PPV_ARGS(&buf))) || !buf) continue;
        const D3D12_RESOURCE_DESC d = buf->GetDesc();
        ResourceDesc rd = from_d3d12(d, D3D12_HEAP_TYPE_DEFAULT, false);
        rd.swapchain = true;
        {
            std::lock_guard<std::mutex> lk(impl_->tables);
            impl_->resources[buf] = rd;
        }
        Event e;
        e.kind = EventKind::ResourceCreate;
        e.frame = frame_;
        e.resource.key = ResKey{reinterpret_cast<u64>(buf)};
        e.resource.dkey = desc_key(rd, out_);
        e.resource.desc = rd;
        push(e);
        // GetBuffer sube el refcount; el swapchain es el dueno, no nosotros.
        buf->Release();
    }
}

void Collector::on_rtv(D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource *res) {
    if (!armed_ || !res || !h.ptr) return;
    ResourceDesc rd;
    float scale = 1.0f;
    {
        std::lock_guard<std::mutex> lk(impl_->tables);
        auto it = impl_->resources.find(res);
        if (it == impl_->resources.end()) return;  // recurso que no vimos crear
        rd = it->second;
        auto sc = impl_->scaled.find(desc_key(rd, out_).v);
        if (sc != impl_->scaled.end()) scale = sc->second;
    }
    rtv_put(h.ptr, ResKey{reinterpret_cast<u64>(res)}, desc_key(rd, out_), rd.w,
            rd.h, rd.bytes, scale);
}

void Collector::on_dsv(D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource *res) {
    on_rtv(h, res);  // la tabla es la misma: lo unico que importa es el handle
}

// --- PSOs --------------------------------------------------------------

namespace {

PsoKey hash_graphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &d) {
    // El hash sale del BYTECODE de cada etapa, no del puntero al PSO: dos
    // corridas del mismo juego tienen que dar la misma clave para poder
    // comparar sesiones.
    const StageBlob stages[5] = {
        {d.VS.pShaderBytecode, d.VS.BytecodeLength},
        {d.HS.pShaderBytecode, d.HS.BytecodeLength},
        {d.DS.pShaderBytecode, d.DS.BytecodeLength},
        {d.GS.pShaderBytecode, d.GS.BytecodeLength},
        {d.PS.pShaderBytecode, d.PS.BytecodeLength},
    };
    Format rts[8]{};
    const u32 n = d.NumRenderTargets > 8 ? 8 : d.NumRenderTargets;
    for (u32 i = 0; i < n; ++i) rts[i] = static_cast<Format>(d.RTVFormats[i]);
    return pso_key(stages, 5, rts, n, static_cast<Format>(d.DSVFormat));
}

u32 graphics_stage_mask(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &d) {
    u32 m = 0;
    if (d.VS.BytecodeLength) m |= 1u << 0;
    if (d.PS.BytecodeLength) m |= 1u << 1;
    if (d.DS.BytecodeLength) m |= 1u << 2;
    if (d.HS.BytecodeLength) m |= 1u << 3;
    if (d.GS.BytecodeLength) m |= 1u << 4;
    return m;
}

u64 graphics_bytes(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &d) {
    return d.VS.BytecodeLength + d.PS.BytecodeLength + d.DS.BytecodeLength +
           d.HS.BytecodeLength + d.GS.BytecodeLength;
}

}  // namespace

PsoKey Collector::on_graphics_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc,
                                  ID3D12PipelineState *pso, u64 compile_ns,
                                  bool render_thread) {
    if (!armed_) return PsoKey{};
    const PsoKey key = hash_graphics(desc);
    if (pso) {
        std::lock_guard<std::mutex> lk(impl_->tables);
        impl_->psos[pso] = key;
    }
    Event e;
    e.kind = EventKind::Pso;
    e.frame = frame_;
    e.cpu_ns = now_ns();
    e.flags = render_thread ? kEvRenderThread : kEvNone;
    e.pso.key = key;
    e.pso.compile_ns = compile_ns;
    e.pso.bytecode_bytes = graphics_bytes(desc);
    e.pso.stage_mask = graphics_stage_mask(desc);
    e.pso.is_compute = 0;
    push(e);
    return key;
}

PsoKey Collector::on_compute_pso(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc,
                                 ID3D12PipelineState *pso, u64 compile_ns,
                                 bool render_thread) {
    if (!armed_) return PsoKey{};
    const StageBlob cs{desc.CS.pShaderBytecode, desc.CS.BytecodeLength};
    const PsoKey key = pso_key(&cs, 1, nullptr, 0, Format::Unknown);
    if (pso) {
        std::lock_guard<std::mutex> lk(impl_->tables);
        impl_->psos[pso] = key;
    }
    Event e;
    e.kind = EventKind::Pso;
    e.frame = frame_;
    e.cpu_ns = now_ns();
    e.flags = render_thread ? kEvRenderThread : kEvNone;
    e.pso.key = key;
    e.pso.compile_ns = compile_ns;
    e.pso.bytecode_bytes = desc.CS.BytecodeLength;
    e.pso.stage_mask = 1u << 5;
    e.pso.is_compute = 1;
    push(e);
    return key;
}

// --- estado por command list -------------------------------------------

namespace {

thread_local ListState *tls_last = nullptr;

}  // namespace

ListState *Collector_find_list(Collector::Impl *im, ID3D12GraphicsCommandList *l) {
    // Cache por hilo: una lista la graba un hilo por vez, asi que el hit es
    // practicamente siempre y no se toca el mutex en el hot path.
    if (tls_last && tls_last->list == l) return tls_last;
    std::lock_guard<std::mutex> lk(im->tables);
    auto it = im->lists.find(l);
    if (it == im->lists.end()) return nullptr;
    tls_last = it->second;
    return tls_last;
}

void Collector::cmd_begin(ID3D12GraphicsCommandList *list) {
    if (!armed_ || !list) return;
    ListState *ls = nullptr;
    {
        std::lock_guard<std::mutex> lk(impl_->tables);
        auto it = impl_->lists.find(list);
        if (it != impl_->lists.end()) {
            ls = it->second;
        } else {
            if (!impl_->list_pool.empty()) {
                // Reuso: despues del primer frame no se aloca mas.
                ls = impl_->list_pool.back();
                impl_->list_pool.pop_back();
            } else {
                ls = new (std::nothrow) ListState();
                if (!ls) return;
                ls->passes.reserve(256);
            }
            impl_->lists[list] = ls;
        }
    }
    ls->list = list;
    ls->slot = impl_->cur_slot;
    ls->frame = frame_;
    ls->pass_open = false;
    ls->pass_skipped = false;
    ls->draws = 0;
    ls->ordinal = 0;
    ls->q_next = ls->q_limit = 0;
    ls->passes.clear();
    ls->cur_pso = PsoKey{};
    ls->cur_rts = RtSetKey{};
    ls->rt_key = ResKey{};
    ls->rt_dkey = DescKey{};
    ls->rt_w = ls->rt_h = 0;
    ls->rt_bytes = 0;
    ls->rt_scale = 1.0f;
    tls_last = ls;
}

namespace {

// Reserva de queries: un bloque por lista, y cuando se agota se pide otro. El
// presupuesto del slot es fijo, asi que cuando se acaba simplemente se deja de
// medir y se cuenta -- nunca se crece un heap adentro de un frame.
bool take_query(Collector::Impl *im, ListState *ls, u32 *out) {
    if (ls->q_next >= ls->q_limit) {
        SlotState &slot = im->slots[ls->slot];
        const u32 base = slot.next_query.fetch_add(kQueryBlock, std::memory_order_relaxed);
        if (base + kQueryBlock > kMaxQueries) return false;
        ls->q_next = base;
        ls->q_limit = base + kQueryBlock;
    }
    *out = ls->q_next++;
    return true;
}

// Abre una pasada si no hay ninguna abierta. El corte normal es el cambio de
// render targets, pero una command list de COMPUTE nunca llama
// OMSetRenderTargets: su pasada empieza en el primer dispatch y su identidad
// es el PSO. Sin esto, todo el trabajo de async compute quedaba sin medir --
// y era justo el caso que el modelo de frame necesita para poder decir "esta
// pasada corre tapada".
bool open_pass(Collector::Impl *im, ListState *ls,
               ID3D12GraphicsCommandList *list, bool deep, u32 budget) {
    if (ls->pass_open) return true;
    const u32 used = im->slots[ls->slot].next_query.load(std::memory_order_relaxed);
    if (!deep && used >= budget) return false;
    u32 q = 0;
    if (!take_query(im, ls, &q)) return false;
    list->EndQuery(im->slots[ls->slot].heap, D3D12_QUERY_TYPE_TIMESTAMP, q);
    ls->q_begin = q;
    ls->pass_open = true;
    ls->draws = 0;
    return true;
}

void close_pass(Collector::Impl *im, ListState *ls) {
    if (!ls->pass_open) return;
    ls->pass_open = false;
    u32 q_end = 0;
    if (take_query(im, ls, &q_end)) {
        ls->list->EndQuery(im->slots[ls->slot].heap,
                           D3D12_QUERY_TYPE_TIMESTAMP, q_end);
        PassRecord pr;
        pr.key = ls->cur_pass;
        pr.rts = ls->cur_rts;
        pr.pso = ls->cur_pso;
        pr.rt_key = ls->rt_key;
        pr.rt_dkey = ls->rt_dkey;
        pr.q_begin = ls->q_begin;
        pr.q_end = q_end;
        pr.draws = ls->draws;
        pr.ordinal = ls->ordinal;
        pr.rt_w = ls->rt_w;
        pr.rt_h = ls->rt_h;
        pr.rt_bytes = ls->rt_bytes;
        pr.frame = ls->frame;
        ls->passes.push_back(pr);
    }
    ls->draws = 0;
    ++ls->ordinal;
}

}  // namespace

void Collector::cmd_set_render_targets(ID3D12GraphicsCommandList *list, u32 n,
                                       const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs,
                                       BOOL single_handle,
                                       const D3D12_CPU_DESCRIPTOR_HANDLE *dsv) {
    if (!armed_) return;
    ListState *ls = Collector_find_list(impl_, list);
    if (!ls) return;

    // Un cambio de render targets es el corte natural entre pasadas logicas.
    // Es lo mismo que hacen PIX y Nsight para agrupar, y tiene la ventaja de
    // no depender de que el juego marque nada.
    close_pass(impl_, ls);

    DescKey keys[9]{};
    u32 nk = 0;
    ResKey main_res{};
    DescKey main_dkey{};
    u32 w = 0, h = 0;
    u64 bytes = 0;
    float scale = 1.0f;
    if (rtvs && n) {
        const u32 count = single_handle ? 1 : n;
        for (u32 i = 0; i < count && nk < 8; ++i) {
            const RtvEntry *e = rtv_get(rtvs[i].ptr);
            if (!e) continue;
            keys[nk++] = e->dkey;
            if (i == 0) {
                main_res = e->res;
                main_dkey = e->dkey;
                w = e->w;
                h = e->h;
                bytes = e->bytes;
                scale = e->scale;
            }
        }
    }
    DescKey dsv_key{};
    if (dsv && dsv->ptr) {
        const RtvEntry *e = rtv_get(dsv->ptr);
        if (e) {
            dsv_key = e->dkey;
            if (!main_res.v) {
                // Una pasada de solo profundidad (shadow map) no tiene RTs: el
                // recurso principal es el depth.
                main_res = e->res;
                main_dkey = e->dkey;
                w = e->w;
                h = e->h;
                bytes = e->bytes;
                scale = e->scale;
            }
        }
    }

    ls->cur_rts = rt_set_key(keys, nk, dsv_key);
    ls->rt_key = main_res;
    ls->rt_dkey = main_dkey;
    ls->rt_w = w;
    ls->rt_h = h;
    ls->rt_bytes = bytes;
    ls->rt_scale = scale;
    ls->cur_pass = pass_key(ls->cur_rts, ls->cur_pso, ls->ordinal);
    // skip_pass se decide UNA vez por pasada, no por draw: si cambiara a mitad
    // de la pasada quedaria medio frame dibujado.
    ls->pass_skipped = actions_ && actions_->skip_pass(ls->cur_pass, main_dkey);

    // En modo light se instrumentan las pasadas hasta agotar el presupuesto de
    // queries; en deep, todas.
    open_pass(impl_, ls, list, deep_, impl_->cfg.query_budget);
}

void Collector::cmd_set_pso(ID3D12GraphicsCommandList *list,
                            ID3D12PipelineState *pso) {
    if (!armed_) return;
    ListState *ls = Collector_find_list(impl_, list);
    if (!ls) return;
    PsoKey key{};
    {
        std::lock_guard<std::mutex> lk(impl_->tables);
        auto it = impl_->psos.find(pso);
        if (it != impl_->psos.end()) key = it->second;
    }
    ls->cur_pso = key;
    // El PSO del PRIMER draw es parte del ancla de la pasada: si la pasada
    // todavia no tuvo draws, todavia estamos a tiempo de fijarlo.
    if (ls->draws == 0) ls->cur_pass = pass_key(ls->cur_rts, key, ls->ordinal);
}

bool Collector::cmd_draw(ID3D12GraphicsCommandList *list, u32 count,
                         u32 instances) {
    if (!armed_) return true;
    ListState *ls = Collector_find_list(impl_, list);
    if (!ls) return true;
    ++ls->draws;
    if (ls->pass_skipped) return false;
    if (!deep_) return true;   // en light no se emite un evento por draw
    Event e;
    e.kind = EventKind::Draw;
    e.frame = ls->frame;
    e.flags = kEvDeepFrame;
    e.draw.pass = ls->cur_pass;
    e.draw.pso = ls->cur_pso;
    e.draw.count = count;
    e.draw.instances = instances;
    push(e);
    return true;
}

bool Collector::cmd_dispatch(ID3D12GraphicsCommandList *list, u32 x, u32 y,
                             u32 z) {
    if (!armed_) return true;
    ListState *ls = Collector_find_list(impl_, list);
    if (!ls) return true;
    if (!ls->pass_open) {
        // Pasada de compute: sin render targets, anclada al PSO.
        ls->cur_pass = pass_key(ls->cur_rts, ls->cur_pso, ls->ordinal);
        open_pass(impl_, ls, list, deep_, impl_->cfg.query_budget);
    }
    ++ls->draws;
    if (ls->pass_skipped) return false;
    if (!deep_) return true;
    Event e;
    e.kind = EventKind::Dispatch;
    e.frame = ls->frame;
    e.flags = kEvDeepFrame;
    e.draw.pass = ls->cur_pass;
    e.draw.pso = ls->cur_pso;
    e.draw.gx = x;
    e.draw.gy = y;
    e.draw.gz = z;
    push(e);
    return true;
}

u32 Collector::cmd_barrier(ID3D12GraphicsCommandList *list, u32 n,
                           const D3D12_RESOURCE_BARRIER *barriers,
                           D3D12_RESOURCE_BARRIER *out) {
    if (!armed_ || !barriers) return n;
    (void)list;
    u32 kept = 0;
    for (u32 i = 0; i < n; ++i) {
        const D3D12_RESOURCE_BARRIER &b = barriers[i];
        Event e;
        e.kind = EventKind::Barrier;
        e.frame = frame_;
        e.barrier.kind = static_cast<u32>(b.Type);
        bool drop = false;
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            e.barrier.key = ResKey{reinterpret_cast<u64>(b.Transition.pResource)};
            e.barrier.before = static_cast<u32>(b.Transition.StateBefore);
            e.barrier.after = static_cast<u32>(b.Transition.StateAfter);
            if (actions_)
                drop = actions_->drop_barrier(e.barrier.before, e.barrier.after);
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
            e.barrier.key = ResKey{reinterpret_cast<u64>(b.UAV.pResource)};
        } else {
            e.barrier.key = ResKey{reinterpret_cast<u64>(b.Aliasing.pResourceAfter)};
        }
        e.barrier.dropped = drop ? 1u : 0u;
        push(e);
        if (!drop && out) out[kept] = b;
        if (!drop) ++kept;
    }
    return out ? kept : n;
}

bool Collector::cmd_viewports(ID3D12GraphicsCommandList *list, u32 n,
                              const D3D12_VIEWPORT *in, D3D12_VIEWPORT *out) {
    if (!armed_ || !in) return false;
    ListState *ls = Collector_find_list(impl_, list);
    if (!ls) return false;
    // El gate: para escalar un RT hay que poder escalar sus viewports, y eso
    // solo se sabe si pasan por aca.
    if (actions_ && ls->rt_dkey.v) actions_->note_viewport(ls->rt_dkey);
    if (!out || ls->rt_scale >= 1.0f || ls->rt_scale <= 0.0f) return false;
    for (u32 i = 0; i < n; ++i) {
        out[i] = in[i];
        out[i].Width *= ls->rt_scale;
        out[i].Height *= ls->rt_scale;
        out[i].TopLeftX *= ls->rt_scale;
        out[i].TopLeftY *= ls->rt_scale;
    }
    return true;
}

void Collector::cmd_copy(ID3D12GraphicsCommandList *list, ID3D12Resource *dst,
                         ID3D12Resource *src) {
    if (!armed_ || !actions_) return;
    (void)list;
    // Una copia con extents fijos es la razon numero uno por la que un recurso
    // NO se puede escalar. Que quede anotado es lo que despues hace que el
    // gate lo rechace en vez de corromper el render tres minutos mas tarde.
    std::lock_guard<std::mutex> lk(impl_->tables);
    auto note = [&](ID3D12Resource *r, bool as_source) {
        if (!r) return;
        auto it = impl_->resources.find(r);
        if (it == impl_->resources.end()) return;
        actions_->note_copy(desc_key(it->second, out_), as_source);
    };
    note(src, true);
    note(dst, false);
}

void Collector::cmd_close(ID3D12GraphicsCommandList *list) {
    if (!armed_) return;
    ListState *ls = Collector_find_list(impl_, list);
    if (!ls) return;
    close_pass(impl_, ls);

    if (!ls->passes.empty()) {
        // Resolver los timestamps de ESTA lista al readback del slot. Se hace
        // una vez por lista, con un rango contiguo: es una sola copia en la
        // GPU y no frena nada.
        u32 lo = 0xFFFFFFFFu, hi = 0;
        for (const PassRecord &p : ls->passes) {
            if (p.q_begin < lo) lo = p.q_begin;
            if (p.q_end > hi) hi = p.q_end;
        }
        SlotState &slot = impl_->slots[ls->slot];
        list->ResolveQueryData(slot.heap, D3D12_QUERY_TYPE_TIMESTAMP, lo,
                               hi - lo + 1, slot.readback,
                               static_cast<UINT64>(lo) * sizeof(u64));
    }
    // Las pasadas quedan en la lista hasta ExecuteCommandLists: recien ahi se
    // sabe en QUE queue corrieron, y sin eso no se pueden comparar los ticks
    // (cada queue tiene su frecuencia y su reloj).
}

// --- ejecucion ---------------------------------------------------------

namespace {

QueueInfo *find_queue(Collector::Impl *im, ID3D12CommandQueue *q) {
    for (u32 i = 0; i < im->nqueues; ++i)
        if (im->queues[i].queue == q) return &im->queues[i];
    if (im->nqueues >= kMaxQueues) return nullptr;
    QueueInfo &qi = im->queues[im->nqueues];
    qi.queue = q;
    qi.index = im->nqueues;
    UINT64 freq = 0;
    if (FAILED(q->GetTimestampFrequency(&freq)) || freq == 0) {
        // Una queue de copia o de video puede no soportar timestamps. No es un
        // error: simplemente no se mide lo que corra ahi.
        qi.freq = 0;
    } else {
        qi.freq = freq;
    }
    if (FAILED(im->device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                       IID_PPV_ARGS(&qi.fence))))
        return nullptr;
    ++im->nqueues;
    return &qi;
}

}  // namespace

void Collector::on_execute(ID3D12CommandQueue *queue, u32 n,
                           ID3D12CommandList *const *lists) {
    if (!armed_ || !queue || !lists) return;

    std::lock_guard<std::mutex> lk(impl_->tables);
    QueueInfo *qi = find_queue(impl_, queue);
    if (!qi) return;

    u32 slot_used = impl_->cur_slot;
    bool any = false;
    for (u32 i = 0; i < n; ++i) {
        auto it = impl_->lists.find(lists[i]);
        if (it == impl_->lists.end()) continue;
        ListState *ls = it->second;
        if (ls->passes.empty()) continue;
        slot_used = ls->slot;
        SlotState &slot = impl_->slots[slot_used];
        for (PassRecord &p : ls->passes) {
            p.queue = qi->index;
            slot.passes.push_back(p);
        }
        ls->passes.clear();
        any = true;
    }
    if (!any) return;

    // Nuestra propia fence sobre la queue del juego. Es la unica forma de
    // saber cuando los timestamps de este frame estan escritos sin frenar a
    // nadie, y no cambia el orden de nada: se senaliza DESPUES de que el juego
    // ya encolo su trabajo.
    SlotState &slot = impl_->slots[slot_used];
    const u64 value = ++qi->next_value;
    if (SUCCEEDED(queue->Signal(qi->fence, value))) {
        slot.pending[qi->index] = value;
        slot.in_flight = true;
        slot.frame = frame_;
    }
}

// --- cobro de timestamps ------------------------------------------------

namespace {

bool slot_ready(Collector::Impl *im, SlotState &slot) {
    if (!slot.in_flight) return false;
    for (u32 i = 0; i < im->nqueues; ++i) {
        if (slot.pending[i] == 0) continue;
        if (im->queues[i].fence->GetCompletedValue() < slot.pending[i])
            return false;
    }
    return true;
}

void collect_slot(Collector::Impl *im, SlotState &slot, bool deep) {
    void *mapped = nullptr;
    D3D12_RANGE range{0, kMaxQueries * sizeof(u64)};
    if (FAILED(slot.readback->Map(0, &range, &mapped)) || !mapped) {
        slot.passes.clear();
        slot.next_query.store(0, std::memory_order_relaxed);
        slot.in_flight = false;
        for (u32 i = 0; i < kMaxQueues; ++i) slot.pending[i] = 0;
        return;
    }
    const u64 *ticks = static_cast<const u64 *>(mapped);

    for (const PassRecord &p : slot.passes) {
        const u64 begin = ticks[p.q_begin];
        const u64 end = ticks[p.q_end];
        if (end <= begin) continue;  // query que no se escribio: se saltea
        Event e;
        e.kind = EventKind::PassTiming;
        e.frame = p.frame;
        e.flags = deep ? kEvDeepFrame : kEvNone;
        e.pass.pass = p.key;
        e.pass.rts = p.rts;
        e.pass.first_pso = p.pso;
        e.pass.gpu_begin_ticks = begin;
        e.pass.gpu_end_ticks = end;
        e.pass.tick_freq = (p.queue < im->nqueues) ? im->queues[p.queue].freq : 0;
        e.pass.draws = p.draws;
        e.pass.ordinal = p.ordinal;
        e.pass.queue = p.queue;
        e.pass.rt_w = p.rt_w;
        e.pass.rt_h = p.rt_h;
        e.pass.rt_bytes = p.rt_bytes;
        e.pass.rt_key = p.rt_key;
        e.pass.rt_dkey = p.rt_dkey;
        push(e);

        // La tabla viva del overlay.
        const double ms = ticks_to_ms(begin, end, e.pass.tick_freq);
        std::lock_guard<std::mutex> lk(im->live_mu);
        Collector::PassLive &lv = im->live[p.key.v];
        lv.key = p.key;
        lv.rt_dkey = p.rt_dkey;
        lv.gpu_ms = lv.last_frame ? lv.gpu_ms * 0.85 + ms * 0.15 : ms;
        lv.draws = p.draws;
        lv.rt_w = p.rt_w;
        lv.rt_h = p.rt_h;
        lv.queue = p.queue;
        lv.last_frame = p.frame;
    }

    const D3D12_RANGE nothing{0, 0};
    slot.readback->Unmap(0, &nothing);
    slot.passes.clear();
    slot.next_query.store(0, std::memory_order_relaxed);
    slot.in_flight = false;
    for (u32 i = 0; i < kMaxQueues; ++i) slot.pending[i] = 0;
}

}  // namespace

void Collector::on_present(IDXGISwapChain *swapchain) {
    if (!armed_) return;

    const u64 now = now_ns();
    const u64 cpu_ns = now - impl_->last_present_ns;
    impl_->last_present_ns = now;

    {
        std::lock_guard<std::mutex> lk(impl_->live_mu);
        const double ms = static_cast<double>(cpu_ns) / 1e6;
        impl_->frame_ms_ema = impl_->frame_ms_ema
                                  ? impl_->frame_ms_ema * 0.9 + ms * 0.1 : ms;
    }

    Event fe;
    fe.kind = EventKind::FrameEnd;
    fe.frame = frame_;
    fe.flags = deep_ ? kEvDeepFrame : kEvNone;
    fe.cpu_ns = now;
    fe.frame_ev.cpu_ns = cpu_ns;
    fe.frame_ev.present_ns = 0;
    fe.frame_ev.dropped =
        static_cast<u32>(g_dropped_this_frame.exchange(0, std::memory_order_relaxed));
    fe.frame_ev.queries_used =
        impl_->slots[impl_->cur_slot].next_query.load(std::memory_order_relaxed);
    push(fe);

    // VRAM cada 30 frames: el budget del adaptador se mueve despacio y no vale
    // una linea por frame.
    if (impl_->adapter && (frame_ % 30) == 0) {
        DXGI_QUERY_VIDEO_MEMORY_INFO vm{};
        if (SUCCEEDED(impl_->adapter->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm))) {
            Event ve;
            ve.kind = EventKind::Vram;
            ve.frame = frame_;
            ve.vram.budget = vm.Budget;
            ve.vram.current_usage = vm.CurrentUsage;
            ve.vram.committed = vm.CurrentUsage;
            ve.vram.available_res = vm.CurrentReservation;
            push(ve);
            std::lock_guard<std::mutex> lk(impl_->live_mu);
            impl_->vram_budget = vm.Budget;
            impl_->vram_usage = vm.CurrentUsage;
        }
    }

    // Cobrar el slot mas viejo si su fence ya paso. Nunca se espera: si
    // todavia no esta, se intenta en el frame siguiente.
    {
        std::lock_guard<std::mutex> lk(impl_->tables);
        for (u32 i = 0; i < kSlots; ++i) {
            SlotState &s = impl_->slots[(impl_->cur_slot + 1 + i) % kSlots];
            if (slot_ready(impl_, s)) collect_slot(impl_, s, false);
        }
        impl_->cur_slot = (impl_->cur_slot + 1) % kSlots;
    }

    // Captura del harness A/B, si toca. Es lo unico de gpuprobe que frena la
    // GPU, y pasa dos veces por candidato -- no por frame. El frame en el que
    // pasa no sirve como muestra, y por eso la captura va DESPUES del
    // frame_end de este frame y antes del frame_begin del proximo.
    if (actions_ && swapchain) {
        if (const char *suffix = actions_->capture_due(frame_)) {
            ID3D12CommandQueue *q = nullptr;
            {
                std::lock_guard<std::mutex> lk(impl_->tables);
                for (u32 i = 0; i < impl_->nqueues; ++i)
                    if (impl_->queues[i].queue) { q = impl_->queues[i].queue; break; }
            }
            if (q && impl_->session_base[0]) {
                char path[MAX_PATH * 2];
                std::snprintf(path, sizeof path, "%s-%s.png", impl_->session_base,
                              suffix);
                capture_swapchain(impl_->device, q, swapchain, path);
            }
        }
    }

    ++frame_;
    deep_ = impl_->cfg.deep_every && (frame_ % impl_->cfg.deep_every) == 0;
    // El unico momento en el que las acciones cambian de estado.
    if (actions_) actions_->begin_frame(frame_);

    Event fb;
    fb.kind = EventKind::FrameBegin;
    fb.frame = frame_;
    fb.flags = deep_ ? kEvDeepFrame : kEvNone;
    push(fb);
}

void Collector::snapshot(PassLive *out, size_t max, size_t *count) const {
    *count = 0;
    if (!impl_ || !out) return;
    std::lock_guard<std::mutex> lk(impl_->live_mu);
    size_t n = 0;
    for (const auto &kv : impl_->live) {
        // Una pasada que no aparece hace 120 frames dejo de existir (el juego
        // cambio de escena): no tiene sentido seguir mostrandola.
        if (frame_ > kv.second.last_frame + 120) continue;
        if (n >= max) break;
        out[n++] = kv.second;
    }
    // Ordenada por costo, que es el orden en el que se mira.
    for (size_t i = 1; i < n; ++i) {
        PassLive tmp = out[i];
        size_t j = i;
        while (j > 0 && out[j - 1].gpu_ms < tmp.gpu_ms) { out[j] = out[j - 1]; --j; }
        out[j] = tmp;
    }
    *count = n;
}

void Collector::note_action(u64 action_id, DescKey target, u32 kind, bool on) {
    if (!armed_) return;
    Event e;
    e.kind = EventKind::ActionApplied;
    e.frame = frame_;
    e.cpu_ns = now_ns();
    e.action.action_id = action_id;
    e.action.target = target;
    e.action.kind = kind;
    e.action.enabled = on ? 1u : 0u;
    push(e);
}

double Collector::frame_ms() const {
    if (!impl_) return 0.0;
    std::lock_guard<std::mutex> lk(impl_->live_mu);
    return impl_->frame_ms_ema;
}

void Collector::vram_now(u64 *budget, u64 *usage) const {
    if (budget) *budget = 0;
    if (usage) *usage = 0;
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->live_mu);
    if (budget) *budget = impl_->vram_budget;
    if (usage) *usage = impl_->vram_usage;
}

}  // namespace gp
