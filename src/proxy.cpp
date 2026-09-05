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
// Set the first time this process actually presents a frame. Games ship
// helper executables in the same folder -- crash handlers, error reporters --
// and they load a proxy named version.dll exactly like the game does.
static volatile LONG g_presents_seen = 0;
// Set from mfg-debug.txt at startup. Everything it turns on writes to the log
// from a hot path, so none of it may be on in a shipped build.
static bool g_debug = false;

// True when a file of this name sits next to the dll. The switches are
// files rather than a config format because the person installing this has
// the dll and nothing else, and creating an empty file is the least we can
// ask of them.
static bool flag_file(const wchar_t *name) {
    wchar_t p[MAX_PATH];
    int j = 0;
    while (g_log[j] != 0 && j < MAX_PATH - 1) { p[j] = g_log[j]; ++j; }
    while (j > 0 && p[j - 1] != 0x5C) --j;      // back over the file name
    int i = 0;
    while (name[i] != 0 && j + i < MAX_PATH - 1) { p[j + i] = name[i]; ++i; }
    p[j + i] = 0;                                // terminated by measurement
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

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


// ---- overriding the multiplier the game asked for ------------------------
//
// The game picks its own multiplier and we take whatever it picked. To force
// one, the value has to change before Streamline sees it, and the place it
// arrives is `slDLSSGSetOptions(viewport, options)`.
//
// That function is not a DLL export. Streamline hands it out through
// `slGetFeatureFunction(feature, name, fn)`, which *is* exported by
// sl.interposer.dll, so the game asks for it by name and caches the pointer.
// Hooking the lookup and handing back our own wrapper is enough: one hook, on
// a public and stable API rather than a byte signature, on a path called when
// settings change rather than per frame.
//
// The struct is versioned and self-identifying, which is what makes writing
// into it safe. From include/sl_struct.h and include/sl_dlss_g.h:
//
//     BaseStructure   next(8) structType(16) structVersion(8)   = 32 bytes
//     DLSSGOptions    mode @ +32, numFramesToGenerate @ +36
//
// and structType is a fixed guid, fac5f1cb-2dfd-4f36-a1e6-3a9e865256c5. It is
// identical in 2.8.0 (struct version 3) and 2.12.0 (version 5) -- the whole
// range this proxy patches -- and the guid is checked before a byte is
// written, so a layout change means the override quietly does nothing instead
// of corrupting whatever else lives at +36.
//
// numFramesToGenerate is frames *generated*, not the multiplier: 1 is 2x,
// 2 is 3x, 3 is 4x.

static const unsigned char kDlssgOptionsGuid[16] = {
    0xcb, 0xf1, 0xc5, 0xfa,             // 0xfac5f1cb, little endian
    0xfd, 0x2d,                         // 0x2dfd
    0x36, 0x4f,                         // 0x4f36
    0xa1, 0xe6, 0x3a, 0x9e, 0x86, 0x52, 0x56, 0xc5
};

// 0 AUTO, 1 OFF, 2..4 = 2x/3x/4x. DLSSGOptions carries the mode at +32
// (DLSSGMode: eOff 0, eOn 1, eAuto 2) and the generated-frame count at +36,
// so switching frame generation off is a different field from choosing a
// multiplier -- writing a count of zero would not do it.
// Streamline tells us, in the struct itself, whether eDynamic exists. The
// version sits at +24 of every sl structure; DLSSGOptions reached version 5
// in 2.11.1, which is where DLSSGMode::eDynamic and dynamicTargetFrameRate
// were added. On 2.8.0 (version 3) a mode of 3 is eCount -- an invalid value,
// not dynamic -- and the struct does not even extend to +116. So the row is
// offered only when the game's own struct says it can be.
static volatile LONG g_opts_version = 0;
// Whether this Streamline knows DLSSGMode::eDynamic at all, decided from the
// plugin's own image rather than from the first slDLSSGSetOptions the game
// happens to make. Waiting for that call left the row greyed out on a build
// that supports it, for as long as the game had not touched its settings.
static bool g_dynamic_known = false;

// Looks for a literal anywhere in a mapped image.
static bool image_has(unsigned char *base, const char *needle) {
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const size_t n = nt->OptionalHeader.SizeOfImage;
    const size_t m = strlen(needle);
    if (m == 0 || n < m) return false;
    for (size_t i = 0; i + m <= n; ++i)
        if (base[i] == (unsigned char)needle[0] && memcmp(base + i, needle, m) == 0)
            return true;
    return false;
}
// The frame rate eDynamic aims at, in whole frames per second; 0 means
// "the display's refresh rate", which is what NVIDIA's own app calls Max
// refresh rate.
//
// DLSSGOptions::dynamicTargetFrameRate sits at +116 and is a **float**. The
// offset was carried in a note with no evidence behind it and the type was
// never established at all, so both were checked before anything was written
// there. Two independent sources agree:
//
//   * sl.dlss_g.dll reports the value with `movss xmm2, dword ptr [r15+0x30]`
//     into the vtable slot it uses for floats (+0x30; integers go through
//     +0x20 and strings through +0x18), under the tag DLSSG.TargetFrameRate.
//   * NVIDIA's published sl_dlss_g.h declares `float dynamicTargetFrameRate{}`
//     as the last member added in kStructVersion5, and laying the struct out
//     from the 32-byte base lands it at exactly 116 with sizeof 120 -- which
//     also reproduces the two offsets already known to be right, mode at 32
//     and numFramesToGenerate at 36.
//
// This mattered: writing 60 as an integer into a float field gives 8.4e-44,
// a denormal indistinguishable from zero -- so the wrong guess would have
// read on screen as a working target while silently meaning "auto".
static volatile LONG g_dyn_target = 0;

static volatile LONG g_force_sel = 0;
static LONG g_saved_mode = 0;
static volatile LONG g_force_generated = 0;   // kept: 0 = leave the game alone
static volatile LONG g_last_seen_generated = 0;
static bool g_override_said = false;

typedef unsigned (*PFN_slDLSSGSetOptions)(const void *, const void *);
typedef unsigned (*PFN_slGetFeatureFunction)(unsigned, const char *, void *&);
static PFN_slDLSSGSetOptions g_orig_setoptions = nullptr;
static PFN_slGetFeatureFunction g_orig_getfeaturefn = nullptr;

// Enough of the last call to make it again ourselves. The header says
// slDLSSGSetOptions is not thread safe, so the thread the game used is
// recorded with it and the replay only happens on that same thread -- from
// the present hook, which the game enters every frame.
static unsigned char g_opt_copy[256];
static unsigned char g_vp_copy[64];
static volatile LONG g_opt_have = 0;
static volatile LONG g_opt_thread = 0;
static volatile LONG g_opt_pending = 0;

// Copies up to `want` bytes without running off the end of the page the
// struct sits in: the real size varies by struct version and reading past a
// page boundary would fault.
static unsigned copy_bounded(void *dst, const void *src, unsigned want) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(src, &mbi, sizeof(mbi)) == 0) return 0;
    const unsigned char *end = (const unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    unsigned avail = (unsigned)(end - (const unsigned char *)src);
    if (avail > want) avail = want;
    memcpy(dst, src, avail);
    return avail;
}

// Applies the current selection to a live options struct, returning what was
// there so the caller can put it back.
// Set when force_into wrote the frame-rate target, so the restore afterwards
// puts back exactly the fields that were touched and no others.
static bool g_target_written = false;
static float g_saved_target = 0.0f;
// 0 nothing said yet, 1 written, 2 declined. Reset when the target changes so
// a new value gets its own line, and guarded so this never logs per frame:
// log_line opens and closes the file on every call.
static int g_dyn_said = 0;

static void force_into(unsigned char *p, LONG *savedMode, LONG *savedCount) {
    *savedMode = *(LONG *)(p + 32);
    *savedCount = *(LONG *)(p + 36);
    g_target_written = false;
    const LONG sel = g_force_sel;
    if (sel == 1) {
        *(LONG *)(p + 32) = 0;                 // DLSSGMode::eOff
    } else if (sel == 5) {
        // eDynamic, with our own frame-rate target.
        // Both checks: the plugin knows the mode, and this particular options
        // struct is new enough to carry it.
        if (g_dynamic_known && g_opts_version >= 5) {
            if (g_dyn_said != 1) {
                g_dyn_said = 1;
                log_num("dynamic: eDynamic written, target fps ", (unsigned)g_dyn_target);
            }
            *(LONG *)(p + 32) = 3;
            // Only from version 5: on an older struct the allocation does not
            // reach this field and writing it would land on whatever follows.
            g_saved_target = *(float *)(p + 116);
            *(float *)(p + 116) = (float)g_dyn_target;
            g_target_written = true;
        } else if (!g_dynamic_known && g_dyn_said != 2) {
            g_dyn_said = 2;
            log_line("dynamic: NOT applied -- this plugin does not know eDynamic");
        }
        // An older struct is not a dead end any more: the call is remade from
        // a version 5 copy in options_for_call.
    } else if (sel >= 2) {
        *(LONG *)(p + 32) = 1;                 // DLSSGMode::eOn
        *(LONG *)(p + 36) = sel - 1;           // 2x -> 1 generated frame
    }
}

// What the last capture was taken from. A game may call slDLSSGSetOptions
// once per frame, and re-capturing on every call meant two VirtualQuery
// syscalls per frame on the game's own render thread -- to copy bytes that had
// not changed since the frame before. The capture itself is still page-bounded
// every time it runs; what is skipped is running it when there is nothing new.
static LONG g_cap_mode = -1, g_cap_cnt = -1;
static const void *g_cap_vp = nullptr;

// A version 5 DLSSGOptions built from an older one, in memory of ours.
//
// Cyberpunk fills in version 3. That struct stops at offset 112: it has no
// dynamicTargetFrameRate at all, so there was nothing to write and DYNAMIC
// silently did nothing -- while the log still said "override applied now,
// selection 5", because that line reports the selection and not what reached
// the struct.
//
// Writing past the end of the game's 112 bytes is not an option. Making the
// call ourselves is: everything the game filled in is copied verbatim, the
// two fields it never had are given their defined values, and the version is
// declared to match what the buffer now actually contains. The plugin reads
// our 256 bytes; the game never sees them, exactly as with the multiplier
// override that has been running for days.
//
// Only ever used when the plugin itself knows eDynamic, which means 2.11.1 or
// newer -- announcing version 5 to a plugin that predates it would be a lie
// in the other direction.
static unsigned char g_v5_copy[256];

static const void *dynamic_upgrade(const void *options) {
    const unsigned n = copy_bounded(g_v5_copy, options, 120);
    if (n < 112) return nullptr;         // not even a complete version 3
    *(unsigned long long *)(g_v5_copy + 24) = 5;      // structVersion
    *(LONG *)(g_v5_copy + 32) = 3;                    // DLSSGMode::eDynamic
    // kStructVersion4. The game predates the field, so it never asked for it;
    // Boolean is a one-byte enum and eFalse is 0.
    g_v5_copy[112] = 0;
    g_v5_copy[113] = 0;
    g_v5_copy[114] = 0;
    g_v5_copy[115] = 0;
    // kStructVersion5.
    *(float *)(g_v5_copy + 116) = (float)g_dyn_target;
    return g_v5_copy;
}

// Which struct to actually hand to Streamline for this call.
static const void *options_for_call(const void *options) {
    if (g_force_sel != 5 || !g_dynamic_known || g_opts_version >= 5)
        return options;
    const void *up = dynamic_upgrade(options);
    if (up == nullptr) {
        if (g_dyn_said != 3) {
            g_dyn_said = 3;
            log_line("dynamic: could not read a whole options struct to rebuild");
        }
        return options;
    }
    if (g_dyn_said != 4) {
        g_dyn_said = 4;
        log_num("dynamic: game struct is version ", (unsigned)g_opts_version);
        log_num("  rebuilt as version 5, target fps ", (unsigned)g_dyn_target);
    }
    return up;
}

static unsigned hk_slDLSSGSetOptions(const void *viewport, const void *options) {
    unsigned char *p = (unsigned char *)options;
    long saved = -1;
    LONG raw_mode = -1, raw_cnt = -1;
    if (p != nullptr && memcmp(p + 8, kDlssgOptionsGuid, 16) == 0) {
        LONG *n = (LONG *)(p + 36);
        g_last_seen_generated = *n;
        // Read before force_into rewrites them: what the *game* asked for is
        // what decides whether this call is worth capturing again.
        raw_mode = *(LONG *)(p + 32);
        raw_cnt = *n;
        g_opts_version = (LONG)*(unsigned long long *)(p + 24);
        static bool said_ver = false;
        if (!said_ver) {
            said_ver = true;
            log_num("DLSSGOptions version the game fills in: ", (unsigned)g_opts_version);
        }
        const LONG want = g_force_generated;
        if (g_force_sel > 0) {
            // Written in place and put back straight after the call. The
            // struct belongs to the game and Streamline only reads it for the
            // duration of the call, so it never observes our value later and
            // the game never observes it at all.
            LONG sm, sc;
            force_into(p, &sm, &sc);
            saved = sc;
            g_saved_mode = sm;
            if (!g_override_said) {
                g_override_said = true;
                log_line("multiplier override active");
            }
        }
    }
    // Remember this call so the next key press can repeat it instead of
    // waiting for the game to change a setting on its own.
    if (p != nullptr && memcmp(p + 8, kDlssgOptionsGuid, 16) == 0 &&
        (g_opt_have == 0 || raw_mode != g_cap_mode || raw_cnt != g_cap_cnt ||
         viewport != g_cap_vp)) {
        if (copy_bounded(g_opt_copy, options, sizeof(g_opt_copy)) >= 40 &&
            copy_bounded(g_vp_copy, viewport, sizeof(g_vp_copy)) >= 8) {
            if (g_opt_have == 0) log_line("override: captured a call to repeat");
            g_opt_thread = (LONG)GetCurrentThreadId();
            g_opt_have = 1;
            g_cap_mode = raw_mode;
            g_cap_cnt = raw_cnt;
            g_cap_vp = viewport;
        }
    }
    const unsigned r = g_orig_setoptions(viewport, options_for_call(options));
    if (saved >= 0) {
        *(LONG *)(p + 36) = saved;
        *(LONG *)(p + 32) = g_saved_mode;
        if (g_target_written) *(float *)(p + 116) = g_saved_target;
    }
    return r;
}

// Replays the last options with the forced count. Called from the present
// hook and only on the thread the game itself used.
static void apply_override_now(void) {
    if (g_opt_pending == 0) return;
    // Says which precondition is missing instead of returning quietly. Three
    // can fail and they need different answers: no captured call to replay,
    // no wrapper installed, or the wrong thread.
    static int said = 0;
    if (g_orig_setoptions == nullptr) {
        if (said != 1) { said = 1; log_line("override: nothing wrapped yet"); }
        return;
    }
    if (g_opt_have == 0) {
        if (said != 2) {
            said = 2;
            log_line("override: the game has not called slDLSSGSetOptions yet,");
            log_line("  so there is no call to repeat -- change a frame");
            log_line("  generation setting once to seed it");
        }
        return;
    }
    if ((LONG)GetCurrentThreadId() != g_opt_thread) {
        if (said != 3) {
            said = 3;
            log_num("override: present runs on another thread, game used ",
                    (unsigned)g_opt_thread);
            log_num("  present thread is ", (unsigned)GetCurrentThreadId());
        }
        return;
    }
    said = 0;
    g_opt_pending = 0;
    LONG sm, sc;
    if (g_force_sel == 0) {                    // AUTO: put the game's own back
        *(LONG *)(g_opt_copy + 32) = 1;
        *(LONG *)(g_opt_copy + 36) = g_last_seen_generated;
    } else {
        force_into(g_opt_copy, &sm, &sc);
    }
    g_orig_setoptions(g_vp_copy, options_for_call(g_opt_copy));
    log_num("override applied now, selection ", (unsigned)g_force_sel);
}

static unsigned hk_slGetFeatureFunction(unsigned feature, const char *name, void *&fn) {
    const unsigned r = g_orig_getfeaturefn(feature, name, fn);
    if (r == 0 && name != nullptr && fn != nullptr &&
        strcmp(name, "slDLSSGSetOptions") == 0 &&
        fn != (void *)&hk_slDLSSGSetOptions) {
        g_orig_setoptions = (PFN_slDLSSGSetOptions)fn;
        // Settled here, against the module this pointer came out of, and it
        // overrides whatever the load-time scan concluded -- in both
        // directions. A game may map several copies of the plugin and run one
        // of them; only this one answers the calls we are about to make.
        HMODULE m = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)g_orig_setoptions, &m) && m != nullptr) {
            const bool had = g_dynamic_known;
            g_dynamic_known = image_has((unsigned char *)m, "sl::DLSSGMode::eDynamic");
            if (g_dynamic_known != had)
                log_line(g_dynamic_known
                    ? "  the plugin actually running does know eDynamic"
                    : "  the plugin actually running does NOT know eDynamic"
                      " -- DYNAMIC stays unavailable");
            // A selection made before this was known does not survive it.
            if (!g_dynamic_known && g_force_sel == 5) {
                g_force_sel = 0;
                g_force_generated = 0;
            }
        }
        fn = (void *)&hk_slDLSSGSetOptions;
        log_line("multiplier override armed (slDLSSGSetOptions wrapped)");
    }
    return r;
}

// The game asks for a frame token once per frame, from its own render thread
// -- which is the thread that called slDLSSGSetOptions and the only one it is
// safe to call it from again. With DLSS-G on, Present is Streamline's thread,
// so the replay cannot happen there; the log said so outright.
typedef unsigned (*PFN_slGetNewFrameToken)(void *&, const unsigned *);
static PFN_slGetNewFrameToken g_orig_frametoken = nullptr;
static void *g_frametoken_addr = nullptr;      // found at startup, hooked later
static void apply_override_now(void);

static unsigned hk_slGetNewFrameToken(void *&tok, const unsigned *idx) {
    const unsigned r = g_orig_frametoken(tok, idx);
    apply_override_now();
    return r;
}

// Put in place the first time the player picks something, and never before.
// A game nobody opens the panel in runs with nothing of ours in its per-frame
// path, which is how it was before the override existed.
static void arm_frametoken_hook(void) {
    if (g_orig_frametoken != nullptr || g_frametoken_addr == nullptr) return;
    if (MH_CreateHook(g_frametoken_addr, (void *)&hk_slGetNewFrameToken,
                      (void **)&g_orig_frametoken) != MH_OK ||
        MH_EnableHook(g_frametoken_addr) != MH_OK) {
        g_orig_frametoken = nullptr;
        log_line("override: could not hook slGetNewFrameToken");
        return;
    }
    log_line("override: slGetNewFrameToken hooked now (per-frame, on request)");
}

// ---- who is on the stack when the game dies --------------------------
//
// Windows already records the faulting module for every crash, and for
// Avatar it is always afop.exe itself, always at the same offset -- never
// this dll, Streamline or the driver. That says the fault happens in the
// game's code; it does not say whether we are in the call chain that got it
// there. This walks the return addresses and names the module each belongs
// to, which answers that directly.
static LONG CALLBACK crash_report(EXCEPTION_POINTERS *ep) {
    if (ep == nullptr || ep->ExceptionRecord == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Every exception is named, not only access violations. sl.log reports one
    // five seconds in, and there was no way to tell whether that is the crash
    // or the ordinary C++ exception traffic Streamline catches on every run --
    // 0xE06D7363 is a thrown C++ object and means nothing on its own. Capped,
    // because log_line opens the file per line.
    static LONG seen = 0;
    if (InterlockedIncrement(&seen) <= 6) {
        log_num("exception seen, code ", (unsigned)code);
        log_num("  at address (low32) ",
                (unsigned)((ULONG_PTR)ep->ExceptionRecord->ExceptionAddress & 0xFFFFFFFFu));
    }
    // A stack for anything that is not ordinary traffic. Restricting this to
    // access violations assumed the fault would be one, and heap corruption
    // (0xC0000374) or a failed fast-fail arrive under different codes and
    // would have been logged as a bare number with nothing to chase.
    if (code == 0x40010006u ||        // DBG_PRINTEXCEPTION_C, OutputDebugString
        code == 0x4001000Au ||        // the wide version of the same
        code == 0x406D1388u ||        // a thread announcing its name
        code == 0xE06D7363u)          // a thrown C++ object, caught somewhere
        return EXCEPTION_CONTINUE_SEARCH;
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;

    log_line("");
    log_line("=== access violation, stack below ===");
    log_num("  at address (low32): ",
            (unsigned)((ULONG_PTR)ep->ExceptionRecord->ExceptionAddress & 0xFFFFFFFFu));
    void *frames[24];
    const USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        HMODULE m = nullptr;
        wchar_t name[MAX_PATH];
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)frames[i], &m) && m != nullptr &&
            GetModuleFileNameW(m, name, MAX_PATH) != 0) {
            int j = 0; while (name[j] != 0) ++j;
            while (j > 0 && name[j - 1] != 0x5C) --j;
            char buf[96]; int k = 0;
            buf[k++] = ' '; buf[k++] = ' ';
            for (; name[j] != 0 && k < 80; ++j, ++k) buf[k] = (char)name[j];
            buf[k] = 0;
            log_line(buf);
        } else {
            log_line("  (unknown module)");
        }
    }
    log_line("=== end of stack ===");
    return EXCEPTION_CONTINUE_SEARCH;
}

static void arm_multiplier_override(unsigned char *base) {
    if (base == nullptr || g_orig_getfeaturefn != nullptr) return;
    void *f = (void *)GetProcAddress((HMODULE)base, "slGetFeatureFunction");
    if (f == nullptr) { log_line("  ! slGetFeatureFunction not exported here"); return; }
    // This runs during startup, before whatever other path happens to
    // initialise MinHook first. Without this the hook failed and said
    // nothing -- the export was found, the call returned an error code, and
    // the log looked exactly like a clean run.
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        log_num("  ! MinHook init failed, code ", (unsigned)init);
        return;
    }
    const MH_STATUS c = MH_CreateHook(f, (void *)&hk_slGetFeatureFunction,
                                      (void **)&g_orig_getfeaturefn);
    if (c != MH_OK) {
        log_num("  ! could not hook slGetFeatureFunction, code ", (unsigned)c);
        g_orig_getfeaturefn = nullptr;
        return;
    }
    const MH_STATUS e = MH_EnableHook(f);
    if (e != MH_OK) {
        log_num("  ! could not enable the slGetFeatureFunction hook, code ", (unsigned)e);
        g_orig_getfeaturefn = nullptr;
        return;
    }
    log_line("  slGetFeatureFunction hooked");

    // After MH_Initialize, not before it. Putting this above the init made
    // MH_CreateHook fail with "not initialised" and the else branch swallowed
    // it -- the same silent-failure shape fixed for slGetFeatureFunction
    // earlier today, reintroduced in the same function hours later. Every
    // branch says something now.
    g_frametoken_addr = (void *)GetProcAddress((HMODULE)base, "slGetNewFrameToken");
    if (g_frametoken_addr == nullptr) log_line("  ! slGetNewFrameToken not exported");
    else log_line("  slGetNewFrameToken found (hooked only if an override is picked)");
}

#include "overlay.h"

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
    g_presents_seen = 1;
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
    g_presents_seen = 1;
    if (g_recording != 0) note_display(self);
    ov_draw(self);
    return g_orig_dxgi_present(self, interval, flags);
}

// Where the detoured Present lives, so an unload of that module can be
// recognised for what it is.
static unsigned char *g_present_mod = nullptr;
static size_t g_present_modsize = 0;

static void hook_swapchain_present(void *sc) {
    if (sc == nullptr || g_orig_dxgi_present != nullptr) return;
    void **vt = *reinterpret_cast<void ***>(sc);
    {
        HMODULE m = nullptr;
        wchar_t nm[MAX_PATH];
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)vt[8], &m) && m != nullptr &&
            GetModuleFileNameW(m, nm, MAX_PATH) != 0) {
            int j = 0;
            while (nm[j] != 0) ++j;
            while (j > 0 && nm[j - 1] != 0x5C) --j;
            char buf[96];
            int k = 0;
            buf[k++] = ' '; buf[k++] = ' ';
            const char *lead = "present lives in ";
            for (int i = 0; lead[i] != 0; ++i) buf[k++] = lead[i];
            for (; nm[j] != 0 && k < 90; ++j, ++k) buf[k] = (char)nm[j];
            buf[k] = 0;
            log_line(buf);
            g_present_mod = (unsigned char *)m;
            auto *dos = (IMAGE_DOS_HEADER *)m;
            if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                auto *nt = (IMAGE_NT_HEADERS *)(g_present_mod + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE)
                    g_present_modsize = nt->OptionalHeader.SizeOfImage;
            }
        }
    }
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
    // The queue arrives here too. Capturing it only in CreateSwapChainForHwnd
    // meant the overlay never initialised in a game that uses this older
    // entry point -- Assassin's Creed Shadows does, and every panel open
    // logged "init failed" with nothing to submit on.
    if (SUCCEEDED(hr)) {
        if (desc != nullptr) ov_attach_window(desc->OutputWindow);
        if (out != nullptr) ov_pair_queue(*out, dev);
    }
    if (SUCCEEDED(hr) && out != nullptr) hook_swapchain_present(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_cscfh(void *self, IUnknown *dev, HWND hwnd,
        const void *d1, const void *fs, void *restrict_to, IDXGISwapChain **out) {
    HRESULT hr = g_orig_cscfh(self, dev, hwnd, d1, fs, restrict_to, out);
    if (SUCCEEDED(hr)) {
        ov_attach_window(hwnd);
        if (out != nullptr) ov_pair_queue(*out, dev);
    }
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
        // Faster while the panel is up: WM_INPUT is delivered to the raw
        // input window on this thread, so the pointer only moves as often as
        // this pumps.
        ov_pump_cursor();
        Sleep(g_ov_visible ? 4 : 50);
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

        if (g_presents_seen) {
            static bool tilde_was = false;
            const bool t = (GetAsyncKeyState(VK_OEM_3) & 0x8000) != 0;   // `
            const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
            if (esc && g_ov_visible) {
                // Escape backs out one step at a time: an edit in progress
                // first, the panel only once there is nothing else to leave.
                // The panel swallows the key either way, so the game does not
                // open its own menu behind us.
                if (g_ov_editing) {
                    ov_edit_cancel();
                } else {
                    g_ov_visible = false;
                    ov_grab_cursor(false);
                    log_line("panel: hidden");
                }
            }
            if (t && !tilde_was) {
                // The same key opens and closes. It used to step through the
                // rows while open, which meant the only way to leave the panel
                // was Escape and the only way to reach a row with the keyboard
                // was to walk past the others, applying each one on the way.
                // The mouse selects; the key is a door.
                g_ov_visible = !g_ov_visible;
                // Re-armed on every open, not once per process. It switched
                // itself off after the first good draw, so the crash on a
                // *second* opening left nothing but "panel: shown" behind.
                if (g_ov_visible && g_debug) g_ov_trace = 5;
                if (!g_ov_visible) ov_edit_cancel();
                ov_grab_cursor(g_ov_visible);
                log_line(g_ov_visible ? "panel: shown" : "panel: hidden");
            }
            tilde_was = t;
        }
                
        // The click is polled too. The window procedure's job is only to stop
        // the game seeing it; deciding what it means happens here, next to
        // every other key, so there is no state shared with the message thread.
        if (g_ov_visible) {
            static bool lmb_was = false;
            static bool dragging = false;
            const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            const bool onslider = g_ov_hot == kHotSlider;
            const bool onvalue  = g_ov_hot == kHotValue;
            const bool row_ok = g_ov_hot != 5 || g_dynamic_known;

            if (lmb && !lmb_was) {
                if (onvalue) {
                    ov_edit_begin();          // click the box, type a number
                } else {
                    // Any other click ends an edit rather than abandoning it
                    // half-finished: what was typed is what was meant.
                    const int typed = ov_edit_commit();
                    if (typed >= 0 && typed != g_dyn_target) {
                        g_dyn_target = typed;
                        g_dyn_said = 0;
                        arm_frametoken_hook();
                        g_opt_pending = 1;
                        log_num("panel: dynamic target typed ", (unsigned)typed);
                    }
                    if (onslider) {
                        dragging = true;
                    } else if (g_ov_hot >= 0 && g_ov_hot < kPanRows && row_ok) {
                        g_force_sel = g_ov_hot;
                        g_force_generated = g_ov_hot >= 2 ? g_ov_hot - 1 : 0;
                        arm_frametoken_hook();
                        g_opt_pending = 1;
                        g_override_said = false;
                        log_num("panel: frames to generate now ", (unsigned)g_ov_hot);
                    }
                }
            }
            if (!lmb) dragging = false;
            // Held: the value follows the pointer, but only lands on a stop,
            // so a drag produces frame rates anyone would actually pick rather
            // than whatever pixel the hand stopped on.
            if (dragging) {
                const LONG v = (LONG)kStops[ov_stop_at(g_ov_mx)];
                if (v != g_dyn_target) {
                    g_dyn_target = v;
                    g_dyn_said = 0;
                    arm_frametoken_hook();
                    g_opt_pending = 1;
                    log_num("panel: dynamic target now ", (unsigned)v);
                }
            }
            lmb_was = lmb;

            // Typing, polled like every other key here. A game that never
            // delivers WM_CHAR -- and one using raw input may not -- cannot
            // stop this, which is the same reason the rest of the panel polls.
            if (g_ov_editing) {
                static bool dwas[13] = { false };
                static const int vks[13] = {
                    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
                    VK_BACK, VK_RETURN, VK_DECIMAL };
                for (int k = 0; k < 12; ++k) {
                    const bool dn = (GetAsyncKeyState(vks[k]) & 0x8000) != 0 ||
                                    (k < 10 && (GetAsyncKeyState(VK_NUMPAD0 + k) & 0x8000) != 0);
                    if (dn && !dwas[k]) {
                        if (k < 10)               ov_edit_digit((char)('0' + k));
                        else if (vks[k] == VK_BACK) ov_edit_back();
                        else {
                            const int typed = ov_edit_commit();
                            if (typed >= 0 && typed != g_dyn_target) {
                                g_dyn_target = typed;
                                g_dyn_said = 0;
                                arm_frametoken_hook();
                                g_opt_pending = 1;
                                log_num("panel: dynamic target typed ", (unsigned)typed);
                            }
                        }
                    }
                    dwas[k] = dn;
                }
            }
        }

            // No function-key shortcuts: they collide with what games bind
            // themselves. The panel is driven by the mouse instead.

        const bool down = g_presents_seen &&
                          (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
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
        // The raw input window lives on this thread, so WM_INPUT arrives
        // only while this pumps. Stop pumping and the pointer freezes.
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
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
    if (d == nullptr) return;
    if (reason == 2) {                                       // UNLOADED
        // Only the ones that can carry something of ours. A detour outlives
        // the module it points into, and calling it afterwards is a jump into
        // freed memory -- which is what an exception seconds after
        // `Feature 'kFeatureDLSS_G' unloaded` would look like.
        if (path_has(d->FullDllName, L"sl.") ||
            path_has(d->FullDllName, L"sl_") ||
            path_has(d->FullDllName, L"nvngx")) {
            log_wide("module unloaded: ", d->FullDllName);
            if (g_present_mod != nullptr &&
                (unsigned char *)d->DllBase == g_present_mod) {
                log_line("  !! this is the module our Present detour lives in");
                log_line("  !! the next Present would jump into freed memory");
            }
        }
        return;
    }
    if (reason != 1) return;                                 // 1 = LOADED

    // Load order matters for reading the Streamline log afterwards: the plugin
    // caches the maximum at its startup, so it has to come after the snippet.
    if (name_is(d->BaseDllName, L"sl.interposer.dll")) {
        log_line("sl.interposer.dll mapped");
        arm_multiplier_override((unsigned char *)d->DllBase);
        return;
    }
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
        if (!g_dynamic_known &&
            image_has((unsigned char *)d->DllBase, "sl::DLSSGMode::eDynamic")) {
            g_dynamic_known = true;
            log_line("  this copy knows DLSSGMode::eDynamic (confirmed later"
                     " against the copy that runs)");
        }
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
    if (g_ov_self == nullptr) g_ov_self = self;
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
            g_preset_b = flag_file(L"mfg-presetb.txt");
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
            g_debug = flag_file(L"mfg-debug.txt");
            // On by default, off with mfg-nopanel.txt -- the same reasoning
            // as the cubins. The person installing this has the dll and
            // nothing else, so anything behind a file they have to create
            // reaches nobody.
            g_ov_enabled = !flag_file(L"mfg-nopanel.txt");
            log_line(g_ov_enabled ? "panel on (` opens it)" : "panel off");
            g_cubins = !flag_file(L"mfg-nocubins.txt");
            // g_meter_off was declared and read but never assigned, so
            // mfg-nometer.txt did nothing at all.
            g_meter_off = flag_file(L"mfg-nometer.txt");
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
    if (flag_file(L"mfg-indicator.txt")) {
        wchar_t p[MAX_PATH];
        int j = 0;
        while (g_log[j] != 0 && j < MAX_PATH - 1) { p[j] = g_log[j]; ++j; }
        while (j > 0 && p[j - 1] != 0x5C) --j;
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

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto reg = reinterpret_cast<PFN_LdrRegister>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    if (reg == nullptr) {
        log_line("LdrRegisterDllNotification is unavailable; nothing will be patched");
        return TRUE;
    }
    PVOID cookie = nullptr;
    if (reg(0, &on_dll_load, nullptr, &cookie) == 0) {
        if (g_debug) {
            AddVectoredExceptionHandler(0, &crash_report);
            log_line("debug: access violations will be logged with a stack");
        }
        log_line("armed, waiting for nvngx_dlssg.dll");

        // The notification only fires for modules mapped *after* this point.
        // A game that imports sl.interposer.dll statically -- Avatar does --
        // already has it loaded by the time a proxy runs, so waiting for the
        // notification means never arming at all. Halo loads it dynamically
        // and did fire, which is exactly why this was easy to miss.
        {
            HMODULE si = GetModuleHandleW(L"sl.interposer.dll");
            if (si != nullptr) {
                log_line("sl.interposer.dll already loaded");
                arm_multiplier_override((unsigned char *)si);
            }
        }
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
