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

static int patch_cubins(unsigned char *base) {
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
                            if (c.orig_size != (unsigned)psz) continue;   // must be the same slot
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
static LONG g_written = 0;
static const int kMaxSamples = 200000;
static long long g_qpc_freq = 1;
static wchar_t g_frames[MAX_PATH];

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
        const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !was_down) {
            if (g_recording == 0) {
                g_nsamples = 0; g_written = 0;
                DeleteFileW(g_frames);
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
        if (field == 0) { log_line("    (metering field not located; nothing touched)"); return 0; }
    }
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
        return;
    }
    if (name_is(d->BaseDllName, L"_nvngx.dll"))        { log_line("_nvngx.dll mapped"); return; }

    if (!path_has(d->FullDllName, L"dlssg")) return;
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
        const int cb = patch_cubins(reinterpret_cast<unsigned char *>(d->DllBase));
        g_cubins_done += cb;
        log_num("  kernels rebuilt from the Blackwell PTX: ", (unsigned)cb);
        if (cb != 3) log_line("  (expected 3 -- snippet version may have changed; the rest are untouched)");
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

// ---------------------------------------------------------------- attach ---

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);

    // Log beside this dll, in the game folder.
    DWORD n = GetModuleFileNameW(self, g_log, MAX_PATH);
    while (n > 0 && g_log[n - 1] != L'\\') --n;
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
        {
            wchar_t pb[MAX_PATH];
            int j = 0;
            while (g_frames[j] != 0 && j < MAX_PATH - 1) { pb[j] = g_frames[j]; ++j; }
            while (j > 0 && pb[j - 1] != 0x5C) --j;
            const wchar_t *pn = L"mfg-presetb.txt";
            for (int i = 0; pn[i] != 0; ++i) pb[j + i] = pn[i];
            pb[j + 15] = 0;
            g_preset_b = GetFileAttributesW(pb) != INVALID_FILE_ATTRIBUTES;
            const wchar_t *cn = L"mfg-cubins.txt";
            for (int i = 0; cn[i] != 0; ++i) pb[j + i] = cn[i];
            pb[j + 14] = 0;
            g_cubins = GetFileAttributesW(pb) != INVALID_FILE_ATTRIBUTES;
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
    {
        wchar_t dir[MAX_PATH];
        DWORD m = GetModuleFileNameW(self, dir, MAX_PATH);
        while (m > 0 && dir[m - 1] != L'\\') --m;
        if (m > 1) { dir[m - 1] = 0; SetEnvironmentVariableW(L"SL_LOG_PATH", dir); }
        SetEnvironmentVariableW(L"SL_LOG_LEVEL", L"2");
        SetEnvironmentVariableW(L"SL_ENABLE_CONSOLE_LOGGING", L"0");
        log_line("streamline logging enabled (sl.log lands beside this dll)");
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
    if (reg(0, &on_dll_load, nullptr, &cookie) == 0)
        log_line("armed, waiting for nvngx_dlssg.dll");
    else
        log_line("failed to arm the loader callback");

    // If something loaded the snippet before this proxy did, catch it anyway.
    HMODULE already = GetModuleHandleW(L"nvngx_dlssg.dll");
    if (already != nullptr) {
        const int hits = patch_gates(reinterpret_cast<unsigned char *>(already));
        log_num("snippet was already loaded; gates rewritten: ", (unsigned)hits);
        log_line("(if frame generation still caps at 2x, this proxy loaded too late)");
    }
    return TRUE;
}
