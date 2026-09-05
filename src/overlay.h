#pragma once
//
// A panel drawn straight onto the game's swap chain, toggled with `.
//
// Deliberately small: solid-coloured quads and nothing else. No texture, no
// sampler, no font atlas -- glyphs come from a 5x7 bitmap expanded to one quad
// per lit pixel on the cpu, which is a few thousand vertices for a panel this
// size and saves the whole descriptor-heap-and-upload path a font would need.
//
// It also takes no window messages. An earlier overlay in a sibling project
// subclassed WndProc and crashed on a null pointer between the read and the
// call; this one polls GetAsyncKeyState from the thread that already polls F9,
// so there is no race to lose and nothing to unhook on the way out. The cost
// is that it cannot swallow input from the game, which is why the panel is
// driven by function keys the game does not use rather than by the mouse.

#include <d3d12.h>
#include <dxgi1_4.h>

// ---- 5x7 font, five columns per glyph, low bit is the top row -------------
struct Glyph { char c; unsigned char col[5]; };
static const Glyph kFont[] = {
    {' ',{0x00,0x00,0x00,0x00,0x00}}, {'0',{0x3E,0x51,0x49,0x45,0x3E}},
    {'1',{0x00,0x42,0x7F,0x40,0x00}}, {'2',{0x42,0x61,0x51,0x49,0x46}},
    {'3',{0x21,0x41,0x45,0x4B,0x31}}, {'4',{0x18,0x14,0x12,0x7F,0x10}},
    {'5',{0x27,0x45,0x45,0x45,0x39}}, {'6',{0x3C,0x4A,0x49,0x49,0x30}},
    {'7',{0x01,0x71,0x09,0x05,0x03}}, {'8',{0x36,0x49,0x49,0x49,0x36}},
    {'9',{0x06,0x49,0x49,0x29,0x1E}}, {'A',{0x7E,0x11,0x11,0x11,0x7E}},
    {'B',{0x7F,0x49,0x49,0x49,0x36}}, {'C',{0x3E,0x41,0x41,0x41,0x22}},
    {'D',{0x7F,0x41,0x41,0x22,0x1C}}, {'E',{0x7F,0x49,0x49,0x49,0x41}},
    {'F',{0x7F,0x09,0x09,0x09,0x01}}, {'G',{0x3E,0x41,0x49,0x49,0x7A}},
    {'H',{0x7F,0x08,0x08,0x08,0x7F}}, {'I',{0x00,0x41,0x7F,0x41,0x00}},
    {'K',{0x7F,0x08,0x14,0x22,0x41}}, {'L',{0x7F,0x40,0x40,0x40,0x40}},
    {'M',{0x7F,0x02,0x0C,0x02,0x7F}}, {'N',{0x7F,0x04,0x08,0x10,0x7F}},
    {'O',{0x3E,0x41,0x41,0x41,0x3E}}, {'P',{0x7F,0x09,0x09,0x09,0x06}},
    {'R',{0x7F,0x09,0x19,0x29,0x46}}, {'S',{0x46,0x49,0x49,0x49,0x31}},
    {'T',{0x01,0x01,0x7F,0x01,0x01}}, {'U',{0x3F,0x40,0x40,0x40,0x3F}},
    {'V',{0x1F,0x20,0x40,0x20,0x1F}}, {'X',{0x63,0x14,0x08,0x14,0x63}},
    {'Y',{0x07,0x08,0x70,0x08,0x07}}, {'-',{0x08,0x08,0x08,0x08,0x08}},
    {':',{0x00,0x36,0x36,0x00,0x00}}, {'.',{0x00,0x60,0x60,0x00,0x00}},
};

struct OvVert { float x, y; float r, g, b, a; };

static const int kOvMaxVerts = 24000;

static ID3D12Device        *g_ov_dev   = nullptr;
static ID3D12CommandQueue  *g_ov_queue = nullptr;
// Three allocators in rotation. One allocator plus a blocking fence wait was
// the first shape, and it was wrong twice over: the wait had a timeout, so a
// slow frame let it reset an allocator the gpu was still reading, and it
// blocked inside Present on the game's own queue while DLSS-G was generating
// frames on it. Now nothing waits -- if a slot is still in flight the panel
// simply skips that frame.
static const int kOvSlots = 3;
static ID3D12CommandAllocator *g_ov_alloc_ring[kOvSlots] = { nullptr, nullptr, nullptr };
static UINT64 g_ov_slot_fence[kOvSlots] = { 0, 0, 0 };
static int g_ov_slot = 0;
static ID3D12CommandAllocator *g_ov_alloc = nullptr;
static ID3D12GraphicsCommandList *g_ov_list = nullptr;
static ID3D12RootSignature *g_ov_root = nullptr;
static ID3D12PipelineState *g_ov_pso  = nullptr;
static ID3D12DescriptorHeap *g_ov_rtvheap = nullptr;
static ID3D12Resource      *g_ov_vb   = nullptr;
static OvVert              *g_ov_map  = nullptr;
static ID3D12Fence         *g_ov_fence = nullptr;
static HANDLE               g_ov_event = nullptr;
static UINT64               g_ov_fenceval = 0;
static bool                 g_ov_ready = false;
static bool                 g_ov_failed = false;
static int                  g_ov_verts = 0;

static int g_ov_trace = 1;
bool g_ov_enabled = false;
bool g_ov_visible = false;                 // toggled with `

// ---- input ---------------------------------------------------------------
//
// The panel has to swallow input while it is open, and that needs a window
// procedure -- there is no other way to stop a click reaching the game. The
// last overlay I wrote subclassed one and crashed on a null pointer read
// between testing the saved procedure and calling it, so the saved pointer is
// read once into a local here and DefWindowProcW stands in if it is null.
// Nothing else in this file depends on messages: the pointer position and the
// click come from polling, so a message that never arrives cannot wedge the
// panel.

static HWND     g_ov_hwnd = nullptr;
static WNDPROC  g_ov_prevproc = nullptr;
static float    g_ov_mx = 0.0f, g_ov_my = 0.0f;
static int      g_ov_hot = -1;             // row under the pointer, -1 = none

static LRESULT CALLBACK ov_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const WNDPROC prev = g_ov_prevproc;    // read once, then only use `prev`
    if (g_ov_visible) {
        switch (m) {
        case WM_MOUSEMOVE:   case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:   case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:   case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        case WM_KEYDOWN:     case WM_KEYUP:       case WM_CHAR:
        case WM_SYSKEYDOWN:  case WM_SYSKEYUP:    case WM_INPUT:
            return 0;                      // eaten: the game sees nothing
        case WM_SETCURSOR:
            SetCursor(nullptr);            // we draw our own pointer
            return 1;
        default: break;
        }
    }
    return prev != nullptr ? CallWindowProcW(prev, h, m, w, l)
                           : DefWindowProcW(h, m, w, l);
}

// Separately switchable from the panel itself. With DLSS-G on, Present runs
// on Streamline's own thread while this window procedure runs on the game's,
// and it stays installed for as long as the panel is *enabled* -- not only
// while it is open. That makes it the one piece of the interface still live
// during a crash that happens with the panel closed.
bool g_ov_hook_window = true;

static void ov_attach_window(HWND h) {
    if (!g_ov_hook_window) return;
    // Only when the panel is switched on. This ran unconditionally, so even a
    // build with the overlay disabled still replaced the game's window
    // procedure -- which left nothing truly isolated to compare against.
    if (!g_ov_enabled || h == nullptr || g_ov_hwnd != nullptr) return;
    g_ov_hwnd = h;
    g_ov_prevproc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)&ov_wndproc);
}



// Which set of objects to use, switchable at run time because guessing has
// cost four crashes. The game builds its device through sl.interposer's own
// D3D12CreateDevice, so device, queue and swap-chain buffers are all
// Streamline proxies; mixing an unwrapped queue with proxy back buffers is
// what DXGI_ERROR_ACCESS_DENIED right after ExecuteCommandLists looks like.
// Either everything is unwrapped or nothing is -- F6 flips between the two.
static bool g_ov_own_queue = false;
static IDXGISwapChain *g_ov_sc = nullptr;   // the one everything was built for
static IUnknown *g_ov_qsrc = nullptr;      // exactly what the swap chain got

static void ov_release(void) {
    // Wait for the gpu first. Everything below is memory the gpu may still be
    // reading -- command allocators, the vertex buffer -- and releasing it
    // while a submission is in flight hands the driver back pages that are
    // still being fetched. That is what DXGI_ERROR_ACCESS_DENIED was after
    // the swap-chain rebuild: seven open-and-close cycles that never rebuilt
    // were fine, and the first one that did died on its next submission.
    if (g_ov_queue != nullptr && g_ov_fence != nullptr && g_ov_event != nullptr) {
        const UINT64 v = ++g_ov_fenceval;
        if (SUCCEEDED(g_ov_queue->Signal(g_ov_fence, v)) &&
            g_ov_fence->GetCompletedValue() < v &&
            SUCCEEDED(g_ov_fence->SetEventOnCompletion(v, g_ov_event)))
            WaitForSingleObject(g_ov_event, 1000);
    }
    if (g_ov_vb != nullptr && g_ov_map != nullptr) { g_ov_vb->Unmap(0, nullptr); g_ov_map = nullptr; }
    if (g_ov_vb) { g_ov_vb->Release(); g_ov_vb = nullptr; }
    if (g_ov_fence) { g_ov_fence->Release(); g_ov_fence = nullptr; }
    if (g_ov_event) { CloseHandle(g_ov_event); g_ov_event = nullptr; }
    if (g_ov_rtvheap) { g_ov_rtvheap->Release(); g_ov_rtvheap = nullptr; }
    if (g_ov_list) { g_ov_list->Release(); g_ov_list = nullptr; }
    for (int i = 0; i < kOvSlots; ++i)
        if (g_ov_alloc_ring[i]) { g_ov_alloc_ring[i]->Release(); g_ov_alloc_ring[i] = nullptr; }
    g_ov_alloc = nullptr;
    if (g_ov_pso) { g_ov_pso->Release(); g_ov_pso = nullptr; }
    if (g_ov_root) { g_ov_root->Release(); g_ov_root = nullptr; }
    if (g_ov_queue) { g_ov_queue->Release(); g_ov_queue = nullptr; }
    g_ov_own_queue = false;
    if (g_ov_dev) { g_ov_dev->Release(); g_ov_dev = nullptr; }
    for (int i = 0; i < kOvSlots; ++i) g_ov_slot_fence[i] = 0;
    g_ov_slot = 0; g_ov_fenceval = 0;
    g_ov_sc = nullptr;
    g_ov_ready = false; g_ov_failed = false;
}

// Hooking the swap chain "behind" the proxy was tried and removed. NVIDIA's
// own guide says that with DLSS-G the host has no direct access to the swap
// chain buffers and must take them from the proxy -- so the real chain was
// never the right target, and calling slGetNativeInterface on a swap chain
// from inside CreateSwapChainForHwnd was a call into Streamline at a moment
// it had no reason to expect, made on every launch whether the panel was used
// or not.

static void ov_release(void);

static void ov_capture_queue(IUnknown *dev) {
    if (dev == nullptr) return;
    // Every swap chain, not just the first. The game builds a new one part way
    // through -- the pointer in the log jumps from one value to another and the
    // next submission is denied -- and this used to keep the queue from the
    // first one forever. Rebuilding against a stale queue is the same mismatch
    // as never rebuilding: the buffers belong to the new chain and the queue to
    // the old.
    if (g_ov_qsrc != dev) {
        g_ov_qsrc = dev;
        if (g_ov_queue != nullptr || g_ov_ready) {
            log_line("overlay: new swap chain queue, rebuilding");
            ov_release();
        }
    }
    if (g_ov_queue != nullptr) return;
    // In D3D12 the first argument to CreateSwapChainForHwnd is the command
    // queue presents are made on. That is the one object an overlay cannot
    // obtain from the swap chain afterwards, and it arrives here for free.
    //
    // The queue exactly as the game handed it to the swap chain. NVIDIA's
    // guide is explicit that with DLSS-G the host has no direct access to
    // the swap-chain buffers and must go through Streamline's proxy, so
    // unwrapping the queue, the device or the swap chain was wrong by
    // design. All three were tried and every one was denied.
    IUnknown *use = dev;
    ID3D12CommandQueue *q = nullptr;
    if (SUCCEEDED(use->QueryInterface(__uuidof(ID3D12CommandQueue), (void **)&q)) && q != nullptr)
        g_ov_queue = q;
}

// ---- geometry ------------------------------------------------------------
static float g_ov_w = 1.0f, g_ov_h = 1.0f;

static void ov_rect(float x, float y, float w, float h,
                    float r, float g, float b, float a) {
    if (g_ov_verts + 6 > kOvMaxVerts || g_ov_map == nullptr) return;
    // Pixels to normalised device coordinates, done here so the shader needs
    // no constants and the root signature stays empty.
    const float x0 =  (x / g_ov_w) * 2.0f - 1.0f;
    const float x1 =  ((x + w) / g_ov_w) * 2.0f - 1.0f;
    const float y0 = 1.0f - (y / g_ov_h) * 2.0f;
    const float y1 = 1.0f - ((y + h) / g_ov_h) * 2.0f;
    OvVert *v = g_ov_map + g_ov_verts;
    const OvVert a0 = { x0, y0, r, g, b, a }, b0 = { x1, y0, r, g, b, a };
    const OvVert c0 = { x0, y1, r, g, b, a }, d0 = { x1, y1, r, g, b, a };
    v[0] = a0; v[1] = b0; v[2] = c0; v[3] = b0; v[4] = d0; v[5] = c0;
    g_ov_verts += 6;
}

static void ov_text(const char *s, float x, float y, float px,
                    float r, float g, float b) {
    for (const char *p = s; *p != 0; ++p) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        const Glyph *gl = nullptr;
        for (unsigned i = 0; i < sizeof(kFont) / sizeof(kFont[0]); ++i)
            if (kFont[i].c == c) { gl = &kFont[i]; break; }
        if (gl != nullptr) {
            for (int cx = 0; cx < 5; ++cx)
                for (int cy = 0; cy < 7; ++cy)
                    if ((gl->col[cx] >> cy) & 1)
                        ov_rect(x + cx * px, y + cy * px, px, px, r, g, b, 1.0f);
        }
        x += 6 * px;
    }
}

// ---- one-time device setup ----------------------------------------------
//
// Shaders are compiled at run time through d3dcompiler_47.dll, resolved with
// LoadLibrary rather than linked, so the build still needs nothing but
// kernel32. The vertex format is position-in-NDC plus colour, so the root
// signature has no parameters at all and there is nothing to bind per frame.

typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void *,
        void *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
typedef HRESULT (WINAPI *PFN_SerializeRS)(const D3D12_ROOT_SIGNATURE_DESC *,
        D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);

static const char kOvShader[] =
    "struct VSIn  { float2 p : POSITION; float4 c : COLOR; };\n"
    "struct VSOut { float4 p : SV_POSITION; float4 c : COLOR; };\n"
    "VSOut VSMain(VSIn i){ VSOut o; o.p = float4(i.p, 0, 1); o.c = i.c; return o; }\n"
    "float4 PSMain(VSOut i) : SV_TARGET { return i.c; }\n";

static bool ov_init(IDXGISwapChain *sc) {
    if (g_ov_ready) return true;
    if (g_ov_failed || sc == nullptr) return false;
    g_ov_failed = true;                       // any early return below is fatal

    // The queue the game handed to CreateSwapChainForHwnd, exactly as given.
    // NVIDIA's guide is explicit that with DLSS-G the host has no direct
    // access to the swap-chain buffers and must go through the proxy, so
    // unwrapping the queue, the device or the swap chain was wrong by design
    // -- all three were tried and every one was denied.
    if (g_ov_queue == nullptr) ov_capture_queue(g_ov_qsrc);
    if (g_ov_queue == nullptr) return false;
    if (FAILED(g_ov_queue->GetDevice(__uuidof(ID3D12Device), (void **)&g_ov_dev)) ||
        g_ov_dev == nullptr)
        return false;

    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    if (dc == nullptr) return false;
    PFN_D3DCompile compile = (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile");
    HMODULE d12 = GetModuleHandleA("d3d12.dll");
    PFN_SerializeRS serialize = d12 ? (PFN_SerializeRS)GetProcAddress(
            d12, "D3D12SerializeRootSignature") : nullptr;
    if (compile == nullptr || serialize == nullptr) return false;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob *rsb = nullptr, *err = nullptr;
    if (FAILED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err)) || rsb == nullptr)
        return false;
    if (FAILED(g_ov_dev->CreateRootSignature(0, rsb->GetBufferPointer(),
            rsb->GetBufferSize(), __uuidof(ID3D12RootSignature), (void **)&g_ov_root)))
        return false;

    ID3DBlob *vs = nullptr, *ps = nullptr;
    if (FAILED(compile(kOvShader, sizeof(kOvShader) - 1, nullptr, nullptr, nullptr,
                       "VSMain", "vs_5_0", 0, 0, &vs, &err)) || vs == nullptr) return false;
    if (FAILED(compile(kOvShader, sizeof(kOvShader) - 1, nullptr, nullptr, nullptr,
                       "PSMain", "ps_5_0", 0, 0, &ps, &err)) || ps == nullptr) return false;

    D3D12_INPUT_ELEMENT_DESC il[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    DXGI_SWAP_CHAIN_DESC scd{};
    sc->GetDesc(&scd);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_ov_root;
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout = { il, 2 };
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = scd.BufferDesc.Format;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_ov_dev->CreateGraphicsPipelineState(&pd,
            __uuidof(ID3D12PipelineState), (void **)&g_ov_pso)))
        return false;

    for (int i = 0; i < kOvSlots; ++i)
        if (FAILED(g_ov_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                __uuidof(ID3D12CommandAllocator), (void **)&g_ov_alloc_ring[i])))
            return false;
    g_ov_alloc = g_ov_alloc_ring[0];
    if (FAILED(g_ov_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            g_ov_alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&g_ov_list)))
        return false;
    g_ov_list->Close();

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;                    // rebuilt every frame, never stale
    if (FAILED(g_ov_dev->CreateDescriptorHeap(&hd,
            __uuidof(ID3D12DescriptorHeap), (void **)&g_ov_rtvheap)))
        return false;

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = sizeof(OvVert) * kOvMaxVerts;
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_ov_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            __uuidof(ID3D12Resource), (void **)&g_ov_vb)))
        return false;
    D3D12_RANGE none{ 0, 0 };
    if (FAILED(g_ov_vb->Map(0, &none, (void **)&g_ov_map))) return false;

    if (FAILED(g_ov_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            __uuidof(ID3D12Fence), (void **)&g_ov_fence)))
        return false;
    g_ov_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_ov_event == nullptr) return false;

    g_ov_failed = false;
    g_ov_ready = true;
    g_ov_sc = sc;
    return true;
}

// ---- the panel -----------------------------------------------------------
//
// Reads two globals owned by the override in proxy.cpp: what the game last
// asked for, and what we are forcing. Both are frames *generated*, so the
// multiplier shown is that plus one.

static const float kPanX = 40.0f, kPanY = 40.0f, kPanW = 296.0f, kPanH = 236.0f;
static const int   kPanRows = 5;
static const float kRowTop = 96.0f, kRowH = 20.0f;

// One place for the row rectangle, used by both the drawing and the hit test
// so they cannot drift apart.
static void ov_row_rect(int i, float *rx, float *ry, float *rw, float *rh) {
    *rx = kPanX + 10.0f;
    *ry = kPanY + kRowTop + i * kRowH;
    *rw = kPanW - 20.0f;
    *rh = kRowH - 2.0f;
}

// Pointer position in client pixels, polled rather than taken from messages:
// the game may be using raw input, and a panel that depends on WM_MOUSEMOVE
// arriving would simply stop responding.
static void ov_update_pointer(void) {
    g_ov_hot = -1;
    POINT p;
    if (g_ov_hwnd == nullptr || !GetCursorPos(&p) || !ScreenToClient(g_ov_hwnd, &p))
        return;
    g_ov_mx = (float)p.x;
    g_ov_my = (float)p.y;
    for (int i = 0; i < kPanRows; ++i) {
        float rx, ry, rw, rh;
        ov_row_rect(i, &rx, &ry, &rw, &rh);
        if (g_ov_mx >= rx && g_ov_mx < rx + rw && g_ov_my >= ry && g_ov_my < ry + rh) {
            g_ov_hot = i;
            break;
        }
    }
}

// Selection, shared with the override in proxy.cpp:
//   0 AUTO -- leave whatever the game asked for
//   1 OFF  -- DLSSGMode::eOff
//   2..4   -- eOn with 1, 2 or 3 generated frames, i.e. 2x, 3x, 4x
static const char *kRowName[kPanRows] = { "AUTO", "OFF", "2X", "3X", "4X" };

// Right-aligns a short value at a column, so the two readouts line up
// instead of drifting with the label length.
static void ov_text_right(const char *s, float right, float y, float px,
                          float r, float g, float b) {
    int n = 0;
    for (const char *q = s; *q != 0; ++q) ++n;
    ov_text(s, right - n * 6.0f * px, y, px, r, g, b);
}

static void ov_build_panel(void) {
    g_ov_verts = 0;
    const float px = 2.0f;
    const float x = kPanX, y = kPanY, w = kPanW, h = kPanH;
    const float valueRight = x + w - 16.0f;

    ov_rect(x, y, w, h, 0.05f, 0.06f, 0.07f, 0.90f);
    ov_rect(x, y, w, 3.0f, 0.35f, 0.78f, 0.35f, 1.0f);
    ov_text("MFG UNLOCK", x + 14, y + 14, px, 0.45f, 0.92f, 0.45f);

    const int asked = (int)g_last_seen_generated;
    const int sel   = (int)g_force_sel;

    char v[8];
    v[0] = (char)('0' + (asked + 1 > 9 ? 9 : asked + 1)); v[1] = 'X'; v[2] = 0;
    ov_text("GAME ASKS", x + 14, y + 40, px, 0.62f, 0.62f, 0.66f);
    ov_text_right(v, valueRight, y + 40, px, 0.78f, 0.78f, 0.82f);

    ov_text("OVERRIDE", x + 14, y + 60, px, 0.62f, 0.62f, 0.66f);
    ov_text_right(kRowName[sel], valueRight, y + 60, px,
                  sel > 0 ? 0.45f : 0.62f,
                  sel > 0 ? 0.95f : 0.62f,
                  sel > 0 ? 0.45f : 0.66f);

    ov_rect(x + 14, y + 84, w - 28, 1.0f, 0.24f, 0.26f, 0.30f, 1.0f);

    for (int i = 0; i < kPanRows; ++i) {
        float rx, ry, rw, rh;
        ov_row_rect(i, &rx, &ry, &rw, &rh);
        const bool on  = (sel == i);
        const bool hot = (g_ov_hot == i);
        if (on)       ov_rect(rx, ry, rw, rh, 0.16f, 0.34f, 0.16f, 1.0f);
        else if (hot) ov_rect(rx, ry, rw, rh, 0.17f, 0.19f, 0.23f, 1.0f);
        if (on)       ov_rect(rx, ry, 3.0f, rh, 0.45f, 0.95f, 0.45f, 1.0f);
        const float tr = on ? 0.60f : (hot ? 0.95f : 0.70f);
        const float tg = on ? 1.00f : (hot ? 0.95f : 0.70f);
        const float tb = on ? 0.60f : (hot ? 0.95f : 0.74f);
        ov_text(kRowName[i], rx + 16, ry + 5, px, tr, tg, tb);
    }

    // Placed from where the rows actually end, not from the bottom edge. The
    // last row runs to kRowTop + kPanRows * kRowH, and a footer measured back
    // from the panel height landed on top of it.
    const float afterRows = y + kRowTop + kPanRows * kRowH;
    ov_rect(x + 14, afterRows + 8.0f, w - 28, 1.0f, 0.20f, 0.22f, 0.26f, 1.0f);
    ov_text("CLICK A MODE - ESC CLOSES", x + 14, afterRows + 18.0f, 1.5f,
            0.42f, 0.42f, 0.48f);

    ov_rect(g_ov_mx, g_ov_my, 2.0f, 14.0f, 0.0f, 0.0f, 0.0f, 0.9f);
    ov_rect(g_ov_mx, g_ov_my, 14.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.9f);
    ov_rect(g_ov_mx + 1, g_ov_my + 1, 1.0f, 11.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    ov_rect(g_ov_mx + 1, g_ov_my + 1, 11.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
}

// Logged step by step for the first frames after each open, so a crash names
// the call it died in instead of leaving "panel: shown" as the last record.
#define OVSTEP(n) do { if (g_ov_trace) log_line("  ov: " n); } while (0)

static void ov_draw(IDXGISwapChain *sc) {
    if (!g_ov_enabled || !g_ov_visible || sc == nullptr) return;
    OVSTEP("enter");
    if (g_ov_trace > 0) --g_ov_trace;

    // Everything below is built for one swap chain. Loading a save makes the
    // game build a new one -- and DLSS-G a new one behind it -- so the device
    // and queue cached here stop owning the buffers being drawn into, which
    // is the same DXGI_ERROR_ACCESS_DENIED as before arriving by a different
    // road. Rebuild instead of writing into someone else's buffers.
    if (g_ov_ready && sc != g_ov_sc) {
        log_line("overlay: swap chain changed, rebuilding");
        ov_release();
    }
    // And if the device went away for any reason at all, drop everything and
    // build again rather than carrying a dead one forward.
    if (g_ov_ready && g_ov_dev != nullptr && g_ov_dev->GetDeviceRemovedReason() != S_OK) {
        log_line("overlay: device gone, rebuilding");
        ov_release();
    }   // counted on entry: the early returns
                                        // at levels 1 and 2 never reached the
                                        // decrement at the bottom, so a bisect
                                        // run logged every single frame.
    if (!ov_init(sc)) { OVSTEP("init failed"); g_ov_trace = 0; return; }
    OVSTEP("init ok");
    // Checked before anything is submitted. Every reading so far has been
    // taken *after* ExecuteCommandLists, which cannot tell "our submission
    // removed the device" from "the device was already gone and we are the
    // first to look". Those need opposite fixes.
    {
        const HRESULT pre = g_ov_dev->GetDeviceRemovedReason();
        if (pre != S_OK) {
            log_num("  ov: device was ALREADY removed before we submitted, reason ",
                    (unsigned)pre);
            ov_release();
            return;
        }
    }

    IDXGISwapChain *use_sc = sc;
    IDXGISwapChain3 *sc3 = nullptr;
    if (FAILED(use_sc->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&sc3)) || sc3 == nullptr)
        return;
    OVSTEP("got swapchain3");
    const UINT idx = sc3->GetCurrentBackBufferIndex();
    OVSTEP("got index");
    ID3D12Resource *back = nullptr;
    if (FAILED(sc3->GetBuffer(idx, __uuidof(ID3D12Resource), (void **)&back)) || back == nullptr) {
        sc3->Release(); return;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    sc3->GetDesc(&scd);
    g_ov_w = (float)scd.BufferDesc.Width;
    g_ov_h = (float)scd.BufferDesc.Height;
    if (g_ov_w < 1.0f || g_ov_h < 1.0f) { back->Release(); sc3->Release(); return; }

    OVSTEP("got buffer");
    ov_update_pointer();
    ov_build_panel();
    OVSTEP("panel built");

    // The render target view is recreated here rather than cached, so a
    // resolution change or a swap-chain resize cannot leave a stale
    // descriptor behind -- the failure mode that is invisible until it
    // corrupts a frame.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_ov_rtvheap->GetCPUDescriptorHandleForHeapStart();
    g_ov_dev->CreateRenderTargetView(back, nullptr, rtv);

    OVSTEP("rtv created");
    // Take the next slot only if the gpu has finished with it. No stall, and
    // no chance of resetting an allocator that is still being read.
    const int slot = g_ov_slot;
    if (g_ov_fence->GetCompletedValue() < g_ov_slot_fence[slot]) {
        OVSTEP("slot busy, frame skipped");
        back->Release(); sc3->Release();
        return;
    }
    g_ov_alloc = g_ov_alloc_ring[slot];
    g_ov_alloc->Reset();
    g_ov_list->Reset(g_ov_alloc, g_ov_pso);

    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = back;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_ov_list->ResourceBarrier(1, &br);
    OVSTEP("barrier in");

    D3D12_VIEWPORT vp{ 0, 0, g_ov_w, g_ov_h, 0.0f, 1.0f };
    D3D12_RECT sr{ 0, 0, (LONG)g_ov_w, (LONG)g_ov_h };
    g_ov_list->RSSetViewports(1, &vp);
    g_ov_list->RSSetScissorRects(1, &sr);
    g_ov_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g_ov_list->SetGraphicsRootSignature(g_ov_root);
    g_ov_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = g_ov_vb->GetGPUVirtualAddress();
    vbv.StrideInBytes = sizeof(OvVert);
    vbv.SizeInBytes = (UINT)(sizeof(OvVert) * g_ov_verts);
    g_ov_list->IASetVertexBuffers(0, 1, &vbv);
    g_ov_list->DrawInstanced((UINT)g_ov_verts, 1, 0, 0);

    br.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_ov_list->ResourceBarrier(1, &br);
    g_ov_list->Close();

    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)g_ov_list };
    OVSTEP("closed");
    g_ov_queue->ExecuteCommandLists(1, lists);
    OVSTEP("executed");
    // If the submission upset the device, this says so with a reason code
    // instead of leaving the process to die a frame later with nothing on
    // record. 0x887A0006 is HUNG, 0x887A0007 RESET, 0x887A0020 INTERNAL.
    {
        const HRESULT rr = g_ov_dev->GetDeviceRemovedReason();
        if (rr != S_OK) log_num("  ov: DEVICE REMOVED, reason ", (unsigned)rr);
    }

    // Waited on before the allocator is reused. One command allocator and a
    // full stall is the wrong shape for a hot path, and the right shape for
    // something drawn only while a panel is open.
    ++g_ov_fenceval;
    g_ov_queue->Signal(g_ov_fence, g_ov_fenceval);
    g_ov_slot_fence[slot] = g_ov_fenceval;
    g_ov_slot = (slot + 1) % kOvSlots;

    OVSTEP("signalled");
    back->Release();
    sc3->Release();
}
