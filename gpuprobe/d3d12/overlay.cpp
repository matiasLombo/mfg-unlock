// overlay.cpp -- ver overlay.h.
//
// Lo que muestra, y por que ese y no otro:
//   - la tabla de pasadas ordenada por ms, que es la pregunta "a donde se va
//     el frame" contestada en vivo;
//   - un toggle por candidato del perfil, con el motivo cuando el gate lo
//     rechaza (un candidato que no se aplica sin decir por que es peor que uno
//     que no existe);
//   - el frametime y el delta desde el ultimo toggle, para ver al instante si
//     algo se movio;
//   - la VRAM contra el budget, porque si eso esta en rojo todo lo demas de la
//     pantalla es consecuencia y no causa.
//
// El overlay dibuja en el hilo de presentacion, entre el ultimo trabajo del
// juego y el Present. Usa su propia command list y su propio descriptor heap:
// no toca ni un byte del estado del juego.
#define WIN32_LEAN_AND_MEAN
#include "overlay.h"

#include <windows.h>

#if __has_include(<imgui.h>)

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <cstdio>
#include <vector>

#include "collector.h"
#include "executor.h"
#include "gate.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM,
                                                             LPARAM);

namespace gp {

namespace {

struct FrameCtx {
    ID3D12CommandAllocator *alloc = nullptr;
};

bool g_ready = false;
bool g_failed = false;
bool g_visible = true;
HWND g_hwnd = nullptr;
WNDPROC g_prev_wndproc = nullptr;
ID3D12DescriptorHeap *g_srv = nullptr;
ID3D12DescriptorHeap *g_rtv = nullptr;
ID3D12GraphicsCommandList *g_list = nullptr;
std::vector<FrameCtx> g_frames;
std::vector<ID3D12Resource *> g_backbuffers;
UINT g_rtv_step = 0;
double g_last_toggle_frame_ms = 0.0;
double g_delta_ms = 0.0;

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (g_ready && g_visible && ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l))
        return 1;
    return CallWindowProcW(g_prev_wndproc, hwnd, msg, w, l);
}

void release_backbuffers() {
    for (ID3D12Resource *r : g_backbuffers)
        if (r) r->Release();
    g_backbuffers.clear();
}

bool init(ID3D12Device *device, IDXGISwapChain *swapchain) {
    DXGI_SWAP_CHAIN_DESC sd{};
    if (FAILED(swapchain->GetDesc(&sd))) return false;
    g_hwnd = sd.OutputWindow;
    if (!g_hwnd) return false;
    const UINT count = sd.BufferCount ? sd.BufferCount : 2;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srv)))) return false;

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = count;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv)))) return false;
    g_rtv_step = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < count; ++i) {
        ID3D12Resource *buf = nullptr;
        if (FAILED(swapchain->GetBuffer(i, IID_PPV_ARGS(&buf))) || !buf) return false;
        device->CreateRenderTargetView(buf, nullptr, rtv);
        g_backbuffers.push_back(buf);
        rtv.ptr += g_rtv_step;

        FrameCtx fc;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&fc.alloc))))
            return false;
        g_frames.push_back(fc);
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         g_frames[0].alloc, nullptr,
                                         IID_PPV_ARGS(&g_list))))
        return false;
    g_list->Close();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;   // nada de archivos al lado del juego
    if (!ImGui_ImplWin32_Init(g_hwnd)) return false;

#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19100
    // ImGui 1.91 cambio la firma a una struct de init.
    ImGui_ImplDX12_InitInfo info{};
    info.Device = device;
    info.CommandQueue = nullptr;
    info.NumFramesInFlight = static_cast<int>(count);
    info.RTVFormat = sd.BufferDesc.Format;
    info.SrvDescriptorHeap = g_srv;
    info.LegacySingleSrvCpuDescriptor = g_srv->GetCPUDescriptorHandleForHeapStart();
    info.LegacySingleSrvGpuDescriptor = g_srv->GetGPUDescriptorHandleForHeapStart();
    if (!ImGui_ImplDX12_Init(&info)) return false;
#else
    if (!ImGui_ImplDX12_Init(device, static_cast<int>(count), sd.BufferDesc.Format,
                             g_srv, g_srv->GetCPUDescriptorHandleForHeapStart(),
                             g_srv->GetGPUDescriptorHandleForHeapStart()))
        return false;
#endif

    g_prev_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(&wndproc)));
    gate_log("overlay listo (%u backbuffers, ventana %p)", count,
             static_cast<void *>(g_hwnd));
    return true;
}

const char *fmt_mb(u64 bytes, char *buf, size_t cap) {
    std::snprintf(buf, cap, "%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

void draw_ui() {
    Collector &col = collector();
    Executor &ex = executor();

    ImGui::SetNextWindowSize(ImVec2(620, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("gpuprobe  (F10 para ocultar)")) {
        ImGui::End();
        return;
    }

    const double frame_ms = col.frame_ms();
    ImGui::Text("frame %.2f ms (%.0f fps)   delta desde el ultimo toggle: %+.2f ms",
                frame_ms, frame_ms > 0 ? 1000.0 / frame_ms : 0.0, g_delta_ms);

    u64 budget = 0, usage = 0;
    col.vram_now(&budget, &usage);
    if (budget) {
        char a[32], b[32];
        const double pressure = static_cast<double>(usage) /
                                static_cast<double>(budget);
        // Arriba del budget todo lo demas de esta pantalla es consecuencia del
        // thrashing y no causa, asi que se dice fuerte.
        if (pressure > 1.0)
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                               "VRAM %s / %s  (%.0f%%) -- POR ENCIMA DEL BUDGET: "
                               "los ms de abajo miden paginacion",
                               fmt_mb(usage, a, sizeof a), fmt_mb(budget, b, sizeof b),
                               pressure * 100.0);
        else
            ImGui::Text("VRAM %s / %s  (%.0f%%)", fmt_mb(usage, a, sizeof a),
                        fmt_mb(budget, b, sizeof b), pressure * 100.0);
    }

    ImGui::Separator();
    ImGui::Text("Pasadas (ordenadas por costo)");
    if (ImGui::BeginChild("pasadas", ImVec2(0, 220), true)) {
        ImGui::Columns(5, "cols");
        ImGui::Text("pasada"); ImGui::NextColumn();
        ImGui::Text("ms"); ImGui::NextColumn();
        ImGui::Text("draws"); ImGui::NextColumn();
        ImGui::Text("RT"); ImGui::NextColumn();
        ImGui::Text("queue"); ImGui::NextColumn();
        ImGui::Separator();
        Collector::PassLive rows[64];
        size_t n = 0;
        col.snapshot(rows, 64, &n);
        for (size_t i = 0; i < n; ++i) {
            ImGui::Text("%016llx", static_cast<unsigned long long>(rows[i].key.v));
            ImGui::NextColumn();
            ImGui::Text("%.3f", rows[i].gpu_ms); ImGui::NextColumn();
            ImGui::Text("%u", rows[i].draws); ImGui::NextColumn();
            ImGui::Text("%ux%u", rows[i].rt_w, rows[i].rt_h); ImGui::NextColumn();
            ImGui::Text("%u", rows[i].queue); ImGui::NextColumn();
        }
        ImGui::Columns(1);
    }
    ImGui::EndChild();

    ImGui::Separator();
    const Profile &prof = ex.core().profile();
    if (prof.actions.empty()) {
        ImGui::TextDisabled("Sin perfil. Corre el analizador sobre una sesion y "
                            "guarda el TOML en %%LOCALAPPDATA%%\\gpuprobe\\profiles.");
    } else {
        ImGui::Text("Candidatos%s", prof.observe_only ? "  (perfil en modo "
                                                        "observacion: no se aplica "
                                                        "nada)" : "");
        for (const Action &a : prof.actions) {
            bool on = ex.core().is_on(a.id);
            char label[160];
            std::snprintf(label, sizeof label, "%s##%llu",
                          a.name.empty() ? action_kind_name(a.kind) : a.name.c_str(),
                          static_cast<unsigned long long>(a.id));
            if (ImGui::Checkbox(label, &on)) {
                ex.force(a.id, on);
                g_last_toggle_frame_ms = frame_ms;
                g_delta_ms = 0.0;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", action_kind_name(a.kind));
        }
    }

    if (!ex.last_error().empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", ex.last_error().c_str());
    }
    ImGui::TextDisabled("perfil: %s  (recargas: %llu)", ex.profile_path().c_str(),
                        static_cast<unsigned long long>(ex.reloads()));
    ImGui::End();
}

}  // namespace

bool overlay_toggle() {
    g_visible = !g_visible;
    return g_visible;
}

bool overlay_visible() { return g_visible; }

bool overlay_draw(ID3D12Device *device, ID3D12CommandQueue *queue,
                  IDXGISwapChain *swapchain) {
    if (g_failed || !device || !queue || !swapchain) return false;
    if (!g_ready) {
        if (!init(device, swapchain)) {
            // Un overlay que no arranca no puede tumbar al juego: se apaga y
            // se sigue midiendo, que es lo que importa.
            g_failed = true;
            gate_log("el overlay no arranco: se sigue sin el");
            overlay_shutdown();
            return false;
        }
        g_ready = true;
    }
    if (!g_visible) return true;

    // El delta contra el frametime que habia cuando se toco el ultimo toggle.
    if (g_last_toggle_frame_ms > 0.0)
        g_delta_ms = collector().frame_ms() - g_last_toggle_frame_ms;

    UINT index = 0;
    {
        IDXGISwapChain3 *sc3 = nullptr;
        if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
            index = sc3->GetCurrentBackBufferIndex();
            sc3->Release();
        }
    }
    if (index >= g_frames.size()) return false;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_ui();
    ImGui::Render();

    FrameCtx &fc = g_frames[index];
    fc.alloc->Reset();
    g_list->Reset(fc.alloc, nullptr);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_backbuffers[index];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_list->ResourceBarrier(1, &b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(index) * g_rtv_step;
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap *heaps[] = {g_srv};
    g_list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_list);

    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_list->ResourceBarrier(1, &b);
    g_list->Close();
    ID3D12CommandList *lists[] = {g_list};
    queue->ExecuteCommandLists(1, lists);
    return true;
}

void overlay_shutdown() {
    if (g_ready) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_ready = false;
    }
    if (g_hwnd && g_prev_wndproc) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_prev_wndproc));
        g_prev_wndproc = nullptr;
    }
    release_backbuffers();
    for (FrameCtx &fc : g_frames)
        if (fc.alloc) fc.alloc->Release();
    g_frames.clear();
    if (g_list) { g_list->Release(); g_list = nullptr; }
    if (g_srv) { g_srv->Release(); g_srv = nullptr; }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

}  // namespace gp

#else  // sin ImGui

namespace gp {
bool overlay_draw(ID3D12Device *, ID3D12CommandQueue *, IDXGISwapChain *) {
    return false;
}
bool overlay_toggle() { return false; }
bool overlay_visible() { return false; }
void overlay_shutdown() {}
}  // namespace gp

#endif
