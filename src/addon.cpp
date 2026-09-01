// Multi Frame Generation multiplier override.
//
// The patched NGX snippet advertises DLSSG.MultiFrameCountMax = 5 on any
// architecture, and sl.dlss_g is happy to generate that many frames.  What is
// missing is a game that asks for more than one: anything that shipped before
// DLSS 4 calls slDLSSGSetOptions with numFramesToGenerate = 1 and gets exactly
// what it asked for.
//
// This add-on rewrites that one field on its way into the Streamline plugin.

#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>

#include <reshade.hpp>
#include <MinHook.h>

extern "C" __declspec(dllexport) const char *NAME        = "MFG Multiplier";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Raises DLSS Frame Generation past 2x on GPUs NVIDIA limits to a single generated frame.";

static void logf(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    reshade::log::message(reshade::log::level::info, buf);
}

// ---------------------------------------------------------------- layout ---
//
// Streamline chains its option structs through a common header.  Read out of
// sl.dlss_g 2.13 rather than a header, because the header is not shipped with
// the games:
//
//   BaseStructure  +0x00 BaseStructure *next
//                  +0x08 StructType     structType[16]
//                  +0x18 uint64_t       structVersion
//   DLSSGOptions   +0x20 uint32_t       mode                 0 = off
//                  +0x24 uint32_t       numFramesToGenerate
//
// slSetData walks the chain comparing the two qwords at +0x08 and +0x10, which
// is where this identifier comes from.

static const unsigned char kDLSSGOptionsType[16] = {
    0xcb, 0xf1, 0xc5, 0xfa, 0xfd, 0x2d, 0x36, 0x4f,
    0xa1, 0xe6, 0x3a, 0x9e, 0x86, 0x52, 0x56, 0xc5,
};
static const unsigned kOffNext  = 0x00;
static const unsigned kOffType  = 0x08;
static const unsigned kOffMode  = 0x20;
static const unsigned kOffCount = 0x24;

// The Streamline plugin caps itself at 5 generated frames regardless of what
// the snippet advertises, so there is nothing above 6x to ask for.
static const int kMaxGenerated = 5;

static bool readable(const void *p, size_t n) {
    if (p == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof mbi) == 0 || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    return reinterpret_cast<uintptr_t>(p) + n <= base + mbi.RegionSize;
}

static bool writable(const void *p) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof mbi) == 0 || mbi.State != MEM_COMMIT) return false;
    const DWORD w = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & w) != 0 && (mbi.Protect & PAGE_GUARD) == 0;
}

// Follow the next chain looking for the DLSSGOptions node.  Bounded, because a
// malformed chain must not become an infinite loop inside the game's present.
static void *find_options(void *head) {
    void *p = head;
    for (int hop = 0; hop < 16 && p != nullptr; ++hop) {
        if (!readable(p, kOffCount + 4)) return nullptr;
        if (std::memcmp(static_cast<char *>(p) + kOffType, kDLSSGOptionsType, 16) == 0)
            return p;
        p = *reinterpret_cast<void **>(static_cast<char *>(p) + kOffNext);
    }
    return nullptr;
}

// ------------------------------------------------------------------ state ---

static int  g_target        = 1;      // 1 = leave the game alone
static bool g_hooked        = false;
static bool g_plugin_seen   = false;
static int  g_game_asked    = 0;      // what the game last requested
static int  g_applied       = 0;      // what we last handed to the plugin
static int  g_mode          = 0;      // the game's DLSSGMode
static unsigned g_overrides = 0;
static unsigned g_rejects   = 0;
static int  g_last_result   = 0;
static bool g_readonly_opts = false;  // caller's struct was not writable
static char g_note[192]     = "waiting for sl.dlss_g.dll";

// ------------------------------------------------------------------ hooks ---
//
// Both entry points are reached through slGetPluginFunction and both put the
// chain pointer in one of the first two argument slots.  Read off the plugin
// rather than the public header, which disagrees:
//
//   slDLSSGSetOptions(rcx = options, rdx = viewport)
//       copies the first 0x28 bytes of the options onto its stack, replaces the
//       copy's `next` with the viewport, and calls slSetData on the copy.
//   slSetData(rcx = chain head)
//
// So hooking both is not redundant.  slDLSSGSetOptions sees the game's own
// struct, which may be const; slSetData then sees the stack copy, which never
// is.  Whichever one can be written wins, and the other passes through because
// the count already matches.
//
// Forwarding four register arguments covers either signature; the callee reads
// only the ones it declared.

typedef uint32_t (*PFN_Sl4)(void *, void *, void *, void *);
static PFN_Sl4 g_orig_setopts = nullptr;
static PFN_Sl4 g_orig_setdata = nullptr;

static uint32_t apply_override(PFN_Sl4 orig, void *a, void *b, void *c, void *d) {
    if (orig == nullptr) return 0xFFFFFFFF;

    void *opts = find_options(a);
    if (opts == nullptr) opts = find_options(b);
    if (opts == nullptr) return orig(a, b, c, d);

    auto *pmode  = reinterpret_cast<uint32_t *>(static_cast<char *>(opts) + kOffMode);
    auto *pcount = reinterpret_cast<uint32_t *>(static_cast<char *>(opts) + kOffCount);
    g_mode       = static_cast<int>(*pmode);
    g_game_asked = static_cast<int>(*pcount);

    const int want = g_target;
    // Frame generation off, or nothing to change: stay out of the way entirely.
    if (want <= 1 || g_mode == 0 || static_cast<int>(*pcount) == want)
        return orig(a, b, c, d);

    if (!writable(pcount)) {
        if (!g_readonly_opts) {
            g_readonly_opts = true;
            logf("[MFG] the game's DLSSGOptions live in read-only memory; cannot override");
            std::snprintf(g_note, sizeof g_note, "options are read-only, override not possible");
        }
        return orig(a, b, c, d);
    }

    // Write, forward, restore.  Patching in place rather than passing a copy is
    // deliberate: the struct is versioned and its real size is whatever the game
    // was built against, so copying it means guessing how many bytes to read.
    const uint32_t saved = *pcount;
    *pcount = static_cast<uint32_t>(want);
    uint32_t r = orig(a, b, c, d);
    *pcount = saved;

    if (r != 0) {
        // The plugin refused.  Hand the game's own request through so frame
        // generation keeps working instead of switching off underneath it.
        ++g_rejects;
        g_last_result = static_cast<int>(r);
        r = orig(a, b, c, d);
        std::snprintf(g_note, sizeof g_note,
                      "plugin rejected %dx (0x%X) -- is nvngx_dlssg.dll patched?",
                      want + 1, g_last_result);
    } else {
        ++g_overrides;
        g_applied     = want;
        g_last_result = 0;
        std::snprintf(g_note, sizeof g_note, "generating %d frames per rendered frame", want);
    }
    return r;
}

static uint32_t hk_setopts(void *a, void *b, void *c, void *d) {
    return apply_override(g_orig_setopts, a, b, c, d);
}
static uint32_t hk_setdata(void *a, void *b, void *c, void *d) {
    return apply_override(g_orig_setdata, a, b, c, d);
}

// ---------------------------------------------------------------- install ---

typedef void *(*PFN_GetPluginFunction)(const char *);

static void install_hook() {
    if (g_hooked) return;
    HMODULE h = GetModuleHandleW(L"sl.dlss_g.dll");
    if (h == nullptr) return;
    if (!g_plugin_seen) {
        g_plugin_seen = true;
        logf("[MFG] sl.dlss_g.dll present");
    }
    auto gpf = reinterpret_cast<PFN_GetPluginFunction>(GetProcAddress(h, "slGetPluginFunction"));
    if (gpf == nullptr) {
        std::snprintf(g_note, sizeof g_note, "sl.dlss_g.dll has no slGetPluginFunction");
        g_hooked = true;   // nothing more to try
        return;
    }
    // Resolve the plugin entry points the same way Streamline does, so the real
    // function gets detoured no matter when the game cached its pointer.
    void *setopts = gpf("slDLSSGSetOptions");
    void *setdata = gpf("slSetData");
    if (setopts == nullptr && setdata == nullptr) return;   // plugin not up yet

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        logf("[MFG] MH_Initialize failed");
        g_hooked = true;
        return;
    }
    int n = 0;
    if (setopts != nullptr &&
        MH_CreateHook(setopts, reinterpret_cast<void *>(&hk_setopts),
                      reinterpret_cast<void **>(&g_orig_setopts)) == MH_OK &&
        MH_EnableHook(setopts) == MH_OK) {
        logf("[MFG] hooked slDLSSGSetOptions at %p", setopts);
        ++n;
    }
    if (setdata != nullptr &&
        MH_CreateHook(setdata, reinterpret_cast<void *>(&hk_setdata),
                      reinterpret_cast<void **>(&g_orig_setdata)) == MH_OK &&
        MH_EnableHook(setdata) == MH_OK) {
        logf("[MFG] hooked slSetData at %p", setdata);
        ++n;
    }
    if (n == 0) {
        std::snprintf(g_note, sizeof g_note, "found the plugin but could not hook it");
    } else {
        std::snprintf(g_note, sizeof g_note, "hooked, waiting for the game to enable frame generation");
    }
    g_hooked = true;
}

// --------------------------------------------------------------- settings ---

static void load_settings(reshade::api::effect_runtime *rt) {
    int v = 0;
    if (reshade::get_config_value(rt, "MFG", "Target", v))
        g_target = (v < 1 ? 1 : (v > kMaxGenerated ? kMaxGenerated : v));
}
static void save_settings() {
    reshade::set_config_value(nullptr, "MFG", "Target", g_target);
}

static void on_present(reshade::api::command_queue *, reshade::api::swapchain *,
                       const reshade::api::rect *, const reshade::api::rect *,
                       uint32_t, const reshade::api::rect *) {
    install_hook();
}

// --------------------------------------------------------------------- ui ---

static void draw_overlay(reshade::api::effect_runtime *) {
    static const char *kLabels[] = {
        "Off (leave the game's setting)", "2x", "3x", "4x", "5x", "6x",
    };
    int idx = g_target;                       // target N generated -> (N+1)x
    if (ImGui::Combo("Multiplier", &idx, kLabels, IM_ARRAYSIZE(kLabels))) {
        g_target  = idx;
        g_rejects = 0;
        save_settings();
    }
    ImGui::TextDisabled("%s", g_note);

    ImGui::Separator();
    if (!g_plugin_seen) {
        ImGui::TextWrapped("sl.dlss_g.dll has not loaded. This game either does not use "
                           "Streamline frame generation, or has not started it yet.");
        return;
    }
    ImGui::Text("Game requests   : %d generated (%dx)", g_game_asked, g_game_asked + 1);
    ImGui::Text("Frame generation: %s",
                g_mode == 0 ? "off" : (g_mode == 1 ? "on" : "auto"));
    if (g_overrides > 0)
        ImGui::Text("Applied         : %d generated (%dx), %u times",
                    g_applied, g_applied + 1, g_overrides);
    if (g_rejects > 0)
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                           "Rejected %u times, last result 0x%X", g_rejects, g_last_result);

    ImGui::Separator();
    ImGui::TextWrapped("Anything above 2x also needs the architecture gate removed from "
                       "nvngx_dlssg.dll (tools/mfg_unlock.py). Without it the plugin "
                       "reports a maximum of one generated frame and refuses the rest.");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule)) return FALSE;
        reshade::register_overlay("MFG Multiplier", draw_overlay);
        reshade::register_event<reshade::addon_event::init_effect_runtime>(load_settings);
        reshade::register_event<reshade::addon_event::present>(on_present);
        logf("[MFG] addon registered");
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(load_settings);
        reshade::unregister_event<reshade::addon_event::present>(on_present);
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
