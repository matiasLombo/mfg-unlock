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
    {'/',{0x20,0x10,0x08,0x04,0x02}}, {'>',{0x00,0x41,0x22,0x14,0x08}},
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
// Where this frame's vertices go: slot * kOvMaxVerts into the mapping.
static int                  g_ov_vbase = 0;
static ID3D12Fence         *g_ov_fence = nullptr;
static HANDLE               g_ov_event = nullptr;
static UINT64               g_ov_fenceval = 0;
static bool                 g_ov_ready = false;
static bool                 g_ov_failed = false;
static int                  g_ov_verts = 0;

static int g_ov_trace = 0;
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

static const float kPanX = 40.0f, kPanY = 40.0f, kPanW = 340.0f;
static const float kPad  = 14.0f;          // panel edge to any content
static const float kHdrH = 42.0f;          // title strip
static const float kBoxH = 26.0f;          // one readout box
static const float kBoxGap = 6.0f;
static const int   kPanRows = 6;
static const float kRowH = 22.0f;
static const float kListPad = 6.0f;
static const float kListH = kPanRows * kRowH + 2 * kListPad;
static const float kGap = 8.0f;
static const float kTgtH = 80.0f;          // the target block, when shown
static const float kFootH = 38.0f;
// Offsets from kPanY.
static const float kListTop = kHdrH + 2.0f * (kBoxH + kBoxGap) + 2.0f;
static const float kTgtTop  = kListTop + kListH + kGap;
// Hot ids for the two controls in the target block, clear of the row indices.
static const int kHotSlider = 100, kHotValue = 101;
static const int kStops[] = { 0, 30, 40, 48, 60, 72, 90, 100, 120, 144,
                              165, 180, 200, 240 };
static const int kNStops = (int)(sizeof(kStops) / sizeof(kStops[0]));

// Typing into the value box. The digits are polled like every other key in
// this project, so a game that never delivers WM_CHAR cannot stop it.
static bool g_ov_editing = false;
static char g_ov_edit[5] = { 0, 0, 0, 0, 0 };
static int  g_ov_editlen = 0;
static unsigned g_ov_frame = 0;                 // for the cursor blink

static float    g_ov_w = 1.0f, g_ov_h = 1.0f;   // client size, set each draw
static HWND     g_ov_hwnd = nullptr;
static WNDPROC  g_ov_prevproc = nullptr;
static float    g_ov_mx = 0.0f, g_ov_my = 0.0f;
static int      g_ov_hot = -1;             // row under the pointer, -1 = none

// Our own module handle, which the raw input window class registers with.
HMODULE g_ov_self = nullptr;

// Relative motion straight from the mouse device, via our own raw input sink.
//
// Every position-based scheme failed, and the reason was only visible once the
// dll logged its own numbers: the low-level hook saw 926 real movement events
// and zero injected ones, while the cursor never moved more than a single
// pixel from where it was put -- `largest step seen 1`, every run. That is not
// the game re-centring, since SetCursorPos events carry LLMHF_INJECTED and
// there were none. It is ClipCursor: the game confines the cursor to a tiny
// box and re-applies it every frame, so GetCursorPos and MSLLHOOKSTRUCT::pt
// are both clamped to that box. A constant one-pixel bias then accumulated
// into a slow drift that parked the pointer in the corner and kept it there,
// which is exactly what "the mouse does not move" looked like.
//
// Raw input reports device deltas. It has no relationship to the cursor, so
// clipping, re-centring and pointer acceleration cannot touch it. An earlier
// comment here dismissed it on the grounds that WM_INPUT only arrives if the
// game registered raw input against the window we subclassed. That was wrong:
// RIDEV_INPUTSINK registers a window of *ours*, and delivers even when we are
// not in the foreground.
//
// The one real hazard is that a process may hold only one raw input
// registration per device, so ours displaces whatever the game had. That is
// saved first and handed back on close -- and while the panel is open, the
// game losing mouse input is the input blocking we wanted anyway.
static HWND  g_ov_riwnd = nullptr;
static bool  g_ov_riactive = false;
static RAWINPUTDEVICE g_ov_riprev = { 0, 0, 0, nullptr };
static bool  g_ov_rihadprev = false;
static void ov_raw_mouse(const RAWMOUSE *m) {
    if (!g_ov_visible) return;
    if ((m->usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
        // Tablets and remote desktop report an absolute position in a
        // normalised 0..65535 space rather than a delta.
        const bool virt = (m->usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        const int sw = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        const int sh = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        if (sw > 0 && sh > 0) {
            g_ov_mx = (float)m->lLastX / 65535.0f * g_ov_w;
            g_ov_my = (float)m->lLastY / 65535.0f * g_ov_h;
        }
    } else if (m->lLastX != 0 || m->lLastY != 0) {
        g_ov_mx += (float)m->lLastX;
        g_ov_my += (float)m->lLastY;
    }
    if (g_ov_mx < 0) g_ov_mx = 0;
    if (g_ov_my < 0) g_ov_my = 0;
    if (g_ov_mx > g_ov_w) g_ov_mx = g_ov_w;
    if (g_ov_my > g_ov_h) g_ov_my = g_ov_h;
}

static LRESULT CALLBACK ov_raw_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT) {
        // Sized first: a mouse packet fits in RAWINPUT, but asking keeps this
        // correct if a packet ever carries more than it does today.
        UINT sz = 0;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, nullptr, &sz,
                            sizeof(RAWINPUTHEADER)) == 0 &&
            sz != 0 && sz <= sizeof(RAWINPUT)) {
            RAWINPUT ri;
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz,
                                sizeof(RAWINPUTHEADER)) == sz &&
                ri.header.dwType == RIM_TYPEMOUSE) {
                ov_raw_mouse(&ri.data.mouse);
            }
        }
    }
    return DefWindowProcW(h, m, w, l);
}

// Must run on the thread that pumps messages, which is the recorder thread.
static bool ov_raw_window(void) {
    if (g_ov_riwnd != nullptr) return true;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ov_raw_proc;
    wc.hInstance = g_ov_self;
    wc.lpszClassName = L"MfgUnlockRawInput";
    RegisterClassExW(&wc);            // benign if already registered
    g_ov_riwnd = CreateWindowExW(0, L"MfgUnlockRawInput", L"", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, g_ov_self, nullptr);
    if (g_ov_riwnd == nullptr) log_line("panel: raw input window FAILED");
    return g_ov_riwnd != nullptr;
}

// Claims the mouse for our window, remembering what the game had so it can be
// handed back. Re-asserted on every tick while the panel is open: a game that
// re-registers raw input each frame would otherwise take it straight back.
static void ov_raw_claim(void) {
    if (!ov_raw_window()) return;
    if (!g_ov_riactive) {
        UINT n = 0;
        g_ov_rihadprev = false;
        // Sizing call: this one is documented to return -1 and set n, so an
        // ordinary "did it fail" test would read backwards here.
        GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE));
        if (n != 0 && n <= 64) {
            RAWINPUTDEVICE cur[64];
            const UINT got = GetRegisteredRawInputDevices(cur, &n,
                                                          sizeof(RAWINPUTDEVICE));
            if (got != (UINT)-1) {
                for (UINT i = 0; i < got; ++i) {
                    if (cur[i].usUsagePage == 0x01 && cur[i].usUsage == 0x02) {
                        g_ov_riprev = cur[i];
                        g_ov_rihadprev = true;
                        break;
                    }
                }
            }
        }
        log_line(g_ov_rihadprev ? "panel: taking raw mouse from the game"
                                : "panel: claiming raw mouse");
    }
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;            // generic desktop
    rid.usUsage     = 0x02;            // mouse
    rid.dwFlags     = RIDEV_INPUTSINK; // delivered even without focus
    rid.hwndTarget  = g_ov_riwnd;
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid)) == FALSE && !g_ov_riactive) {
        log_line("panel: raw mouse claim FAILED");
    }
    g_ov_riactive = true;
}

static void ov_raw_release(void) {
    if (!g_ov_riactive) return;
    if (g_ov_rihadprev) {
        RegisterRawInputDevices(&g_ov_riprev, 1, sizeof(RAWINPUTDEVICE));
    } else {
        RAWINPUTDEVICE rid;
        rid.usUsagePage = 0x01;
        rid.usUsage     = 0x02;
        rid.dwFlags     = RIDEV_REMOVE;
        rid.hwndTarget  = nullptr;     // must be null when removing
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }
    g_ov_riactive = false;
}

static void ov_grab_cursor(bool on) {
    if (on) {
        // Start on the panel rather than in a corner, so the first movement is
        // visible where the player is already looking.
        g_ov_mx = kPanX + kPanW * 0.5f;
        g_ov_my = kPanY + kListTop + kRowH * 0.5f;
        ov_raw_claim();
        return;
    }
    ov_raw_release();
}

// The cursor itself is left alone now: nothing is re-centred and nothing is
// clipped, so there is no fight with the game over a position we no longer
// read. This only re-asserts the raw input claim.
static void ov_pump_cursor(void) {
    if (!g_ov_visible) return;
    // Re-asserted, but not on every tick. The panel polls at 4 ms, and
    // re-registering 250 times a second is a syscall a second for every four
    // milliseconds of a claim nothing is contesting. Every eighth tick still
    // takes it back within about 32 ms of a game grabbing it, which is below
    // anything a hand can notice.
    static int n = 0;
    if (--n > 0) return;
    n = 8;
    ov_raw_claim();
}

static LRESULT CALLBACK ov_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const WNDPROC prev = g_ov_prevproc;    // read once, then only use `prev`
    if (g_ov_visible) {
        switch (m) {
        case WM_MOUSEMOVE:
        case WM_INPUT:
            // Eaten, not read. The pointer has exactly one source now -- the
            // raw input sink -- because the last time two paths wrote the same
            // position, every movement was applied twice and the pointer shot
            // to the clamp in the corner, which is indistinguishable from a
            // pointer that does not move at all.
            return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:   case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:   case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        case WM_KEYDOWN:     case WM_KEYUP:       case WM_CHAR:
        case WM_SYSKEYDOWN:  case WM_SYSKEYUP:
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

// One drawer at a time, and never a queued one: a frame that arrives while
// another thread is mid-submission is dropped, not waited for. Blocking here
// would put a lock on the render thread of a game running at 150 fps to draw
// a panel that can equally well miss a frame.
static volatile LONG g_ov_drawing = 0;
// Distinct threads seen presenting, reported once each. This is how the
// question "does more than one thread present?" gets answered by the dll
// rather than by reasoning about what Streamline probably does.
static volatile LONG g_ov_thr[4] = { 0, 0, 0, 0 };

static void ov_note_thread(void) {
    const LONG me = (LONG)GetCurrentThreadId();
    for (int i = 0; i < 4; ++i) {
        if (g_ov_thr[i] == me) return;
        if (InterlockedCompareExchange(&g_ov_thr[i], me, 0) == 0) {
            log_num("overlay: present called from thread ", (unsigned)me);
            if (i > 0) log_line("  (more than one thread presents here)");
            return;
        }
    }
}

static void ov_draw_locked(IDXGISwapChain *sc);

static void ov_draw(IDXGISwapChain *sc) {
    if (!g_ov_enabled || !g_ov_visible || sc == nullptr) return;
    ov_note_thread();
    if (InterlockedCompareExchange(&g_ov_drawing, 1, 0) != 0) return;
    ov_draw_locked(sc);
    InterlockedExchange(&g_ov_drawing, 0);
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

// Swap chains seen being created, each with the queue it was given. The table
// owns a reference on every queue; g_ov_queue below only borrows one.
struct OvChain { IDXGISwapChain *sc; ID3D12CommandQueue *q; };
static OvChain g_ov_chains[8];
static int g_ov_nchains = 0;

static ID3D12CommandQueue *ov_queue_for(IDXGISwapChain *sc) {
    for (int i = 0; i < g_ov_nchains; ++i)
        if (g_ov_chains[i].sc == sc) return g_ov_chains[i].q;
    return nullptr;
}

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
    g_ov_queue = nullptr;   // borrowed from g_ov_chains, which owns it
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

// Records one swap chain against the queue it was created with. In D3D12 the
// first argument to CreateSwapChain[ForHwnd] is that queue, and it is the one
// object an overlay cannot get from the swap chain afterwards.
//
// The queue exactly as the game handed it over, never unwrapped. NVIDIA's
// guide is explicit that with DLSS-G the host has no direct access to the
// swap-chain buffers and must go through Streamline's proxy, so unwrapping the
// queue, the device or the chain was wrong by design; all three were tried and
// every one was denied.
static void ov_pair_queue(IDXGISwapChain *sc, IUnknown *dev) {
    if (sc == nullptr || dev == nullptr) return;
    ID3D12CommandQueue *q = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D12CommandQueue), (void **)&q)) ||
        q == nullptr)
        return;                                  // D3D11 or something else
    for (int i = 0; i < g_ov_nchains; ++i) {
        if (g_ov_chains[i].sc != sc) continue;
        if (g_ov_chains[i].q == q) { q->Release(); return; }
        g_ov_chains[i].q->Release();             // same chain, requeued
        g_ov_chains[i].q = q;
        if (g_ov_sc == sc) ov_release();         // built against the old one
        return;
    }
    if (g_ov_nchains >= 8) { q->Release(); return; }
    g_ov_chains[g_ov_nchains].sc = sc;
    g_ov_chains[g_ov_nchains].q = q;
    ++g_ov_nchains;
    log_num("overlay: swap chain registered, now tracking ", (unsigned)g_ov_nchains);
}

// ---- geometry ------------------------------------------------------------

static void ov_rect(float x, float y, float w, float h,
                    float r, float g, float b, float a) {
    if (g_ov_verts + 6 > kOvMaxVerts || g_ov_map == nullptr) return;
    // Pixels to normalised device coordinates, done here so the shader needs
    // no constants and the root signature stays empty.
    const float x0 =  (x / g_ov_w) * 2.0f - 1.0f;
    const float x1 =  ((x + w) / g_ov_w) * 2.0f - 1.0f;
    const float y0 = 1.0f - (y / g_ov_h) * 2.0f;
    const float y1 = 1.0f - ((y + h) / g_ov_h) * 2.0f;
    OvVert *v = g_ov_map + g_ov_vbase + g_ov_verts;
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
    if (g_ov_queue == nullptr) return false;   // set by the caller, per chain
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
    rd.Width = (UINT64)sizeof(OvVert) * kOvMaxVerts * kOvSlots;
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


// Whether the target block exists at all. It is not dimmed-but-present any
// more: DYNAMIC is the only mode that has a frame-rate target, so for every
// other mode the control is not a disabled thing to wonder about, it is
// simply not there.
static bool ov_target_shown(void) {
    return g_force_sel == 5 && g_dynamic_known;
}

static float ov_foot_top(void) {
    return kPanY + kTgtTop + (ov_target_shown() ? kTgtH + kGap : 0.0f);
}

static float ov_panel_h(void) {
    return ov_foot_top() + kFootH - kPanY;
}

// A one-pixel border. Four rects rather than a rounded rectangle, because the
// renderer draws axis-aligned quads and nothing else.
static void ov_frame(float x, float y, float w, float h,
                     float r, float g, float b, float a) {
    ov_rect(x, y, w, 1.0f, r, g, b, a);
    ov_rect(x, y + h - 1.0f, w, 1.0f, r, g, b, a);
    ov_rect(x, y, 1.0f, h, r, g, b, a);
    ov_rect(x + w - 1.0f, y, 1.0f, h, r, g, b, a);
}

// One place for the row rectangle, used by both the drawing and the hit test
// so they cannot drift apart.
static void ov_row_rect(int i, float *rx, float *ry, float *rw, float *rh) {
    *rx = kPanX + kPad + 4.0f;
    *ry = kPanY + kListTop + kListPad + i * kRowH;
    *rw = kPanW - 2.0f * kPad - 8.0f;
    *rh = kRowH - 2.0f;
}

// The slider track, and the box showing the number.
static void ov_track_rect(float *x0, float *x1, float *y) {
    *x0 = kPanX + kPad + 18.0f;
    *x1 = kPanX + kPanW - kPad - 18.0f;
    *y  = kPanY + kTgtTop + 48.0f;
}

static void ov_value_rect(float *x, float *y, float *w, float *h) {
    *w = 96.0f;
    *h = 26.0f;
    *x = kPanX + kPanW - kPad - 12.0f - *w;
    *y = kPanY + kTgtTop + 10.0f;
}

static int ov_stop_index(int fps) {
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i < kNStops; ++i) {
        const int d = kStops[i] > fps ? kStops[i] - fps : fps - kStops[i];
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

// The stop nearest a pointer x, so dragging lands on a real value rather than
// on whatever pixel the hand stopped at.
static int ov_stop_at(float mx) {
    float x0, x1, ty;
    ov_track_rect(&x0, &x1, &ty);
    float t = (mx - x0) / (x1 - x0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    int i = (int)(t * (float)(kNStops - 1) + 0.5f);
    if (i < 0) i = 0;
    if (i >= kNStops) i = kNStops - 1;
    return i;
}

// Pointer position in client pixels, polled rather than taken from messages:
// the game may be using raw input, and a panel that depends on WM_MOUSEMOVE
// arriving would simply stop responding.
static void ov_update_pointer(void) {
    g_ov_hot = -1;
    if (ov_target_shown()) {
        float vx, vy, vw, vh;
        ov_value_rect(&vx, &vy, &vw, &vh);
        if (g_ov_mx >= vx && g_ov_mx < vx + vw && g_ov_my >= vy && g_ov_my < vy + vh) {
            g_ov_hot = kHotValue;
            return;
        }
        float x0, x1, ty;
        ov_track_rect(&x0, &x1, &ty);
        // A generous band: the track is four pixels tall and nobody can hit
        // that with a pointer they are moving by hand.
        if (g_ov_mx >= x0 - 10.0f && g_ov_mx <= x1 + 10.0f &&
            g_ov_my >= ty - 12.0f && g_ov_my <= ty + 14.0f) {
            g_ov_hot = kHotSlider;
            return;
        }
    }
    for (int i = 0; i < kPanRows; ++i) {
        float rx, ry, rw, rh;
        ov_row_rect(i, &rx, &ry, &rw, &rh);
        if (g_ov_mx >= rx && g_ov_mx < rx + rw && g_ov_my >= ry && g_ov_my < ry + rh) {
            g_ov_hot = i;
            break;
        }
    }
}

// Typing state. Committing clamps rather than refusing: a number below the
// lowest stop means AUTO, and anything above the highest is the highest.
static void ov_edit_begin(void) {
    g_ov_editing = true;
    g_ov_editlen = 0;
    g_ov_edit[0] = 0;
}

static void ov_edit_digit(char c) {
    if (!g_ov_editing || g_ov_editlen >= 3) return;
    g_ov_edit[g_ov_editlen++] = c;
    g_ov_edit[g_ov_editlen] = 0;
}

static void ov_edit_back(void) {
    if (!g_ov_editing || g_ov_editlen == 0) return;
    g_ov_edit[--g_ov_editlen] = 0;
}

// Returns the value to apply, or -1 if nothing should change.
static int ov_edit_commit(void) {
    if (!g_ov_editing) return -1;
    g_ov_editing = false;
    if (g_ov_editlen == 0) return -1;
    int v = 0;
    for (int i = 0; i < g_ov_editlen; ++i) v = v * 10 + (g_ov_edit[i] - '0');
    if (v > 0 && v < kStops[1]) v = 0;             // below the lowest: AUTO
    if (v > kStops[kNStops - 1]) v = kStops[kNStops - 1];
    return v;
}

static void ov_edit_cancel(void) {
    g_ov_editing = false;
    g_ov_editlen = 0;
    g_ov_edit[0] = 0;
}

// Selection, shared with the override in proxy.cpp:
//   0 AUTO -- leave whatever the game asked for
//   1 OFF  -- DLSSGMode::eOff
//   2..4   -- eOn with 1, 2 or 3 generated frames, i.e. 2x, 3x, 4x
static const char *kRowName[kPanRows] = { "AUTO", "OFF", "2X", "3X", "4X", "DYNAMIC" };

// Right-aligns a short value at a column, so the two readouts line up
// instead of drifting with the label length.
static void ov_text_right(const char *s, float right, float y, float px,
                          float r, float g, float b) {
    int n = 0;
    for (const char *q = s; *q != 0; ++q) ++n;
    ov_text(s, right - n * 6.0f * px, y, px, r, g, b);
}

// Writes a number, right-aligned, into a caller's buffer.
static void ov_num(char *out, int v) {
    int k = 0, n = 0, d[5];
    if (v <= 0) { out[0] = '0'; out[1] = 0; return; }
    while (v > 0 && n < 5) { d[n++] = v % 10; v /= 10; }
    while (n > 0) out[k++] = (char)('0' + d[--n]);
    out[k] = 0;
}

static void ov_build_panel(void) {
    g_ov_verts = 0;
    ++g_ov_frame;
    const float px = 2.0f;
    const float x = kPanX, y = kPanY, w = kPanW, h = ov_panel_h();
    const float in = x + kPad;               // left edge of every box
    const float inw = w - 2.0f * kPad;       // and their width

    // Ground, then the accent strip that names the thing.
    ov_rect(x, y, w, h, 0.05f, 0.06f, 0.07f, 0.94f);
    ov_frame(x, y, w, h, 0.20f, 0.55f, 0.25f, 0.55f);
    ov_rect(x, y, w, 3.0f, 0.35f, 0.88f, 0.38f, 1.0f);

    ov_text("MFG UNLOCK", in, y + 16.0f, 2.4f, 0.42f, 0.95f, 0.45f);
    ov_text("///", x + w - kPad - 24.0f, y + 18.0f, 1.6f, 0.25f, 0.60f, 0.30f);

    const int asked = (int)g_last_seen_generated;
    const int sel   = (int)g_force_sel;

    // The two readouts, each in its own box so the eye separates "what the
    // game asked for" from "what we are doing about it".
    {
        char v[8];
        v[0] = (char)('0' + (asked + 1 > 9 ? 9 : asked + 1)); v[1] = 'X'; v[2] = 0;
        const float by = y + kHdrH;
        ov_frame(in, by, inw, kBoxH, 0.22f, 0.26f, 0.30f, 1.0f);
        ov_text("GAME ASKS", in + 12.0f, by + 9.0f, px, 0.60f, 0.62f, 0.66f);
        ov_text_right(v, in + inw - 12.0f, by + 9.0f, px, 0.80f, 0.82f, 0.86f);

        const float by2 = by + kBoxH + kBoxGap;
        ov_frame(in, by2, inw, kBoxH, 0.22f, 0.26f, 0.30f, 1.0f);
        ov_text("OVERRIDE", in + 12.0f, by2 + 9.0f, px, 0.60f, 0.62f, 0.66f);
        ov_text_right(kRowName[sel], in + inw - 12.0f, by2 + 9.0f, px,
                      sel > 0 ? 0.42f : 0.60f,
                      sel > 0 ? 0.95f : 0.62f,
                      sel > 0 ? 0.45f : 0.66f);
    }

    // The modes.
    {
        const float ly = y + kListTop;
        ov_frame(in, ly, inw, kListH, 0.22f, 0.26f, 0.30f, 1.0f);
        for (int i = 0; i < kPanRows; ++i) {
            float rx, ry, rw, rh;
            ov_row_rect(i, &rx, &ry, &rw, &rh);
            const bool avail = (i != 5) || g_dynamic_known;
            const bool on  = (sel == i) && avail;
            const bool hot = (g_ov_hot == i) && avail;
            if (on) {
                // Filled solid, and the text goes dark on it. A selection you
                // have to compare against its neighbours to find is not a
                // selection.
                ov_rect(rx, ry, rw, rh, 0.36f, 0.92f, 0.40f, 1.0f);
                ov_rect(rx - 6.0f, ry, 4.0f, rh, 0.42f, 1.00f, 0.45f, 1.0f);
                ov_text(kRowName[i], rx + 12.0f, ry + 7.0f, px, 0.04f, 0.10f, 0.05f);
                ov_text(">", rx + rw - 16.0f, ry + 7.0f, px, 0.04f, 0.10f, 0.05f);
            } else {
                if (hot) ov_rect(rx, ry, rw, rh, 0.13f, 0.16f, 0.19f, 1.0f);
                const float d = avail ? 1.0f : 0.34f;
                const float c = hot ? 0.95f : 0.72f;
                ov_text(kRowName[i], rx + 12.0f, ry + 7.0f, px, c * d, c * d, (c + 0.03f) * d);
            }
        }
    }

    // The target, only while DYNAMIC is the selection -- the panel is shorter
    // without it rather than carrying a dead control.
    if (ov_target_shown()) {
        const float ty = y + kTgtTop;
        ov_frame(in, ty, inw, kTgtH, 0.22f, 0.26f, 0.30f, 1.0f);
        ov_text("TARGET FPS", in + 12.0f, ty + 17.0f, px, 0.60f, 0.62f, 0.66f);

        // What the number is right now: what is being typed, if anything.
        char t[8];
        if (g_ov_editing) {
            int k = 0;
            for (; k < g_ov_editlen; ++k) t[k] = g_ov_edit[k];
            t[k] = 0;
        } else if (g_dyn_target <= 0) {
            t[0] = 'A'; t[1] = 'U'; t[2] = 'T'; t[3] = 'O'; t[4] = 0;
        } else {
            ov_num(t, (int)g_dyn_target);
        }

        float vx, vy, vw, vh;
        ov_value_rect(&vx, &vy, &vw, &vh);
        const bool vhot = g_ov_hot == kHotValue;
        ov_rect(vx, vy, vw, vh, 0.03f, 0.05f, 0.04f, 1.0f);
        ov_frame(vx, vy, vw, vh,
                 g_ov_editing ? 0.42f : (vhot ? 0.35f : 0.24f),
                 g_ov_editing ? 1.00f : (vhot ? 0.62f : 0.30f),
                 g_ov_editing ? 0.45f : (vhot ? 0.38f : 0.34f), 1.0f);
        ov_text(t, vx + 10.0f, vy + 9.0f, px, 0.55f, 0.98f, 0.58f);
        if (g_ov_editing && ((g_ov_frame / 30) & 1) == 0) {
            int n = 0;
            while (t[n] != 0) ++n;
            ov_rect(vx + 10.0f + n * 6.0f * px, vy + 7.0f, 2.0f, 12.0f,
                    0.55f, 0.98f, 0.58f, 1.0f);
        }

        // The track. Filled up to the handle so the value reads as a level,
        // not only as a position.
        float x0, x1, sy;
        ov_track_rect(&x0, &x1, &sy);
        // The handle follows the value in force, not the half-typed digits:
        // "1" on the way to "120" would otherwise throw it to the far left.
        const int shown = ov_stop_index((int)g_dyn_target);
        const float f = (float)shown / (float)(kNStops - 1);
        const float hx = x0 + (x1 - x0) * f;
        ov_rect(x0, sy, x1 - x0, 4.0f, 0.16f, 0.20f, 0.18f, 1.0f);
        ov_rect(x0, sy, hx - x0, 4.0f, 0.36f, 0.92f, 0.40f, 1.0f);
        const bool shot = g_ov_hot == kHotSlider;
        ov_rect(hx - 7.0f, sy - 7.0f, 14.0f, 18.0f,
                shot ? 0.50f : 0.36f, shot ? 1.00f : 0.92f, shot ? 0.52f : 0.40f, 1.0f);

        // Ticks, at the stops worth naming. Their positions are their real
        // ones -- the spacing is even because the stops are, not because the
        // labels were placed where they looked good.
        static const int kLabelled[] = { 0, 4, 6, 8, kNStops - 1 };
        for (int i = 0; i < 5; ++i) {
            const int si = kLabelled[i];
            const float tx = x0 + (x1 - x0) * ((float)si / (float)(kNStops - 1));
            const bool cur = si == shown;
            ov_rect(tx, sy + 10.0f, 1.0f, 5.0f, 0.32f, 0.36f, 0.40f, 1.0f);
            char lb[8];
            if (kStops[si] == 0) { lb[0] = 'A'; lb[1] = 'U'; lb[2] = 'T'; lb[3] = 'O'; lb[4] = 0; }
            else ov_num(lb, kStops[si]);
            int n = 0;
            while (lb[n] != 0) ++n;
            ov_text(lb, tx - n * 3.0f * 1.6f, sy + 18.0f, 1.6f,
                    cur ? 0.45f : 0.48f, cur ? 0.95f : 0.50f, cur ? 0.48f : 0.54f);
        }
    }

    // Footer, measured from where the block above actually ended.
    {
        const float fy = ov_foot_top();
        ov_rect(in, fy, inw, 1.0f, 0.20f, 0.22f, 0.26f, 1.0f);
        ov_frame(in, fy + 10.0f, 34.0f, 18.0f, 0.35f, 0.85f, 0.38f, 1.0f);
        ov_text("ESC", in + 6.0f, fy + 15.0f, 1.6f, 0.45f, 0.95f, 0.48f);
        ov_text(g_ov_editing ? "CANCELS" : "CLOSES", in + 44.0f, fy + 15.0f, 1.6f,
                0.50f, 0.52f, 0.56f);
        ov_text("///", x + w - kPad - 24.0f, fy + 15.0f, 1.6f, 0.25f, 0.60f, 0.30f);
    }

    ov_rect(g_ov_mx, g_ov_my, 2.0f, 14.0f, 0.0f, 0.0f, 0.0f, 0.9f);
    ov_rect(g_ov_mx, g_ov_my, 14.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.9f);
    ov_rect(g_ov_mx + 1, g_ov_my + 1, 1.0f, 11.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    ov_rect(g_ov_mx + 1, g_ov_my + 1, 11.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
}

// Logged step by step for the first frames after each open, so a crash names
// the call it died in instead of leaving "panel: shown" as the last record.
#define OVSTEP(n) do { if (g_ov_trace) log_line("  ov: " n); } while (0)

static void ov_draw_locked(IDXGISwapChain *sc) {
    // This chain's own queue, or nothing at all. A swap chain we never saw
    // created has no queue we may legally submit against, and drawing on the
    // wrong one is what killed the device rather than merely failing.
    ID3D12CommandQueue *const q = ov_queue_for(sc);
    if (q == nullptr) {
        static IDXGISwapChain *said = nullptr;
        if (said != sc) {
            said = sc;
            log_line("overlay: this swap chain was not seen being created,"
                     " so its queue is unknown -- not drawing on it");
        }
        return;
    }
    if (q != g_ov_queue) {
        if (g_ov_ready) log_line("overlay: swap chain changed queue, rebuilding");
        ov_release();
        g_ov_queue = q;
    }
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

    // Take the next slot only if the gpu has finished with it. No stall, and
    // no chance of resetting an allocator -- or overwriting vertices -- that
    // are still being read. Chosen before anything is written, because what
    // gets written now belongs to this slot.
    const int slot = g_ov_slot;
    if (g_ov_fence->GetCompletedValue() < g_ov_slot_fence[slot]) {
        OVSTEP("slot busy, frame skipped");
        back->Release(); sc3->Release();
        return;
    }
    g_ov_vbase = slot * kOvMaxVerts;

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
    vbv.BufferLocation = g_ov_vb->GetGPUVirtualAddress() +
                         (UINT64)g_ov_vbase * sizeof(OvVert);
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
        if (rr != S_OK) {
            log_num("  ov: DEVICE REMOVED, reason ", (unsigned)rr);
            if ((unsigned)rr == 0x887A002Bu)
                log_line("  ov: that is ACCESS_DENIED -- the panel wrote a back buffer"
                         " it was not allowed to");
            // Shut the panel down for the rest of the process. It cannot be
            // rebuilt on a removed device, and retrying every frame only adds
            // failures on top of the one that matters.
            g_ov_visible = false;
            g_ov_enabled = false;
            back->Release();
            sc3->Release();
            return;
        }
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
