// capture.cpp -- ver capture.h.
#define WIN32_LEAN_AND_MEAN
#include "capture.h"

#include <windows.h>

#include <vector>

#include "core/png.h"
#include "gate.h"

namespace gp {

namespace {

// Un ComPtr de tres lineas: esto corre una vez cada varios miles de frames y
// no justifica traer nada.
template <class T>
struct Ref {
    T *p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T **operator&() { return &p; }
    T *operator->() const { return p; }
    operator T *() const { return p; }
};

// El backbuffer puede ser BGRA o RGBA, y con o sin sRGB. Lo unico que cambia
// para nosotros es el orden de los canales.
bool is_bgra(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

bool is_supported(DXGI_FORMAT f) {
    return is_bgra(f) || f == DXGI_FORMAT_R8G8B8A8_UNORM ||
           f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

}  // namespace

bool capture_swapchain(ID3D12Device *device, ID3D12CommandQueue *queue,
                       IDXGISwapChain *swapchain, const char *path) {
    if (!device || !queue || !swapchain || !path) return false;

    Ref<ID3D12Resource> back;
    UINT index = 0;
    {
        IDXGISwapChain3 *sc3 = nullptr;
        if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
            index = sc3->GetCurrentBackBufferIndex();
            sc3->Release();
        }
    }
    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&back))) || !back.p)
        return false;

    const D3D12_RESOURCE_DESC desc = back->GetDesc();
    if (!is_supported(desc.Format)) {
        // HDR (R10G10B10A2, RGBA16F) necesita un tonemap para verse bien en un
        // PNG, y un tonemap inventado por nosotros mostraria una imagen que el
        // juego nunca dibujo. Preferimos no capturar y decirlo.
        gate_log("captura salteada: formato %u sin camino honesto a PNG",
                 static_cast<unsigned>(desc.Format));
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0, row_bytes = 0;
    UINT rows = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row_bytes, &total);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = total;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Ref<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback))))
        return false;

    Ref<ID3D12CommandAllocator> alloc;
    Ref<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&alloc))))
        return false;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.p,
                                         nullptr, IID_PPV_ARGS(&list))))
        return false;

    // El backbuffer esta en PRESENT cuando el juego ya presento. Se lo lleva a
    // COPY_SOURCE y se lo devuelve, para no dejarle al juego un estado que no
    // es el que el cree tener.
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = back.p;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = back.p;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    dst.pResource = readback.p;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    if (FAILED(list->Close())) return false;

    ID3D12CommandList *lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);

    Ref<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return false;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) return false;
    queue->Signal(fence.p, 1);
    bool ok = true;
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, ev);
        // Este es el unico lugar de todo gpuprobe donde se espera a la GPU, y
        // pasa dos veces por candidato del harness, no por frame.
        ok = WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0;
    }
    CloseHandle(ev);
    if (!ok) {
        gate_log("captura: la GPU no termino la copia en 5 s");
        return false;
    }

    void *mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    if (FAILED(readback->Map(0, &range, &mapped)) || !mapped) return false;

    const u32 w = static_cast<u32>(desc.Width);
    const u32 h = desc.Height;
    std::vector<u8> rgba(static_cast<size_t>(w) * h * 4);
    const u8 *base = static_cast<const u8 *>(mapped);
    const bool bgra = is_bgra(desc.Format);
    for (u32 y = 0; y < h; ++y) {
        const u8 *srow = base + static_cast<size_t>(y) * fp.Footprint.RowPitch;
        u8 *drow = &rgba[static_cast<size_t>(y) * w * 4];
        for (u32 x = 0; x < w; ++x) {
            const u8 *s = srow + static_cast<size_t>(x) * 4;
            u8 *d = drow + static_cast<size_t>(x) * 4;
            d[0] = bgra ? s[2] : s[0];
            d[1] = s[1];
            d[2] = bgra ? s[0] : s[2];
            d[3] = 255;   // el alfa del backbuffer no significa nada
        }
    }
    const D3D12_RANGE nothing{0, 0};
    readback->Unmap(0, &nothing);

    const bool written = png_write(path, rgba.data(), w, h);
    gate_log("captura %s: %ux%u %s", path, w, h, written ? "ok" : "FALLO al escribir");
    return written;
}

}  // namespace gp
