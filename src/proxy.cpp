// A version.dll that sits in a game folder and unlocks Multi Frame Generation.
//
// Why a proxy and not a file patch: NGX verifies the Authenticode signature of
// nvngx_dlssg.dll as it loads it.  A modified snippet is refused --
// "nvLoadSignedLibraryW() failed ... the digital signature of the object did not
// verify" -- and NGX falls back to the driver store copy, which for DLSS-G is an
// older build with no multi-frame parameters at all.  sl.dlss_g then finds
// neither DLSSG.ModelVersion nor DLSSG.MultiFrameCountMax and decides DLSS-G
// cannot run, so frame generation vanishes rather than merely staying at 2x.
//
// The signature is checked at load and never again.  So the two architecture
// comparisons get rewritten in the mapped image instead, while the loader is
// still bringing it in.  Nothing on disk changes, no signature breaks, and the
// change lives and dies with the process.
//
// Timing is the whole trick.  The snippet's PopulateParameters -- which decides
// what DLSSG.MultiFrameCountMax will say -- runs inside NVSDK_NGX_*_Init_Ext.
// Patching after that call is too late; the capability block is already filled
// in.  LdrRegisterDllNotification fires early enough, and a statically imported
// proxy is loaded before the game's first instruction, so the callback is armed
// long before NGX exists.
//
// version.dll is the target because DOOM The Dark Ages imports it directly
// (GetFileVersionInfoA, GetFileVersionInfoSizeA, VerQueryValueA), it is tiny,
// and it is loaded at process start.
//
// build: see build.sh

#include <windows.h>
#include <winternl.h>
#include <dxgi.h>
#include <cstdint>
#include <MinHook.h>
#include "cubins.h"

// ------------------------------------------------------------------- log ---
//
// Raw file calls, no CRT: some of this runs under the loader lock.

static wchar_t g_log[MAX_PATH];

static void log_line(const char *text) {
    if (g_log[0] == 0) return;
    HANDLE h = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    SetFilePointer(h, 0, nullptr, FILE_END);
    size_t len = 0;
    while (text[len] != 0) ++len;
    WriteFile(h, text, (DWORD)len, &n, nullptr);
    WriteFile(h, "\r\n", 2, &n, nullptr);
    CloseHandle(h);
}

static void log_num(const char *label, unsigned long long v) {
    char buf[128];
    int i = 0;
    while (label[i] != 0 && i < 90) { buf[i] = label[i]; ++i; }
    char digits[24];
    int d = 0;
    if (v == 0) digits[d++] = '0';
    while (v > 0) { digits[d++] = (char)('0' + (v % 10)); v /= 10; }
    while (d > 0) buf[i++] = digits[--d];
    buf[i] = 0;
    log_line(buf);
}

// ----------------------------------------------------------- the rewrite ---

static const unsigned kArchBlackwell = 0x1B0;
static int g_gates = 0;
static bool g_preset_b = false;
static bool g_cubins = false;
static bool g_meter_off = false;

// The id of the newest build in NVIDIA's OTA cache, or empty if there is none.
// NGX loads that copy and leaves the one in the game folder unused, so a patch
// failing against the game's own copy is not a failure worth reporting -- and
// reporting it anyway is exactly how a log cries wolf: DOOM maps both, and the
// verdict shouted "CUBINS NOT APPLIED" about the image that never executes
// while the one that does was patched correctly.
static wchar_t g_ota_newest[64] = {0};
// Set once that build has actually been seen mapping, which is the only thing
// that makes another copy provably redundant.
static bool g_ota_mapped = false;

// ---- selecting the interpolation model ---------------------------------
//
// The snippet does not have one interpolation network, it has two, and it picks
// between them from a driver setting read in EndpointConfiguration::ReadRegkeys:
//
//     INFO: Preset A selected, disabling UIR.
//     INFO: Preset B selected, enabling UIR.
//
// UIR is UI recomposition -- how the network separates interface elements from
// the world it is warping. The weights sit next to each other in the binary,
// convoluted_cat/endpoint and endpoint_uir, and the snippet's own telemetry
// reports which is live. On this machine the key reads zero, no case matches,
// and it falls through to the plain endpoint variant with UIR off.
//
// The selection is a switch on the preset id:
//
//     83 EF 01    sub edi, 1
//     74 2D       je  <Preset A>
//     83 EF 01    sub edi, 1
//     74 11       je  <Preset B>
//     ...
//
// Replacing the first test with a jump into the Preset B arm takes the branch
// the plugin already takes when the driver asks for preset 2 -- a configuration
// NVIDIA ships, not an invented one. Five bytes, and the jump is short enough
// to encode in two with the rest padded.

static int patch_preset_b(unsigned char *base) {
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    auto *sec = IMAGE_FIRST_SECTION(nt);
    unsigned char *text = nullptr;
    size_t len = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const char *n = reinterpret_cast<const char *>(sec[i].Name);
        if (n[0] == '.' && n[1] == 't' && n[2] == 'e' && n[3] == 'x' && n[4] == 't') {
            text = base + sec[i].VirtualAddress;
            len = sec[i].Misc.VirtualSize;
            break;
        }
    }
    if (text == nullptr) return 0;
    // sub edi,1 ; je A ; sub edi,1 ; je B ; sub edi,0xFFFFFC ; je A ; cmp edi,1
    static const unsigned char sig[21] = {
        0x83, 0xEF, 0x01, 0x74, 0x2D, 0x83, 0xEF, 0x01, 0x74, 0x11,
        0x81, 0xEF, 0xFC, 0xFF, 0xFF, 0x00, 0x74, 0x20, 0x83, 0xFF, 0x01 };
    int hits = 0;
    for (size_t i = 0; i + sizeof sig <= len; ++i) {
        bool match = true;
        for (size_t k = 0; k < sizeof sig; ++k)
            if (text[i + k] != sig[k]) { match = false; break; }
        if (!match) continue;
        // The Preset B arm is the target of the second je: two bytes past that
        // instruction, plus its own displacement.
        const size_t barm = i + 10 + sig[9];
        const long rel = (long)barm - (long)(i + 2);
        if (rel < -128 || rel > 127) continue;
        unsigned char rep[5] = { 0xEB, (unsigned char)rel, 0x90, 0x90, 0x90 };
        DWORD old = 0;
        if (!VirtualProtect(text + i, 5, PAGE_EXECUTE_READWRITE, &old)) continue;
        for (int k = 0; k < 5; ++k) text[i + k] = rep[k];
        VirtualProtect(text + i, 5, old, &old);
        ++hits;
    }
    return hits;
}



// Both gates are `cmp <r32>, 0x1B0` against the NGX architecture id.  Rewriting
// the immediate to 0 makes the comparison read "arch >= 0", true everywhere, so
// whichever way the compiler phrased the predicate -- jl, setae, cmovl, all of
// which appear across snippet builds -- the Blackwell branch is the one taken.
static int patch_gates(unsigned char *base) {
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    auto *sec = IMAGE_FIRST_SECTION(nt);
    unsigned char *text = nullptr;
    size_t len = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const char *n = reinterpret_cast<const char *>(sec[i].Name);
        if (n[0] == '.' && n[1] == 't' && n[2] == 'e' && n[3] == 'x' && n[4] == 't') {
            text = base + sec[i].VirtualAddress;
            len = sec[i].Misc.VirtualSize;
            break;
        }
    }
    if (text == nullptr) return 0;

    int hits = 0;
    for (size_t i = 0; i + 6 <= len; ++i) {
        // 3D imm32 is cmp eax, imm32; 81 /7 imm32 is cmp r32, imm32 for the rest.
        const bool is_eax = text[i] == 0x3D &&
                            *reinterpret_cast<unsigned *>(text + i + 1) == kArchBlackwell;
        const bool is_reg = text[i] == 0x81 && (text[i + 1] & 0xF8) == 0xF8 &&
                            *reinterpret_cast<unsigned *>(text + i + 2) == kArchBlackwell;
        if (!is_eax && !is_reg) continue;
        unsigned char *imm = text + i + (is_eax ? 1 : 2);
        DWORD old = 0;
        if (!VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
        *reinterpret_cast<unsigned *>(imm) = 0;
        VirtualProtect(imm, 4, old, &old);
        ++hits;
    }
    return hits;
}


// ---- swapping in kernels rebuilt from the Blackwell PTX -------------------
//
// See src/cubins.h for what these are and why they exist. Each replacement is
// located by a fingerprint of the *original* -- .text size, shared size and
// register count -- which is unique across all 31 framework kernels, so nothing
// here depends on an address that a snippet update would move. Everything is
// verified before a byte is written: the fatbin magic, the entry kind, that the
// payload really is an uncompressed ELF, and that the replacement fits.
//
// Opt in with mfg-cubins.txt. This one is not like the gate patch: it puts our
// code on the GPU in the render path.

static int g_cubins_done = 0;

static bool elf_fingerprint(const unsigned char *b, size_t len,
                            unsigned *text, unsigned *shared, unsigned *regs) {
    if (len < 0x40 || b[0] != 0x7F || b[1] != 'E' || b[2] != 'L' || b[3] != 'F') return false;
    const unsigned long long shoff = *reinterpret_cast<const unsigned long long *>(b + 0x28);
    const unsigned short shent = *reinterpret_cast<const unsigned short *>(b + 0x3A);
    const unsigned short shnum = *reinterpret_cast<const unsigned short *>(b + 0x3C);
    const unsigned short shstr = *reinterpret_cast<const unsigned short *>(b + 0x3E);
    if (shent < 0x40 || shnum == 0 || shstr >= shnum) return false;
    if (shoff + (unsigned long long)shent * shnum > len) return false;
    const unsigned long long stoff =
        *reinterpret_cast<const unsigned long long *>(b + shoff + (size_t)shstr * shent + 0x18);
    if (stoff >= len) return false;
    *text = *shared = *regs = 0;
    for (unsigned i = 0; i < shnum; ++i) {
        const unsigned char *s = b + shoff + (size_t)i * shent;
        const unsigned name = *reinterpret_cast<const unsigned *>(s);
        const unsigned long long size = *reinterpret_cast<const unsigned long long *>(s + 0x20);
        const unsigned info = *reinterpret_cast<const unsigned *>(s + 0x2C);
        if (stoff + name >= len) continue;
        const char *n = reinterpret_cast<const char *>(b + stoff + name);
        if (n[0] == '.' && n[1] == 't' && n[2] == 'e' && n[3] == 'x' && n[4] == 't' && n[5] == '.') {
            *text = (unsigned)size;
            *regs = (info >> 24) & 0xFF;
        } else if (n[0] == '.' && n[1] == 'n' && n[2] == 'v' && n[3] == '.' &&
                   n[4] == 's' && n[5] == 'h' && n[6] == 'a') {
            *shared = (unsigned)size;
        }
    }
    return *text != 0;
}

// `live` says whether this image is the one NGX will run. A shadow copy still
// gets patched -- it costs nothing and protects against our guess about which
// copy wins being wrong -- but it does not get to narrate its mismatches.
static int patch_cubins(unsigned char *base, bool live = true) {
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    auto *sec = IMAGE_FIRST_SECTION(nt);
    int hits = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        const char *n = reinterpret_cast<const char *>(sec[s].Name);
        if (!(n[0] == '.' && n[1] == 'd' && n[2] == 'a' && n[3] == 't' && n[4] == 'a')) continue;
        unsigned char *p = base + sec[s].VirtualAddress;
        const size_t len = sec[s].Misc.VirtualSize;

        for (size_t i = 0; i + 16 <= len; ++i) {
            if (*reinterpret_cast<unsigned *>(p + i) != 0xBA55ED50u) continue;
            const unsigned short hsz = *reinterpret_cast<unsigned short *>(p + i + 6);
            const unsigned long long fsz = *reinterpret_cast<unsigned long long *>(p + i + 8);
            if (hsz != 16 || fsz == 0 || fsz > 4u * 1024 * 1024 || i + 16 + fsz > len) continue;

            size_t e = i + 16;
            const size_t end = i + 16 + (size_t)fsz;
            while (e + 32 <= end) {
                const unsigned short kind = *reinterpret_cast<unsigned short *>(p + e);
                const unsigned ehsz = *reinterpret_cast<unsigned *>(p + e + 4);
                if (ehsz < 64 || ehsz > 256) break;
                const unsigned long long psz = *reinterpret_cast<unsigned long long *>(p + e + 8);
                const unsigned long long csz = *reinterpret_cast<unsigned long long *>(p + e + 16);
                const unsigned sm = *reinterpret_cast<unsigned *>(p + e + 28);
                if (e + ehsz + psz > end) break;
                // kind 2 is ELF; csz 0 means it was stored uncompressed.
                if (kind == 2 && sm == 89 && csz == 0 && psz > 0x40) {
                    unsigned char *payload = p + e + ehsz;
                    unsigned t = 0, sh = 0, rg = 0;
                    if (elf_fingerprint(payload, (size_t)psz, &t, &sh, &rg)) {
                        for (const auto &c : kCubinPatches) {
                            if (c.text != t || c.shared != sh || c.regs != rg) continue;
                            // The fingerprint matched, so this IS the kernel we
                            // built against -- but the ELF around it has to be
                            // the same size too, or the replacement was derived
                            // from a different snippet build and its constants
                            // may no longer line up. NVIDIA moved all three of
                            // these by 128-256 bytes in the 2026-09-03 OTA drop
                            // while leaving the fingerprints untouched, so the
                            // swap stopped applying and said only "expected 3".
                            // Say which slot moved and by how much: that one
                            // line is the difference between "rerun
                            // rebuild_cubins.py" and a night of guessing.
                            if (c.orig_size != (unsigned)psz) {
                                if (live) {
                                    log_line("  ! slot moved, not patched:");
                                    log_line(c.what);
                                    log_num("      snippet has: ", (unsigned)psz);
                                    log_num("      built for:   ", c.orig_size);
                                }
                                continue;
                            }
                            if (c.size > psz) continue;                   // must fit
                            DWORD old = 0;
                            if (!VirtualProtect(payload, (SIZE_T)psz, PAGE_READWRITE, &old)) break;
                            for (unsigned k = 0; k < c.size; ++k) payload[k] = c.data[k];
                            for (unsigned k = c.size; k < (unsigned)psz; ++k) payload[k] = 0;
                            VirtualProtect(payload, (SIZE_T)psz, old, &old);
                            ++hits;
                            log_line(c.what);
                            break;
                        }
                    }
                }
                e += ehsz + (size_t)psz;
            }
            i = end - 1;
        }
    }
    return hits;
}


// ---- recording, to tell whether a pacing change did anything -------------
//
// The pacer runs inside the game's own present call, so the spacing of those
// calls is a direct readout of it: a pacer that is regulating makes them
// tighter. Timestamp only, off until F9, one hook on the module the game
// actually calls -- Streamline interposes the Vulkan loader, so a hook on
// vulkan-1.dll sees nothing.


// VkPresentInfoKHR, 64-bit layout:
//   +0x00 sType   +0x08 pNext            +0x10 waitSemaphoreCount
//   +0x18 pWaitSemaphores               +0x20 swapchainCount
//   +0x28 pSwapchains                   +0x30 pImageIndices
// The image index says which swapchain image each present actually shows, so a
// run of them is the presentation *order* -- which timestamps alone cannot
// give. Frames generated at t=1/4, 2/4, 3/4 that are shown out of order would
// be evenly spaced and still look wrong.
//
// While we are here, walk pNext for VkSetPresentConfigNV (sType 1000613000,
// numFramesPerBatch at +0x10): that measures what the hardware metering was
// actually told, per present, instead of inferring it from the disassembly.
static const unsigned kSetPresentConfigNV = 1000613000u;

// Two layers matter and they are not the same. The game calls
// sl.interposer!vkQueuePresentKHR once per *rendered* frame; Streamline then
// issues the generated frames further down, through the Vulkan loader. Hooking
// only the top layer measures the input rate, not the output -- which is why an
// earlier capture showed two images at 71 fps and no metering at all. Hook both
// and tag which one produced each row.
struct Sample { long long qpc; unsigned img; int meter; unsigned char src; };
static Sample *g_samples = nullptr;
static volatile LONG g_nsamples = 0;
static volatile LONG g_recording = 0;
// QPC at the start of a recording, so display times can be stored as a small
// offset rather than a 64-bit absolute.
static long long g_rec_qpc0 = 0;
static LONG g_written = 0;
static const int kMaxSamples = 200000;
static long long g_qpc_freq = 1;
static wchar_t g_frames[MAX_PATH];
static wchar_t g_frames_base[MAX_PATH];   // unnumbered name, per-run suffix added at F9
static int g_run_no = 0;

typedef int(__stdcall *PFN_Present)(void *, const void *);
static PFN_Present g_orig_present = nullptr;

static PFN_Present g_orig_present2 = nullptr;

static void note_present(const void *info, unsigned char src) {
    if (g_recording != 0) {
        const LONG i = InterlockedIncrement(&g_nsamples) - 1;
        if (g_samples != nullptr && i < kMaxSamples) {
            g_samples[i].src = src;
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            g_samples[i].qpc = t.QuadPart;
            g_samples[i].img = 0xFFFFFFFFu;
            g_samples[i].meter = -1;
            if (info != nullptr) {
                auto p = reinterpret_cast<const unsigned char *>(info);
                const unsigned nsc = *reinterpret_cast<const unsigned *>(p + 0x20);
                auto idx = *reinterpret_cast<const unsigned *const *>(p + 0x30);
                if (nsc >= 1 && idx != nullptr) g_samples[i].img = idx[0];
                // pNext is a null-terminated chain; bound the walk regardless.
                auto n = *reinterpret_cast<const unsigned char *const *>(p + 8);
                for (int k = 0; n != nullptr && k < 8; ++k) {
                    if (*reinterpret_cast<const unsigned *>(n) == kSetPresentConfigNV) {
                        g_samples[i].meter = (int)*reinterpret_cast<const unsigned *>(n + 0x10);
                        break;
                    }
                    n = *reinterpret_cast<const unsigned char *const *>(n + 8);
                }
            }
        }
    }
}

static int __stdcall hk_present(void *queue, const void *info) {
    note_present(info, 0);
    return g_orig_present(queue, info);
}

// ---- the same measurement for D3D12 ---------------------------------------
//
// The hook above is vkQueuePresentKHR, so it measures Vulkan and nothing else.
// Of the games this project runs on, only DOOM is Vulkan: GTA V, Cyberpunk,
// Avatar and AC Shadows are all D3D12, and in every one of them F9 recorded a
// clean, meaningless zero -- the hook installs, the game simply never calls
// that function. The instrument covered one game out of five and said so
// nowhere.
//
// Present is a COM method, so there is no export to detour; the address lives
// in the swapchain's vtable, slot 8, fixed by the COM contract. Reaching it
// means following DXGI's own chain: the two factory entry points, then the two
// creation methods on the factory that comes back, then the swapchain. Every
// step degrades to doing nothing, because a game whose swapchain arrives some
// way this did not anticipate should lose a diagnostic, not its picture.
//
// Rows land with src=2. img and meter stay empty: they are read out of the
// Vulkan present info, and there is no equivalent to read here.

typedef HRESULT(STDMETHODCALLTYPE *PFN_DXGIPresent)(IDXGISwapChain *, UINT, UINT);
static PFN_DXGIPresent g_orig_dxgi_present = nullptr;

// ---- pacing the frames ourselves, where nothing else will --------------
//
// Streamline gained a pacer in 2.10 and hardware flip metering in 2.11.1.
// Before that there is nothing to patch and nothing to enable: a game on
// 2.8.0 hands all four frames of a 4x batch to Present back to back and then
// waits. Measured in Avatar: 551 batches, 522 of them exactly four presents,
// 1.14 ms apart inside the batch and up to 41 ms of nothing after it. Four
// flips inside one refresh interval means three of them are overwritten
// before the display ever scans them -- the frames are generated, paid for in
// latency, and thrown away. That is why 2x feels better than 4x there.
//
// So pace them here. Not by identifying batches -- once pacing works the
// batches stop existing and the detector eats itself -- but by holding each
// present to the running average interval. Average presents/second is set by
// the game's real framerate times the multiplier and does not change when the
// spacing does, so the target is stable while the bursts smooth out.
//
// Deliberately conservative:
//   - it releases at 95% of the measured average, so it can never become the
//     thing limiting throughput,
//   - it never waits longer than one average interval, so a framerate change
//     cannot turn into a stall,
//   - and it does nothing at all unless the native pacer was looked for and
//     not found. On 2.10+ NVIDIA's own runs and this stays out of the way.
//
// Sleep's granularity is milliseconds and the waits here are single-digit
// milliseconds, so it sleeps on a high-resolution timer for the bulk and
// spins the last stretch.

static bool g_native_pacer_found = false;  // sticky: one plugin with sites is enough
// Timestamps on the *natural* clock -- real time minus everything this pacer
// has slept. Measuring on the real clock fed our own delays back into the
// average we derive the delays from, a closed loop whose only brake was the
// 5% margin shrinking it each pass. It converged, but by accident rather than
// by design, and it hid the burst structure the moment it started working.
// Subtracting our own waiting restores the cadence the game would have had
// untouched: the bursts stay visible in it, and the average stops chasing
// itself.


// What the display did with the frames, rather than what we asked it to do.
// This is the feedback Streamline gets from notifyFrameFlipped and we had no
// equivalent for: everything above schedules against an inferred average and
// then never learns whether any of it reached the screen. DXGI keeps the
// answer on the swapchain -- PresentCount is how many presents the runtime
// took, PresentRefreshCount which refresh the last one was shown at, and
// SyncQPCTime when that refresh happened. Four presents inside one refresh
// interval show up here as a PresentCount that climbs four times while
// PresentRefreshCount climbs once, which is the difference between frames
// that were displayed and frames that were paid for and overwritten.
static void note_display(IDXGISwapChain *sc) {
    DXGI_FRAME_STATISTICS st{};
    if (sc == nullptr || FAILED(sc->GetFrameStatistics(&st))) return;
    const LONG i = g_nsamples - 1;                 // the row note_present just wrote
    if (i >= 0 && i < kMaxSamples && g_samples != nullptr) {
        // The refresh count, not the present count, because the two readings
        // taken so far disagree and this is what separates them. Distinct
        // SyncQPCTime values came out at 43/s, which either means only one
        // frame per batch of four ever reaches the glass, or means the driver
        // updates this structure once per batch and the other three calls read
        // back a stale copy -- 826 distinct values against ~820 batches is
        // suspiciously exact. An earlier run measured 1.06 presents per
        // refresh, which flatly contradicts 43. If the panel refreshes ~165
        // times a second here, the stats are stale and the frames are fine.
        g_samples[i].img = (unsigned)st.PresentRefreshCount;
        // SyncQPCTime, not PresentRefreshCount: when the frame was actually
        // put on the glass, rather than how many refreshes have gone by. The
        // question it answers is whether submitting a batch back-to-back also
        // *shows* it back-to-back, or whether the swapchain queue hands them
        // to the display one per refresh regardless -- which decides whether
        // spacing presents at all is worth anything.
        g_samples[i].meter = (g_qpc_freq > 0 && g_rec_qpc0 != 0)
                ? (int)((st.SyncQPCTime.QuadPart - g_rec_qpc0) * 1000000 / g_qpc_freq)
                : 0;
    }
}

static HRESULT STDMETHODCALLTYPE hk_dxgi_present(IDXGISwapChain *self, UINT interval, UINT flags) {
    note_present(nullptr, 2);
    if (g_recording != 0) note_display(self);
    return g_orig_dxgi_present(self, interval, flags);
}

static void hook_swapchain_present(void *sc) {
    if (sc == nullptr || g_orig_dxgi_present != nullptr) return;
    void **vt = *reinterpret_cast<void ***>(sc);
    if (MH_CreateHook(vt[8], reinterpret_cast<void *>(&hk_dxgi_present),
                      reinterpret_cast<void **>(&g_orig_dxgi_present)) == MH_OK &&
        MH_EnableHook(vt[8]) == MH_OK)
        log_line("recorder: D3D12 present hooked (src=2 rows)");
    else
        g_orig_dxgi_present = nullptr;
}

typedef HRESULT(STDMETHODCALLTYPE *PFN_CSC)(IDXGIFactory *, IUnknown *,
        DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CSCFH)(void *, IUnknown *, HWND, const void *,
        const void *, void *, IDXGISwapChain **);
static PFN_CSC g_orig_csc = nullptr;
static PFN_CSCFH g_orig_cscfh = nullptr;

static HRESULT STDMETHODCALLTYPE hk_csc(IDXGIFactory *self, IUnknown *dev,
        DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **out) {
    HRESULT hr = g_orig_csc(self, dev, desc, out);
    if (SUCCEEDED(hr) && out != nullptr) hook_swapchain_present(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_cscfh(void *self, IUnknown *dev, HWND hwnd,
        const void *d1, const void *fs, void *restrict_to, IDXGISwapChain **out) {
    HRESULT hr = g_orig_cscfh(self, dev, hwnd, d1, fs, restrict_to, out);
    if (SUCCEEDED(hr) && out != nullptr) hook_swapchain_present(*out);
    return hr;
}

// Slots 10 and 15 on IDXGIFactory2 are CreateSwapChain (inherited) and
// CreateSwapChainForHwnd, fixed by the COM contract rather than by version.
static void hook_factory(void *factory) {
    if (factory == nullptr || g_orig_cscfh != nullptr) return;
    void **vt = *reinterpret_cast<void ***>(factory);
    if (MH_CreateHook(vt[15], reinterpret_cast<void *>(&hk_cscfh),
                      reinterpret_cast<void **>(&g_orig_cscfh)) != MH_OK ||
        MH_EnableHook(vt[15]) != MH_OK)
        g_orig_cscfh = nullptr;
    if (MH_CreateHook(vt[10], reinterpret_cast<void *>(&hk_csc),
                      reinterpret_cast<void **>(&g_orig_csc)) != MH_OK ||
        MH_EnableHook(vt[10]) != MH_OK)
        g_orig_csc = nullptr;
}

typedef HRESULT(WINAPI *PFN_F1)(REFIID, void **);
typedef HRESULT(WINAPI *PFN_F2)(UINT, REFIID, void **);
static PFN_F1 g_orig_f0 = nullptr, g_orig_f1 = nullptr;
static PFN_F2 g_orig_f2 = nullptr;

// One detour per export: a shared one could not tell which entry point it was
// reached through, and would forward half its calls to the wrong original.
static HRESULT WINAPI hk_f0(REFIID riid, void **out) {
    HRESULT hr = g_orig_f0(riid, out);
    if (SUCCEEDED(hr) && out != nullptr) hook_factory(*out);
    return hr;
}
static HRESULT WINAPI hk_f1(REFIID riid, void **out) {
    HRESULT hr = g_orig_f1(riid, out);
    if (SUCCEEDED(hr) && out != nullptr) hook_factory(*out);
    return hr;
}
static HRESULT WINAPI hk_f2(UINT flags, REFIID riid, void **out) {
    HRESULT hr = g_orig_f2(flags, riid, out);
    if (SUCCEEDED(hr) && out != nullptr) hook_factory(*out);
    return hr;
}

static bool g_dxgi_armed = false;

static void arm_dxgi_recorder() {
    if (g_dxgi_armed) return;
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (dxgi == nullptr) return;          // not a D3D game, or not loaded yet
    g_dxgi_armed = true;
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return;
    struct { const char *name; void *detour; void **orig; } e[] = {
        { "CreateDXGIFactory",  (void *)&hk_f0, (void **)&g_orig_f0 },
        { "CreateDXGIFactory1", (void *)&hk_f1, (void **)&g_orig_f1 },
        { "CreateDXGIFactory2", (void *)&hk_f2, (void **)&g_orig_f2 },
    };
    int n = 0;
    for (auto &x : e) {
        FARPROC p = GetProcAddress(dxgi, x.name);
        if (p == nullptr) continue;
        if (MH_CreateHook(reinterpret_cast<void *>(p), x.detour, x.orig) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void *>(p)) == MH_OK)
            ++n;
    }
    if (n > 0) log_num("recorder: watching DXGI for a swapchain, entry points: ", (unsigned)n);
}

static int __stdcall hk_present2(void *queue, const void *info) {
    note_present(info, 1);
    return g_orig_present2(queue, info);
}

static void write_samples() {
    const LONG n = g_nsamples > kMaxSamples ? kMaxSamples : g_nsamples;
    if (n <= g_written || g_samples == nullptr || g_frames[0] == 0) return;
    HANDLE h = CreateFileW(g_frames, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    SetFilePointer(h, 0, nullptr, FILE_END);
    if (g_written == 0) {
        const char hd[] = "ms,src,img,meter\r\n";
        WriteFile(h, hd, (DWORD)(sizeof(hd) - 1), &w, nullptr);
    }
    const long long t0 = g_samples[0].qpc;
    char line[64];
    for (LONG i = g_written; i < n; ++i) {
        long long us = (g_samples[i].qpc - t0) * 1000000 / g_qpc_freq;
        long long ms = us / 1000, frac = us % 1000;
        int p = 0; char tmp[24]; int dg = 0;
        if (ms == 0) tmp[dg++] = '0';
        while (ms > 0) { tmp[dg++] = (char)('0' + ms % 10); ms /= 10; }
        while (dg > 0) line[p++] = tmp[--dg];
        line[p++] = '.';
        line[p++] = (char)('0' + (frac / 100) % 10);
        line[p++] = (char)('0' + (frac / 10) % 10);
        line[p++] = (char)('0' + frac % 10);
        // img, then meter; -1 prints as an empty cell so gaps stay obvious.
        long long v[3] = { (long long)g_samples[i].src,
                           (long long)(int)g_samples[i].img,
                           (long long)g_samples[i].meter };
        for (int c = 0; c < 3; ++c) {
            line[p++] = ',';
            if (v[c] < 0) continue;
            long long q = v[c]; dg = 0;
            if (q == 0) tmp[dg++] = '0';
            while (q > 0) { tmp[dg++] = (char)('0' + q % 10); q /= 10; }
            while (dg > 0) line[p++] = tmp[--dg];
        }
        line[p++] = 0x0D; line[p++] = 0x0A;
        WriteFile(h, line, p, &w, nullptr);
    }
    CloseHandle(h);
    g_written = n;
}

static DWORD WINAPI recorder(LPVOID) {
    bool was_down = false;
    int ticks = 0;
    for (;;) {
        Sleep(50);
        if (g_orig_present == nullptr) {
            HMODULE vk = GetModuleHandleW(L"sl.interposer.dll");
            if (vk != nullptr && g_samples != nullptr) {
                auto fn = reinterpret_cast<PFN_Present>(GetProcAddress(vk, "vkQueuePresentKHR"));
                if (fn != nullptr &&
                    (MH_Initialize() == MH_OK || MH_Initialize() == MH_ERROR_ALREADY_INITIALIZED) &&
                    MH_CreateHook(reinterpret_cast<void *>(fn), reinterpret_cast<void *>(&hk_present),
                                  reinterpret_cast<void **>(&g_orig_present)) == MH_OK &&
                    MH_EnableHook(reinterpret_cast<void *>(fn)) == MH_OK) {
                    log_line("recorder ready (F9)");
                } else {
                    g_orig_present = nullptr;
                }
            }
            continue;
        }
        // The interposer hook above sees one present per rendered frame. The
        // generated ones go straight to the loader, so hook that too and let
        // the src column separate them. Distinct address only: if Streamline
        // forwards to the same code we would otherwise chain onto ourselves.
        if (g_orig_present2 == nullptr) {
            HMODULE ld = GetModuleHandleW(L"vulkan-1.dll");
            if (ld != nullptr) {
                auto fn2 = reinterpret_cast<PFN_Present>(GetProcAddress(ld, "vkQueuePresentKHR"));
                HMODULE si = GetModuleHandleW(L"sl.interposer.dll");
                auto fn1 = si ? reinterpret_cast<PFN_Present>(GetProcAddress(si, "vkQueuePresentKHR"))
                              : nullptr;
                // Refuse to stack on someone else's detour. Chaining
                // trampolines is what produced black frames the last time a
                // hook went into the render path, so if the prologue is
                // already a jump, leave it alone and say so.
                bool clean = false;
                if (fn2 != nullptr) {
                    auto b = reinterpret_cast<const unsigned char *>(fn2);
                    clean = !(b[0] == 0xE9 || b[0] == 0xEB ||
                              (b[0] == 0xFF && b[1] == 0x25) ||
                              (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0));
                    if (!clean) log_line("loader present is already detoured; not stacking on it");
                }
                if (fn2 != nullptr && fn2 != fn1 && clean &&
                    MH_CreateHook(reinterpret_cast<void *>(fn2), reinterpret_cast<void *>(&hk_present2),
                                  reinterpret_cast<void **>(&g_orig_present2)) == MH_OK &&
                    MH_EnableHook(reinterpret_cast<void *>(fn2)) == MH_OK) {
                    log_line("loader present hooked too (src=1 rows)");
                } else {
                    g_orig_present2 = nullptr;
                    if (fn2 != nullptr && fn2 == fn1)
                        log_line("loader present is the same function; src=0 rows only");
                }
            }
        }
        // D3D12 games never reach the Vulkan branches above, so this is not in
        // the else of anything: both are attempted, and whichever applies wins.
        arm_dxgi_recorder();

        const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !was_down) {
            if (g_recording == 0) {
                g_nsamples = 0; g_written = 0;
                // Number each recording instead of overwriting the last one.
                // Comparing two settings means two runs back to back, and
                // deleting the file on every F9 threw the first one away --
                // it cost the pacer-off half of an A/B that had already been
                // played, and there is no way to get a run back once the
                // player has moved on.
                // Built from a stored base each time, never from the last
                // name -- appending to the previous one would grow
                // "-1-2-3.csv" run by run.
                if (g_frames_base[0] != 0) {
                    int k = 0;
                    while (g_frames_base[k] != 0 && k < MAX_PATH - 10) {
                        g_frames[k] = g_frames_base[k]; ++k;
                    }
                    while (k > 0 && g_frames[k - 1] != L'.') --k;   // sits after the dot
                    if (k > 1) {
                        ++g_run_no;
                        int d = k - 1;                              // on the dot
                        g_frames[d++] = L'-';
                        if (g_run_no >= 10) g_frames[d++] = (wchar_t)(L'0' + g_run_no / 10);
                        g_frames[d++] = (wchar_t)(L'0' + g_run_no % 10);
                        g_frames[d++] = L'.';
                        g_frames[d++] = L'c'; g_frames[d++] = L's'; g_frames[d++] = L'v';
                        g_frames[d] = 0;
                    }
                }
                DeleteFileW(g_frames);
                {
                    LARGE_INTEGER q0;
                    QueryPerformanceCounter(&q0);
                    g_rec_qpc0 = q0.QuadPart;
                }
                g_recording = 1;
                log_line("F9: recording started");
            } else {
                g_recording = 0;
                write_samples();
                log_num("F9: recording stopped, frames: ", (unsigned)g_nsamples);
            }
        }
        was_down = down;
        if (++ticks >= 20) { ticks = 0; if (g_recording) write_samples(); }
    }
    return 0;
}

// ------------------------------------------------- catching the dll load ---

// ---- make the CPU pacer run -------------------------------------------
//
// NVIDIA's own account of why multi-frame generation is Blackwell-only:
// "DLSS 3 Frame Generation used CPU-based pacing with variability that can
// compound with additional frames", and Blackwell moves that job into the
// display engine. The interesting part is what sl.dlss_g does on a card without
// that display engine.
//
// presentCommon opens its pacing block with
//
//     cmp  byte [ctx+0x4081], 0
//     jne  <past the whole wait loop>
//
// so when that flag is set the CPU pacer -- the loop that computes each
// generated frame's target time in microseconds and waits for it, spinning the
// last 2ms -- never runs at all. The flag is built in updateAppVSyncState as
//
//     flag = checkFullscreen() ? 0 : (flipMeteringAvailable && field >= 30)
//
// and under Vulkan checkFullscreen() starts by requiring RSync, which the log
// states outright is DX12 only. So on Vulkan the flag is always 1: Streamline
// hands pacing to the driver's flip metering and switches its own pacer off.
// On Ada that metering is not the hardware unit Blackwell added, which leaves
// the generated frames with neither pacer doing the job properly.
//
// The plugin already has a supported configuration where metering is off and
// the CPU pacer runs -- it takes it when it detects an FG1 dll. Clearing the
// flag selects that same path. `mov esi, r15d` above already leaves esi zero,
// so neutralising the cmov that would set it is enough:
//
//     0F 43 F1    cmovae esi, ecx   ->  90 90 90
//
// The same flag also pins the DLFG output count to 2, so this frees that too.


static int g_queue_mode = -1;


static int patch_queue_mode(unsigned char *base, int mode) {
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    auto *sec = IMAGE_FIRST_SECTION(nt);
    unsigned char *text = nullptr;
    size_t len = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const char *n = reinterpret_cast<const char *>(sec[i].Name);
        if (n[0] == '.' && n[1] == 't' && n[2] == 'e' && n[3] == 'x' && n[4] == 't') {
            text = base + sec[i].VirtualAddress;
            len = sec[i].Misc.VirtualSize;
            break;
        }
    }
    if (text == nullptr || mode < 0 || mode > 3) return 0;
    // mov ebx,[r14+0x4550] ; cmp byte [r14+0x4668],0
    static const unsigned char sig[15] = {
        0x41, 0x8B, 0x9E, 0x50, 0x45, 0x00, 0x00,
        0x41, 0x80, 0xBE, 0x68, 0x46, 0x00, 0x00, 0x00 };
    int hits = 0;
    for (size_t i = 0; i + sizeof sig <= len; ++i) {
        bool match = true;
        for (size_t k = 0; k < sizeof sig; ++k)
            if (text[i + k] != sig[k]) { match = false; break; }
        if (!match) continue;
        // mov ebx, imm32 is five bytes where the load took seven.
        unsigned char rep[7] = { 0xBB, (unsigned char)mode, 0, 0, 0, 0x90, 0x90 };
        DWORD old = 0;
        if (!VirtualProtect(text + i, 7, PAGE_EXECUTE_READWRITE, &old)) continue;
        for (int k = 0; k < 7; ++k) text[i + k] = rep[k];
        VirtualProtect(text + i, 7, old, &old);
        ++hits;
    }
    return hits;
}




// ---- run one pacer, not two --------------------------------------------
//
// Enabling the CPU pacer left the driver's flip metering running as well, so
// two mechanisms were spacing the same frames. The plugin has a configuration
// where metering is off and the CPU pacer does the work on its own: it selects
// it when an FG1 dll is present, logging "FG1 DLL has been detected: forcing
// flip-metering off". That is the DLSS 3 arrangement, the one Ada was built
// for, and it is reached by setting the same byte that path sets.
//
//     mov byte ptr [ctx+0x44A0], 0   ->   mov byte ptr [ctx+0x44A0], 1
//
// Measured on DOOM The Dark Ages at 4x: display intervals in place went from
// 88.8% to 94.3% and frames replaced before being shown from 0.1% to none.

static int patch_metering_off(unsigned char *base) {
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    auto *sec = IMAGE_FIRST_SECTION(nt);
    unsigned char *text = nullptr;
    size_t len = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const char *n = reinterpret_cast<const char *>(sec[i].Name);
        if (n[0] == '.' && n[1] == 't' && n[2] == 'e' && n[3] == 'x' && n[4] == 't') {
            text = base + sec[i].VirtualAddress;
            len = sec[i].Misc.VirtualSize;
            break;
        }
    }
    if (text == nullptr) return 0;
    // The field moved between plugin versions -- 0x44A0 in 2.11.1, 0x44F8 in
    // 2.12.129 -- so match the shape rather than one offset: a byte store of
    // zero into the context, followed by the cmp that reads the log-once guard.
    // Do not hardcode the offset -- it has moved every single version (0x44A0 in
    // 2.11.1, 0x44F8 in 2.12.129, 0x44F0 in 2.13.0). Read it out of the one
    // place that identifies the field beyond doubt: the flag computation, where
    // it is tested immediately before the `sete al` that feeds the pacer cmov.
    //
    //     cmp   edx, 0x1E                 83 FA 1E
    //     setae r9b                       41 0F 93 C1
    //     cmp   byte [r14+off], 0         41 80 BE off 00
    //     sete  al                        0F 94 C0
    unsigned field = 0;
    for (size_t i = 0; i + 22 <= len; ++i) {
        if (text[i] != 0x83 || text[i + 1] != 0xFA || text[i + 2] != 0x1E) continue;
        if (text[i + 3] != 0x41 || text[i + 4] != 0x0F || text[i + 5] != 0x93) continue;
        if (text[i + 7] != 0x41 || text[i + 8] != 0x80 || (text[i + 9] & 0xC7) != 0x86) continue;
        if (text[i + 14] != 0x00) continue;
        if (text[i + 15] != 0x0F || text[i + 16] != 0x94 || text[i + 17] != 0xC0) continue;
        field = *reinterpret_cast<const unsigned *>(text + i + 10);
        break;
    }
    // 2.11.1 phrases the flag computation differently and the anchor above does
    // not appear in it, so keep the offsets that were verified by hand on the
    // versions predating this matcher rather than regressing them.
    static const unsigned kKnown[] = { 0x44A0, 0x44F8 };
    if (field == 0) {
        for (unsigned k : kKnown) {
            for (size_t i = 0; i + 13 <= len && field == 0; ++i) {
                if (text[i] != 0xC6 || text[i + 1] != 0x83) continue;
                if (*reinterpret_cast<const unsigned *>(text + i + 2) != k) continue;
                if (text[i + 6] != 0x00) continue;
                if (text[i + 7] != 0x80 || text[i + 8] != 0x3D) continue;
                field = k;
            }
            if (field != 0) break;
        }
        if (field == 0) {
            // Streamline 2.7 through 2.10 phrase this differently and have no
            // guard byte to flip: they gate the NvAPI call on a mode word
            // instead. Avatar: Frontiers of Pandora ships 2.8.0, so the patch
            // reported "field not located" and flip metering stayed on --
            // confirmed in Streamline's own log, `FlipMetering = 1` and
            // `Achieved 'good' FC feedback state`, on a card whose display
            // engine has no multi-frame flip metering at all.
            //
            //     mov  r8d, 1              41 B8 01 00 00 00
            //     mov  r??, rcx            48 8B  modrm(11)
            //     cmp  eax, r8d            41 3B C0        <- mode == 1 ?
            //     jne  L                   0F 85 rel32     <- L skips metering
            //     mov  r9, [rcx+disp32]    4C 8B  ...      <- SetFlipConfig
            //     test r9, r9              4D 85 C9
            //     je   L2                  0F 84 rel32
            //
            // The mode word is only ever compared here, and r8 is reloaded
            // before any other use, so turning the immediate into 0 makes the
            // comparison fail always: the jne is taken every time and
            // NvAPI_D3D12_SetFlipConfig is never called. One byte, same
            // opcode, same length -- the rule this file already follows.
            //
            // Verified offline before shipping: exactly one site in 2.7.32,
            // 2.8.0, 2.9.0, 2.10.0 and 2.10.3, and none at all in 2.11.1,
            // 2.12.0 or 2.13.0, so the builds the block above already handles
            // stay byte-identical. Runs only when that block found nothing.
            int alt = 0;
            size_t where = 0;
            for (size_t i = 0; i + 34 <= len; ++i) {
                if (text[i] != 0x41 || text[i + 1] != 0xB8 || text[i + 2] != 0x01 ||
                    text[i + 3] != 0x00 || text[i + 4] != 0x00 || text[i + 5] != 0x00) continue;
                if (text[i + 6] != 0x48 || text[i + 7] != 0x8B ||
                    (text[i + 8] & 0xC0) != 0xC0) continue;
                if (text[i + 9] != 0x41 || text[i + 10] != 0x3B || text[i + 11] != 0xC0) continue;
                if (text[i + 12] != 0x0F || text[i + 13] != 0x85) continue;
                if (text[i + 18] != 0x4C || text[i + 19] != 0x8B) continue;
                if (text[i + 25] != 0x4D || text[i + 26] != 0x85 || text[i + 27] != 0xC9) continue;
                if (text[i + 28] != 0x0F || text[i + 29] != 0x84) continue;
                ++alt;
                where = i + 2;                     // the immediate byte
            }
            if (alt == 1) {
                unsigned char *imm = text + where;
                DWORD old = 0;
                if (VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &old)) {
                    imm[0] = 0x00;
                    VirtualProtect(imm, 1, old, &old);
                    return 1;
                }
            } else if (alt > 1) {
                log_num("    (2.8-era metering gate is ambiguous, sites: ", (unsigned)alt);
                log_line("     nothing touched)");
            }
            log_line("    (metering field not located; nothing touched)");
            return 0;
        }
    }
    // NVIDIA's 2026-09-03 build (see patch_enable_cpu_pacer) restructures this
    // whole area, and the anchor above genuinely does not appear in it -- not
    // matched, checked by direct disassembly of the file. There is a
    // plausible-looking `movzx ebx, byte [r14+off]` nearby, but "nearby" is
    // 293 bytes from the branchy flag computation it would need to feed, with
    // a *closer* (24 bytes) and structurally more connected candidate
    // reading a different field entirely -- and nothing here resolves which
    // one, if either, is the byte this function exists to protect. Guessing
    // and patching the wrong context offset is worse than doing nothing:
    // patch_enable_cpu_pacer's own fix for this build does not depend on
    // finding this field at all, so leaving this one reporting zero is the
    // honest state until that ambiguity is actually resolved.
    log_num("    metering field at ctx+", field);

    // Now the store that clears it. 2.11.1 and 2.12.129 write an immediate zero;
    // 2.13.0 writes a zeroed register instead, and that form is deliberately not
    // patched here -- in 2.13.0 the same byte also gates a second branch that has
    // not been analysed, and the pacer patch already forces the flag to 0 on its
    // own. Better to report the gap than to change a path we have not read.
    int hits = 0;
    for (size_t i = 0; i + 13 <= len; ++i) {
        if (text[i] != 0xC6 || text[i + 1] != 0x83) continue;    // mov byte [rbx+imm32], imm8
        if (*reinterpret_cast<const unsigned *>(text + i + 2) != field) continue;
        if (text[i + 6] != 0x00) continue;                        // storing zero
        if (text[i + 7] != 0x80 || text[i + 8] != 0x3D) continue; // cmp byte [rip+...], imm8
        unsigned char *imm = text + i + 6;
        DWORD old = 0;
        if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &old)) continue;
        *imm = 1;
        VirtualProtect(imm, 1, old, &old);
        ++hits;
    }
    if (hits != 0) return hits;

    // 2.13.0 stores a zeroed register instead of an immediate:
    //
    //     mov byte [rbx+field], dil      40 88 BB field
    //
    // There is no immediate to flip, so redirect the store to a scratch offset
    // instead -- the field then keeps whatever it held, and the value that
    // reaches it is never zero. Only rewrite the displacement, never the opcode,
    // and only when the byte written is a register the surrounding code is using
    // as its zero. The instruction stays exactly the same length.
    for (size_t i = 0; i + 7 <= len; ++i) {
        if (text[i] != 0x40 || text[i + 1] != 0x88) continue;      // REX + mov r/m8, r8
        if ((text[i + 2] & 0xC7) != 0x83) continue;                 // mod=10, rm=rbx
        if (*reinterpret_cast<const unsigned *>(text + i + 3) != field) continue;
        unsigned char *disp = text + i + 3;
        DWORD old = 0;
        if (!VirtualProtect(disp, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
        // +4 lands on the adjacent slot the flag computation never reads.
        *reinterpret_cast<unsigned *>(disp) = field + 4;
        VirtualProtect(disp, 4, old, &old);
        ++hits;
    }
    if (hits == 0)
        log_line("    (field found, but no store form we recognise -- "
                 "the pacer patch already pins the flag to 0)");
    return hits;
}

static int g_outputs_patched = 0;

static int patch_enable_cpu_pacer(unsigned char *base) {
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    auto *sec = IMAGE_FIRST_SECTION(nt);
    unsigned char *text = nullptr;
    size_t len = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const char *n = reinterpret_cast<const char *>(sec[i].Name);
        if (n[0] == '.' && n[1] == 't' && n[2] == 'e' && n[3] == 'x' && n[4] == 't') {
            text = base + sec[i].VirtualAddress;
            len = sec[i].Misc.VirtualSize;
            break;
        }
    }
    if (text == nullptr) return 0;

    //     sete   al                     0F 94 C0
    //     mov    <d>, r15d              41 8B  modrm(11 d 111)
    //     movzx  ecx, al                0F B6 C8
    //     cmp    edx, 0x1E              83 FA 1E
    //     cmovae <d>, ecx               0F 43  modrm(11 d 001)
    //
    // 2.11.1 built this with esi and 2.12.129 with edi, so a byte-for-byte
    // signature silently stopped matching on the Streamline update and the
    // patch reported zero sites for hours. Match the shape and read the
    // destination register out of the modrm, the way patch_metering_off already
    // does -- that one survived the same update untouched.
    int hits = 0;
    for (size_t i = 0; i + 15 <= len; ++i) {
        if (text[i] != 0x0F || text[i + 1] != 0x94 || text[i + 2] != 0xC0) continue;
        if (text[i + 3] != 0x41 || text[i + 4] != 0x8B) continue;
        const unsigned char m1 = text[i + 5];
        if ((m1 & 0xC7) != 0xC7) continue;                 // mod=11, rm=r15
        const unsigned reg = (m1 >> 3) & 7;
        if (text[i + 6] != 0x0F || text[i + 7] != 0xB6 || text[i + 8] != 0xC8) continue;
        if (text[i + 9] != 0x83 || text[i + 10] != 0xFA || text[i + 11] != 0x1E) continue;
        if (text[i + 12] != 0x0F || text[i + 13] != 0x43) continue;
        if (text[i + 14] != (unsigned char)(0xC0 | (reg << 3) | 1)) continue;  // same dest, ecx
        unsigned char *cmov = text + i + 12;        // the cmovae
        DWORD old = 0;
        if (!VirtualProtect(cmov, 3, PAGE_EXECUTE_READWRITE, &old)) continue;
        cmov[0] = 0x90; cmov[1] = 0x90; cmov[2] = 0x90;
        VirtualProtect(cmov, 3, old, &old);
        ++hits;
    }

    // NVIDIA pushed a new sl.dlss_g build through its OTA cache on 2026-09-03
    // (versions\134656\190_E658703.dll, up from versions\134273\190_E658703.dll
    // dated 2026-08-25) and rewrote this same computation from the cmovae form
    // above into two plain jumps. Confirmed by scanning both files' .text
    // directly off disk, independent of anything this proxy does at runtime:
    // the cmovae form above is the only one present in the Aug 25 build (one
    // site) and is entirely absent from the Sep 3 one; the pattern below is
    // the only one present in the Sep 3 build (one site) and is absent from
    // the Aug 25 one and from the copy Cyberpunk ships in its own bin\x64.
    //
    //     test bl, bl        84 DB
    //     jne  L             75 xx
    //     cmp  edi, 0x1E     83 FF 1E
    //     jb   L             72 yy      -- same target L as the jne above
    //     mov  bl, 1         B3 01
    //
    // bl already holds the guard byte's value on entry (0 = cleared) and edi
    // is feedbackCounter; `mov bl, 1` is the only place the combined flag
    // becomes 1, reached only once both branches have proven bl == 0. NOPing
    // it pins the flag at 0 the same way the cmovae-neutering above does on
    // the older build, just reached through jumps instead of a cmov -- and it
    // needs no knowledge of *where* the guard byte lives in the context
    // struct, unlike patch_metering_off's approach. That matters here: this
    // build's guard byte was not re-verified (see the comment on
    // patch_metering_off), so this patch, which does not depend on it, is the
    // one being shipped for this build.
    for (size_t i = 0; i + 11 <= len; ++i) {
        if (text[i] != 0x84 || text[i + 1] != 0xDB) continue;                    // test bl, bl
        if (text[i + 2] != 0x75) continue;                                      // jne rel8
        if (text[i + 4] != 0x83 || text[i + 5] != 0xFF || text[i + 6] != 0x1E) continue; // cmp edi,0x1E
        if (text[i + 7] != 0x72) continue;                                      // jb rel8
        if (text[i + 9] != 0xB3 || text[i + 10] != 0x01) continue;              // mov bl, 1
        const long t1 = (long)(i + 4) + (signed char)text[i + 3];
        const long t2 = (long)(i + 9) + (signed char)text[i + 8];
        if (t1 != t2) continue;   // jne and jb must share a target, or this isn't it
        unsigned char *imm = text + i + 9;
        DWORD old = 0;
        if (!VirtualProtect(imm, 2, PAGE_EXECUTE_READWRITE, &old)) continue;
        imm[0] = 0x90; imm[1] = 0x90;
        VirtualProtect(imm, 2, old, &old);
        ++hits;
    }
    if (hits != 0) return hits;

    // Fallback only, and deliberately so. Streamline 2.9-era builds phrase the
    // threshold test as a bare setcc rather than feeding a cmov or a pair of
    // jumps:
    //
    //     mov   <r32>, [ctx+counter]
    //     cmp   <r32>, 0x1E        [REX] 83 /7 1E
    //     setae <r8>               [REX] 0F 93 (mod=11)
    //     ...
    //     mov   byte [obj+disp], <r8>     ; the flag reaches its field here
    //
    // Zeroing the setcc pins the flag off, the same outcome as forms A and B.
    // The rewrite is `mov <r8>, 0` -- C6 /0 ib -- which is the same length as
    // setcc with or without a REX prefix, writes the same operand the setcc
    // wrote, and needs no register bookkeeping: /0 is an opcode extension, so
    // the modrm's reg field carries no register and REX.B keeps extending rm
    // exactly as it did. (xor r8,r8 would be shorter by one and would need
    // both REX.R and REX.B set to name an extended register twice.)
    //
    // Why fallback-only: this pair also occurs, exactly once, in every build
    // form A already handles (v2.10.0 through 134273) and in the one form B
    // handles. It is therefore a *different* site from the one those forms
    // patch -- a second place the same counter is compared -- and patching it
    // in a build that is already handled would be changing code we have not
    // read for no reason. Running only when nothing else matched keeps working
    // builds byte-identical to what they are today.
    //
    // Not verified: that the flag this feeds is the metering flag rather than
    // some other consumer of the same threshold. It is inferred from shape.
    // Hence also the exactly-one requirement below -- an ambiguous match is
    // reported and left alone rather than guessed at.
    {
        size_t found = 0, at = 0;
        for (size_t i = 0; i + 8 <= len; ++i) {
            size_t j = i;
            if (text[j] >= 0x40 && text[j] <= 0x4F) ++j;      // optional REX on the cmp
            if (text[j] != 0x83) continue;
            const unsigned char m = text[j + 1];
            if ((m & 0xC0) != 0xC0 || (m & 0x38) != 0x38) continue;   // mod=11, /7 = CMP
            if (text[j + 2] != 0x1E) continue;                        // imm8 == 30
            size_t k = j + 3;
            if (text[k] >= 0x40 && text[k] <= 0x4F) ++k;      // optional REX on the setcc
            if (text[k] != 0x0F || text[k + 1] != 0x93) continue;     // setae
            if ((text[k + 2] & 0xC0) != 0xC0) continue;               // register form
            ++found;
            at = k;                                            // the 0F, REX (if any) at at-1
        }
        if (found == 1) {
            unsigned char *op = text + at;                     // points at the 0F
            const unsigned char rm = (unsigned char)(op[2] & 7);
            DWORD old = 0;
            if (VirtualProtect(op, 3, PAGE_EXECUTE_READWRITE, &old)) {
                op[0] = 0xC6;                                  // mov r/m8, imm8
                op[1] = (unsigned char)(0xC0 | rm);            // mod=11, /0, same rm
                op[2] = 0x00;                                  // = 0
                VirtualProtect(op, 3, old, &old);
                ++hits;
            }
        } else if (found > 1) {
            log_num("    (threshold setcc is ambiguous, sites: ", (unsigned)found);
            log_line("     nothing touched)");
        }
    }

    // Form D, for Streamline 2.8.0 -- the build Avatar: Frontiers of Pandora
    // ships. None of the three forms above match it, so the proxy reported
    // "sites: 0" and concluded the build had no pacer at all. It has one:
    // 2.8.0 carries pacer.cpp with a thread of its own ("The pacer thread is
    // on the frame %llu, the dlfg thread is on the frame %llu"). Failing to
    // find the gate is not the same as there being no gate, and on that false
    // reading the proxy switched on a blocking fallback pacer of its own,
    // on top of NVIDIA's.
    //
    // Here the feedback counter is compared twice around its own increment,
    // inside a function that returns a bool:
    //
    //     cmp  eax, 0x1E          83 F8 1E
    //     jae  L                  0F 83 rel32
    //     inc  eax                FF C0
    //     mov  [rbx+disp32], eax  89 83 disp32
    //     cmp  eax, 0x1E          83 F8 1E
    //     jb   T                  0F 82 rel32   -- T is `mov al, 1`
    //
    // The second branch jumps straight at the function's true-exit, so the
    // byte to change is *derived from the displacement* rather than found by
    // its own signature: T is reached only from here, and `mov al, 1` on its
    // own is far too common a shape to match safely. `mov al, 0` there pins
    // the result false, which is the same thing forms A through C achieve by
    // other means -- the flag never flips mid-game, so the flushAll and DLFG
    // command-context switch that a flip triggers never happen.
    //
    // Not verified: that this bool is the metering flag rather than another
    // consumer of the same counter. It is inferred from the function calling
    // NvAPI_D3D12_SetFlipConfig and returning false when that call fails.
    // Same rule as form C, and for the same reason: exactly one match, or
    // nothing is touched.
    if (hits == 0) {
        size_t found = 0, target = 0;
        for (size_t i = 0; i + 24 <= len; ++i) {
            if (text[i] != 0x83 || text[i + 1] != 0xF8 || text[i + 2] != 0x1E) continue;
            if (text[i + 3] != 0x0F || text[i + 4] != 0x83) continue;      // jae rel32
            if (text[i + 9] != 0xFF || text[i + 10] != 0xC0) continue;     // inc eax
            if (text[i + 11] != 0x89) continue;                            // mov [r+d32], eax
            const unsigned char m = text[i + 12];
            if ((m & 0xC0) != 0x80 || ((m >> 3) & 7) != 0) continue;       // mod=10, reg=eax
            if (text[i + 17] != 0x83 || text[i + 18] != 0xF8 ||
                text[i + 19] != 0x1E) continue;                            // cmp eax, 0x1E
            if (text[i + 20] != 0x0F || text[i + 21] != 0x82) continue;    // jb rel32
            int rel = 0;
            memcpy(&rel, text + i + 22, 4);
            const size_t t = i + 26 + (size_t)(long long)rel;
            if (t + 2 > len) continue;
            if (text[t] != 0xB0 || text[t + 1] != 0x01) continue;          // mov al, 1
            ++found;
            target = t;
        }
        if (found == 1) {
            unsigned char *op = text + target;
            DWORD old = 0;
            if (VirtualProtect(op, 2, PAGE_EXECUTE_READWRITE, &old)) {
                op[1] = 0x00;                                  // mov al, 0
                VirtualProtect(op, 2, old, &old);
                ++hits;
            }
        } else if (found > 1) {
            log_num("    (2.8.0 counter form is ambiguous, sites: ", (unsigned)found);
            log_line("     nothing touched)");
        }
    }
    return hits;
}

struct DllNotifyData {
    ULONG Flags;
    const UNICODE_STRING *FullDllName;
    const UNICODE_STRING *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
typedef VOID(CALLBACK *PFN_Notify)(ULONG, const DllNotifyData *, PVOID);
typedef NTSTATUS(NTAPI *PFN_LdrRegister)(ULONG, PFN_Notify, PVOID, PVOID *);

static wchar_t lower(wchar_t c) { return (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c; }

static bool name_is(const UNICODE_STRING *s, const wchar_t *want) {
    if (s == nullptr || s->Buffer == nullptr) return false;
    const size_t n = s->Length / sizeof(wchar_t);
    size_t i = 0;
    for (; i < n && want[i] != 0; ++i)
        if (lower(s->Buffer[i]) != lower(want[i])) return false;
    return i == n && want[i] == 0;
}

// The snippet does not always arrive under the name nvngx_dlssg.dll. NGX keeps
// OTA-updated snippets under ProgramData as <arch>_<appid>.bin, and that copy is
// usually the newest, so it is the one NGX actually uses -- matching on the file
// name alone patches the two copies that get discarded and misses the one that
// counts. Match on the whole path instead. "dlssg" appears in both
// nvngx_dlssg.dll and ...\NGX\models\dlssg\..., and not in sl.dlss_g.dll, which
// spells it with an underscore.
static bool path_has(const UNICODE_STRING *s, const wchar_t *needle) {
    if (s == nullptr || s->Buffer == nullptr) return false;
    const size_t n = s->Length / sizeof(wchar_t);
    size_t want = 0;
    while (needle[want] != 0) ++want;
    if (want == 0 || n < want) return false;
    for (size_t i = 0; i + want <= n; ++i) {
        size_t j = 0;
        while (j < want && lower(s->Buffer[i + j]) == lower(needle[j])) ++j;
        if (j == want) return true;
    }
    return false;
}

// Narrow a wide path for the log; these are all ASCII in practice.
static void log_wide(const char *label, const UNICODE_STRING *s) {
    char buf[512];
    int i = 0;
    while (label[i] != 0 && i < 120) { buf[i] = label[i]; ++i; }
    const size_t n = s ? s->Length / sizeof(wchar_t) : 0;
    for (size_t k = 0; k < n && i < 500; ++k, ++i) {
        wchar_t c = s->Buffer[k];
        buf[i] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    buf[i] = 0;
    log_line(buf);
}

static VOID CALLBACK on_dll_load(ULONG reason, const DllNotifyData *d, PVOID) {
    if (reason != 1 || d == nullptr) return;                 // 1 = LDR_DLL_NOTIFICATION_REASON_LOADED

    // Load order matters for reading the Streamline log afterwards: the plugin
    // caches the maximum at its startup, so it has to come after the snippet.
    if (name_is(d->BaseDllName, L"sl.interposer.dll")) { log_line("sl.interposer.dll mapped"); return; }
    // Match the path, not the file name. Streamline prefers a newer plugin from
    // NVIDIA's OTA cache when it finds one -- Cyberpunk 2077 loads
    // ...\NGX\models\sl_dlss_g_0\versions\<id>\files\190_E658703.dll and leaves
    // the copy in its own folder unused. Matching "sl.dlss_g.dll" then patches the
    // file that never runs and reports zero sites on the one that does, which is
    // exactly the trap the snippet already taught us. "sl_dlss_g" catches the cache
    // layout and "sl.dlss_g" the game-folder one; patching both is harmless, since
    // a signature either matches an image or leaves it alone.
    if (path_has(d->FullDllName, L"sl.dlss_g") ||
        path_has(d->FullDllName, L"sl_dlss_g")) {
        log_line("sl.dlss_g mapped");
        log_wide("  in ", d->FullDllName);
        const int n = patch_enable_cpu_pacer(reinterpret_cast<unsigned char *>(d->DllBase));
        g_outputs_patched += n;
        log_num("  CPU pacer enabled, sites: ", (unsigned)n);
        if (g_meter_off) {
            const int mo = patch_metering_off(reinterpret_cast<unsigned char *>(d->DllBase));
            log_num("  driver flip metering switched off, sites: ", (unsigned)mo);
        }
        if (g_queue_mode >= 0) {
            const int q = patch_queue_mode(reinterpret_cast<unsigned char *>(d->DllBase), g_queue_mode);
            log_num("  queue parallelism mode forced to ", (unsigned)g_queue_mode);
            log_num("    sites: ", (unsigned)q);
        }
        // One verdict per module, not one per process: this callback runs for
        // every copy of the plugin that maps, and they do not all get the same
        // result -- the game-folder copy and NVIDIA's OTA copy have reported
        // different site counts in the same session. An aggregate would average
        // that into a number belonging to neither.
        // Nothing matched here, so this build has no pacer for us to enable --
        // in 2.8.0 and 2.9 there is not one to enable at all. Rather than leave
        // the generated frames to go out four-at-a-time, space them in the
        // present hook. One plugin finding sites is enough to call it off: the
        // native one is better than ours and they must not both run.
        // Sticky, and deliberately: this callback runs once per copy of the
        // plugin that maps, and a game can map two of different vintages. A
        // plain assignment let the last one win, so a patched OTA copy mapping
        // before the game's own stale one would leave our pacer switched on in
        // a process that already had NVIDIA's. One copy with sites is enough
        // to settle it for the process.
        if (n > 0) {
            g_native_pacer_found = true;
            log_line("  => pacer OK");
        } else if (!g_native_pacer_found) {
            // Said "spacing generated frames ourselves" until 2026-09-04,
            // when the fallback pacer that would have done so was removed: it
            // never changed anything a player could see, and once a form of
            // the pacer patch existed for every Streamline family on disk it
            // could no longer engage at all. What is left is a warning, and it
            // is worth keeping -- a build with no site here is one nobody has
            // looked at yet.
            log_line("  => no pacer site found in this build -- 4x may not be paced");
        } else {
            log_line("  => no pacer here, but another copy has one -- leaving it to that");
        }
        return;
    }
    if (name_is(d->BaseDllName, L"_nvngx.dll") || name_is(d->BaseDllName, L"nvngx.dll")) {
        log_line(name_is(d->BaseDllName, L"nvngx.dll") ? "nvngx.dll mapped"
                                                       : "_nvngx.dll mapped");
        return;
    }

    if (!path_has(d->FullDllName, L"dlssg")) return;
    // A build sitting in the OTA cache is not proof NGX will load it. Avatar:
    // Frontiers of Pandora has 20318464 cached and maps only its own copy --
    // the same way GTA V's sl.log says "OTA'd plugins will not be loaded!",
    // it is the game's decision, not ours to predict. Assuming the cache wins
    // made the verdict call the copy that actually ran a shadow and tell the
    // reader to ignore it: the precise failure the verdict exists to prevent,
    // in the opposite direction.
    //
    // So claim nothing until the OTA copy actually maps. Until then a copy is
    // treated as possibly-live and reports in full; once the OTA build has
    // been seen, anything else really is redundant and can say so.
    const bool is_ota = g_ota_newest[0] != 0 && path_has(d->FullDllName, g_ota_newest);
    if (is_ota) g_ota_mapped = true;
    const bool shadow = !is_ota && g_ota_mapped;
    const bool live = !shadow;
    const int n = patch_gates(reinterpret_cast<unsigned char *>(d->DllBase));
    if (n > 0) ++g_gates;
    log_num("  gates rewritten: ", (unsigned)n);
    if (g_preset_b) {
        const int pb = patch_preset_b(reinterpret_cast<unsigned char *>(d->DllBase));
        log_num("  interpolation preset B (UIR) forced, sites: ", (unsigned)pb);
    }
    // Only the copy NGX actually loads is worth rebuilding, and it is the one
    // whose gates took: the driver-store fallback reports 0 and is never used.
    if (g_cubins && n > 0) {
        const int cb = patch_cubins(reinterpret_cast<unsigned char *>(d->DllBase), live);
        g_cubins_done += cb;
        log_num("  kernels rebuilt from the Blackwell PTX: ", (unsigned)cb);
        if (shadow) {
            log_line("  => redundant: the OTA build already mapped, this copy is unused");
        } else if (cb == 0) {
            // Was `cb != 3`, from when the header held one build's three
            // kernels. It now covers several builds at once and a snippet
            // takes only its own subset -- Avatar's 310.3 has five where
            // 310.9 has three -- so a fixed count reported "NOT APPLIED"
            // directly under a line saying five had been rebuilt. Nothing had
            // gone wrong; the verdict was measuring the wrong thing. Zero is
            // the only count that means failure.
            //
            // The failure this exists for is silent in play: the unlock still
            // works, the frames still generate, they are just slower kernels.
            // Nothing errors, so say the fix out loud rather than leaving a
            // count to be recognised as wrong.
            log_line("  => CUBINS NOT APPLIED -- unlock works, but 4x will judder");
            log_line("     cubins.h was built for dlssg build:");
            log_line(kCubinsBuiltFor);
            log_line("     the snippet loaded here is the path below; if they differ,");
            log_line("     rerun: python tools/rebuild_cubins.py && sh build-proxy.sh");
        } else {
            log_line("  => gates and cubins OK");
        }
    } else if (n > 0) {
        log_line(shadow ? "  => redundant: the OTA build already mapped, this copy is unused"
                        : "  => gates OK");
    }
    log_wide("  in ", d->FullDllName);
}

// ------------------------------------------------------------ forwarding ---
//
// Resolved lazily rather than in DllMain: calling LoadLibrary under the loader
// lock is how proxies deadlock.

static HMODULE g_real = nullptr;

static FARPROC real(const char *name) {
    if (g_real == nullptr) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 16) return nullptr;
        const wchar_t *tail = L"\\version.dll";
        for (UINT i = 0; tail[i] != 0; ++i) path[n + i] = tail[i];
        path[n + 12] = 0;
        g_real = LoadLibraryW(path);
        if (g_real == nullptr) return nullptr;
    }
    return GetProcAddress(g_real, name);
}

#define FORWARD(ret, name, params, args)                                  \
    extern "C" __declspec(dllexport) ret WINAPI name params {             \
        using fn = ret(WINAPI *) params;                                  \
        auto p = reinterpret_cast<fn>(real(#name));                       \
        return p ? p args : (ret)0;                                       \
    }

FORWARD(BOOL,  GetFileVersionInfoA,       (LPCSTR a, DWORD b, DWORD c, LPVOID d), (a,b,c,d))
FORWARD(BOOL,  GetFileVersionInfoW,       (LPCWSTR a, DWORD b, DWORD c, LPVOID d), (a,b,c,d))
FORWARD(BOOL,  GetFileVersionInfoExA,     (DWORD f, LPCSTR a, DWORD b, DWORD c, LPVOID d), (f,a,b,c,d))
FORWARD(BOOL,  GetFileVersionInfoExW,     (DWORD f, LPCWSTR a, DWORD b, DWORD c, LPVOID d), (f,a,b,c,d))
FORWARD(DWORD, GetFileVersionInfoSizeA,   (LPCSTR a, LPDWORD b), (a,b))
FORWARD(DWORD, GetFileVersionInfoSizeW,   (LPCWSTR a, LPDWORD b), (a,b))
FORWARD(DWORD, GetFileVersionInfoSizeExA, (DWORD f, LPCSTR a, LPDWORD b), (f,a,b))
FORWARD(DWORD, GetFileVersionInfoSizeExW, (DWORD f, LPCWSTR a, LPDWORD b), (f,a,b))
FORWARD(BOOL,  VerQueryValueA,            (LPCVOID a, LPCSTR b, LPVOID *c, PUINT d), (a,b,c,d))
FORWARD(BOOL,  VerQueryValueW,            (LPCVOID a, LPCWSTR b, LPVOID *c, PUINT d), (a,b,c,d))
// These four take non-const strings in winver.h; match it or the declarations clash.
FORWARD(DWORD, VerFindFileA,              (DWORD a, LPSTR b, LPSTR c, LPSTR d, LPSTR e, PUINT f, LPSTR g, PUINT h), (a,b,c,d,e,f,g,h))
FORWARD(DWORD, VerFindFileW,              (DWORD a, LPWSTR b, LPWSTR c, LPWSTR d, LPWSTR e, PUINT f, LPWSTR g, PUINT h), (a,b,c,d,e,f,g,h))
FORWARD(DWORD, VerInstallFileA,           (DWORD a, LPSTR b, LPSTR c, LPSTR d, LPSTR e, LPSTR f, LPSTR g, PUINT h), (a,b,c,d,e,f,g,h))
FORWARD(DWORD, VerInstallFileW,           (DWORD a, LPWSTR b, LPWSTR c, LPWSTR d, LPWSTR e, LPWSTR f, LPWSTR g, PUINT h), (a,b,c,d,e,f,g,h))
FORWARD(DWORD, VerLanguageNameA,          (DWORD a, LPSTR b, DWORD c), (a,b,c))
FORWARD(DWORD, VerLanguageNameW,          (DWORD a, LPWSTR b, DWORD c), (a,b,c))
FORWARD(BOOL,  GetFileVersionInfoByHandle,(DWORD a, HANDLE b, DWORD c, LPVOID d), (a,b,c,d))

// Highest-numbered directory under the OTA cache, which is the build NGX picks.
// Names are decimal ids, so "20318464" beats "20318081"; compared by length
// first so a shorter number never wins on lexical order alone.
static void find_newest_ota_build() {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(
        L"C:\\ProgramData\\NVIDIA\\NGX\\models\\dlssg\\versions\\*", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        int n = 0;
        while (fd.cFileName[n] != 0 && n < 63) {
            if (fd.cFileName[n] < L'0' || fd.cFileName[n] > L'9') { n = -1; break; }
            ++n;
        }
        if (n <= 0) continue;
        int cur = 0;
        while (g_ota_newest[cur] != 0) ++cur;
        bool better = cur == 0 || n > cur;
        if (!better && n == cur) {
            for (int i = 0; i < n; ++i) {
                if (fd.cFileName[i] != g_ota_newest[i]) {
                    better = fd.cFileName[i] > g_ota_newest[i];
                    break;
                }
            }
        }
        if (better) {
            for (int i = 0; i < n; ++i) g_ota_newest[i] = fd.cFileName[i];
            g_ota_newest[n] = 0;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------------------------------------------------------------- attach ---

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);

    // Log beside this dll, in the game folder.
    DWORD n = GetModuleFileNameW(self, g_log, MAX_PATH);
    while (n > 0 && g_log[n - 1] != L'\\') --n;
    find_newest_ota_build();
    const wchar_t *name = L"mfg-unlock.log";
    if (n + 15 < MAX_PATH) {
        for (int i = 0; name[i] != 0; ++i) g_log[n + i] = name[i];
        g_log[n + 14] = 0;
    } else {
        g_log[0] = 0;
    }
    {
        int k = 0;
        while (g_log[k] != 0 && k < MAX_PATH - 1) { g_frames[k] = g_log[k]; ++k; }
        while (k > 0 && g_frames[k - 1] != 0x5C) --k;
        const wchar_t *fn = L"mfg-frames.csv";
        for (int i = 0; fn[i] != 0; ++i) g_frames[k + i] = fn[i];
        g_frames[k + 14] = 0;
        for (int i = 0; i < MAX_PATH; ++i) g_frames_base[i] = g_frames[i];
        {
            wchar_t pb[MAX_PATH];
            int j = 0;
            while (g_frames[j] != 0 && j < MAX_PATH - 1) { pb[j] = g_frames[j]; ++j; }
            while (j > 0 && pb[j - 1] != 0x5C) --j;
            const wchar_t *pn = L"mfg-presetb.txt";
            for (int i = 0; pn[i] != 0; ++i) pb[j + i] = pn[i];
            pb[j + 15] = 0;
            g_preset_b = GetFileAttributesW(pb) != INVALID_FILE_ATTRIBUTES;
            // On by default now, off with mfg-nocubins.txt. It used to be the
            // other way round, from when this was believed to be a speed
            // optimisation -- the script that builds these kernels said in so
            // many words "a speed change, not an image change", and that was
            // never verified. It is wrong. Avatar: Frontiers of Pandora at 4x
            // judders on camera movement without these kernels and is fluid
            // with them, same build, same snippet, same settings, measured
            // both ways after nine other explanations had been tried and
            // discarded. The mvec-estimate kernel is the one that matters,
            // which fits: it is what camera motion gets reconstructed from.
            //
            // Left opt-in, it would have reached nobody. The person installing
            // this has the DLL and nothing else, so the thing that makes 4x
            // usable cannot sit behind a file they have to create.
            const wchar_t *cn = L"mfg-nocubins.txt";
            for (int i = 0; cn[i] != 0; ++i) pb[j + i] = cn[i];
            pb[j + 16] = 0;
            g_cubins = GetFileAttributesW(pb) == INVALID_FILE_ATTRIBUTES;
            // g_meter_off was declared and read but never assigned, so
            // mfg-nometer.txt did nothing at all.
            const wchar_t *mn = L"mfg-nometer.txt";
            for (int i = 0; mn[i] != 0; ++i) pb[j + i] = mn[i];
            pb[j + 15] = 0;
            g_meter_off = GetFileAttributesW(pb) != INVALID_FILE_ATTRIBUTES;
        }
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        g_qpc_freq = f.QuadPart ? f.QuadPart : 1;
        g_samples = static_cast<Sample *>(VirtualAlloc(nullptr, sizeof(Sample) * kMaxSamples,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        HANDLE th = CreateThread(nullptr, 0, &recorder, nullptr, 0, nullptr);
        if (th != nullptr) CloseHandle(th);
    }
    log_line("--- mfg-unlock attached ---  (F9 records)");

    // Turn on Streamline's own logging from here, before anything loads it, so
    // the decisive line arrives without the user editing launch options:
    //   "Multi-frame <state>, max generated frames A
    //    (SL Plugin supports B, NGX feature supports C)"
    // C is what the snippet reports -- our patch -- and A is what survives the
    // plugin's own cap and any driver-profile limit. The three numbers say
    // exactly where the chain stops.
    // Opt in, because level 2 is not free: it writes several megabytes of sl.log
    // beside the game every session, and this used to be switched on for
    // everyone who installed the dll -- our debugging shipped as everyone's
    // disk writes. Off unless mfg-sllog.txt is there. Kept separate from
    // mfg-indicator.txt so the on-screen multiplier can stay on without
    // dragging the log back with it.
    {
        wchar_t p[MAX_PATH];
        int j = 0;
        while (g_log[j] != 0 && j < MAX_PATH - 1) { p[j] = g_log[j]; ++j; }
        while (j > 0 && p[j - 1] != 0x5C) --j;
        const wchar_t *fn = L"mfg-sllog.txt";
        for (int i = 0; fn[i] != 0; ++i) p[j + i] = fn[i];
        p[j + 13] = 0;
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
            wchar_t dir[MAX_PATH];
            DWORD m = GetModuleFileNameW(self, dir, MAX_PATH);
            while (m > 0 && dir[m - 1] != L'\\') --m;
            if (m > 1) { dir[m - 1] = 0; SetEnvironmentVariableW(L"SL_LOG_PATH", dir); }
            SetEnvironmentVariableW(L"SL_LOG_LEVEL", L"2");
            SetEnvironmentVariableW(L"SL_ENABLE_CONSOLE_LOGGING", L"0");
            log_line("streamline logging enabled (sl.log lands beside this dll)");
        } else {
            SetEnvironmentVariableW(L"SL_LOG_LEVEL", L"0");
        }
    }

    // The snippet carries its own diagnostic overlay, drawn by the
    // cuda_font_kernel that sits in its .data next to the network kernels.
    // NGXCubinGeneric::Init reads __NGX_SHOW_INDICATOR, and then compares it
    // against 0x400 and zeroes anything else -- so 1024 is the only value that
    // turns it on. This is NVIDIA's instrumentation, not ours: no patch, no
    // hook, nothing of ours in the render path. Opt in by dropping
    // mfg-indicator.txt beside this dll.
    {
        wchar_t p[MAX_PATH];
        int j = 0;
        while (g_log[j] != 0 && j < MAX_PATH - 1) { p[j] = g_log[j]; ++j; }
        while (j > 0 && p[j - 1] != 0x5C) --j;
        const wchar_t *fn = L"mfg-indicator.txt";
        for (int i = 0; fn[i] != 0; ++i) p[j + i] = fn[i];
        p[j + 17] = 0;
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
            SetEnvironmentVariableW(L"__NGX_SHOW_INDICATOR", L"1024");
            SetEnvironmentVariableW(L"__NGX_LOG_LEVEL", L"2");
            if (j > 1) {
                wchar_t d[MAX_PATH];
                for (int i = 0; i < j - 1; ++i) d[i] = p[i];
                d[j - 1] = 0;
                SetEnvironmentVariableW(L"__NGX_LOG_PATH_OVERRIDE", d);
            }
            log_line("NGX indicator on (__NGX_SHOW_INDICATOR=1024) + snippet log");
        }
    }

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto reg = reinterpret_cast<PFN_LdrRegister>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    if (reg == nullptr) {
        log_line("LdrRegisterDllNotification is unavailable; nothing will be patched");
        return TRUE;
    }
    PVOID cookie = nullptr;
    if (reg(0, &on_dll_load, nullptr, &cookie) == 0) {
        log_line("armed, waiting for nvngx_dlssg.dll");
        if (g_ota_newest[0] != 0) {
            char b[96] = "NGX will load OTA build ";
            int k = 24;
            for (int i = 0; g_ota_newest[i] != 0 && k < 90; ++i, ++k)
                b[k] = (char)g_ota_newest[i];   // decimal digits, ASCII either way
            b[k] = 0;
            log_line(b);
            log_line("  (if it maps, copies that map after it are redundant; some games");
            log_line("   never load the cached build and run their own -- watch which one appears)");
        } else {
            log_line("no OTA cache found; the game's own dlssg copy is the one that runs");
        }
    } else {
        log_line("failed to arm the loader callback");
    }

    // If something loaded the snippet before this proxy did, catch it anyway.
    HMODULE already = GetModuleHandleW(L"nvngx_dlssg.dll");
    if (already != nullptr) {
        const int hits = patch_gates(reinterpret_cast<unsigned char *>(already));
        log_num("snippet was already loaded; gates rewritten: ", (unsigned)hits);
        log_line("(if frame generation still caps at 2x, this proxy loaded too late)");
    }
    return TRUE;
}
