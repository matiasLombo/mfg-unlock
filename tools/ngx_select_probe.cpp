// Which nvngx_dlssg.dll does NGX actually load, and can that be steered?
//
// The other probe loads a snippet file directly, so it says what a given file
// would report but nothing about which file the runtime picks.  That turned out
// to be the whole problem: a machine can hold three copies -- the driver store,
// the game folder, and NVIDIA's OTA cache under ProgramData -- and NGX chooses.
//
// This one goes through the NGX core instead (nvngx.dll ->_nvngx.dll), which is
// what a game does, and asks it for the DLSS-G capability parameters.  With
// __NGX_LOG_LEVEL=2 the core prints "Found <source> snippet at <path>", so one
// run answers the question.
//
// Put a snippet next to this executable to test whether the app directory wins.
//
// build: g++ -std=c++20 -O2 -o ngx_select_probe.exe ngx_select_probe.cpp -ld3d12 -ldxgi -lole32 -static
// usage: ngx_select_probe.exe [appId]

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <initializer_list>
#include <cstdlib>
#include <cstring>
#include <winternl.h>

// NVSDK_NGX_Parameter is MSVC-built with same-name overloads in reverse
// declaration order.  Get(const char*, unsigned*) is slot 12.
static unsigned get_uint(void *p, const char *name, unsigned *out) {
    using pfn = unsigned(__cdecl *)(void *, const char *, unsigned *);
    auto **vt = *reinterpret_cast<void ***>(p);
    return reinterpret_cast<pfn>(vt[12])(p, name, out);
}
static unsigned get_int(void *p, const char *name, int *out) {
    using pfn = unsigned(__cdecl *)(void *, const char *, int *);
    auto **vt = *reinterpret_cast<void ***>(p);
    return reinterpret_cast<pfn>(vt[11])(p, name, out);
}

// ---- patching the snippet as it is mapped -------------------------------
//
// NGX verifies the snippet's Authenticode signature while loading it, and
// refuses a modified file: it falls back to the driver's copy, which for
// DLSS-G is an older build with no multi-frame support at all. Nothing
// re-checks the image once it is mapped, so the same four bytes applied to the
// loaded code do what editing the file cannot.
//
// Timing is the whole trick. The snippet's PopulateParameters runs inside
// NVSDK_NGX_D3D12_Init_Ext, so patching after that call is too late -- the
// capability block has already been filled in. LdrRegisterDllNotification fires
// while the loader is still mapping the DLL, which is early enough.

static int g_patched = -1;

static int patch_gates(unsigned char *base) {
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(
        base + reinterpret_cast<IMAGE_DOS_HEADER *>(base)->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    auto *sec = IMAGE_FIRST_SECTION(nt);
    unsigned char *text = nullptr;
    size_t tlen = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            text = base + sec[i].VirtualAddress;
            tlen = sec[i].Misc.VirtualSize;
        }
    int hits = 0;
    // cmp r32, 0x1B0 in its two encodings: 3D imm32 for eax, 81 /7 imm32 otherwise.
    for (size_t i = 0; text != nullptr && i + 6 <= tlen; ++i) {
        const bool eax = text[i] == 0x3D && *(unsigned *)(text + i + 1) == 0x1B0;
        const bool reg = text[i] == 0x81 && (text[i + 1] & 0xF8) == 0xF8 &&
                         *(unsigned *)(text + i + 2) == 0x1B0;
        if (!eax && !reg) continue;
        unsigned char *imm = text + i + (eax ? 1 : 2);
        DWORD old = 0;
        if (VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old)) {
            *(unsigned *)imm = 0;
            VirtualProtect(imm, 4, old, &old);
            ++hits;
        }
    }
    return hits;
}

struct LDR_DLL_NOTIFICATION_DATA {
    ULONG Flags;
    const UNICODE_STRING *FullDllName;
    const UNICODE_STRING *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
typedef VOID(CALLBACK *PLDR_DLL_NOTIFICATION)(ULONG, const LDR_DLL_NOTIFICATION_DATA *, PVOID);
typedef NTSTATUS(NTAPI *PFN_LdrRegister)(ULONG, PLDR_DLL_NOTIFICATION, PVOID, PVOID *);

static VOID CALLBACK on_dll_load(ULONG reason, const LDR_DLL_NOTIFICATION_DATA *d, PVOID) {
    if (reason != 1 || d == nullptr || d->BaseDllName == nullptr) return;   // 1 = LOADED
    if (_wcsicmp(d->BaseDllName->Buffer, L"nvngx_dlssg.dll") != 0) return;
    const int n = patch_gates(reinterpret_cast<unsigned char *>(d->DllBase));
    // NGX maps the snippet more than once; the later passes find the immediates
    // already rewritten, so keep the first pass that actually did work.
    if (n > 0 && g_patched <= 0) g_patched = n;
}

static bool arm_loader_hook() {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto reg = reinterpret_cast<PFN_LdrRegister>(GetProcAddress(nt, "LdrRegisterDllNotification"));
    if (reg == nullptr) return false;
    PVOID cookie = nullptr;
    return reg(0, &on_dll_load, nullptr, &cookie) == 0;
}

typedef unsigned (__cdecl *PFN_Init)(unsigned long long, const wchar_t *,
                                     ID3D12Device *, const void *, unsigned);
typedef unsigned (__cdecl *PFN_GetCaps)(void **);
typedef unsigned (__cdecl *PFN_Shutdown)(void);

static const wchar_t *kCorePaths[] = {
    L"nvngx.dll",
    L"C:\\Windows\\System32\\nvngx.dll",
    L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_a3944b54ff18b284\\nvngx.dll",
};

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // Static reference to version.dll so a proxy in this folder is loaded at
    // process start, exactly as it would be in a game that imports it.
    { DWORD h = 0; GetFileVersionInfoSizeA("nul", &h); }
    const unsigned long long appId =
        (argc > 1) ? strtoull(argv[1], nullptr, 0) : 0xE658700ull;

    ID3D12Device *dev = nullptr;
    IDXGIFactory4 *fac = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&fac))) {
        std::printf("CreateDXGIFactory1 failed\n"); return 1;
    }
    IDXGIAdapter1 *ad = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        ad->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0,
                                        __uuidof(ID3D12Device), (void **)&dev))) {
            std::printf("adapter : %S\n", d.Description);
            break;
        }
        ad->Release(); ad = nullptr;
    }
    if (dev == nullptr) { std::printf("no D3D12 device\n"); return 1; }

    HMODULE core = nullptr;
    for (auto p : kCorePaths) {
        core = LoadLibraryW(p);
        if (core != nullptr) { std::printf("ngx core: %S\n", p); break; }
    }
    if (core == nullptr) { std::printf("could not load the NGX core\n"); return 1; }

    auto init = (PFN_Init)GetProcAddress(core, "NVSDK_NGX_D3D12_Init_Ext");
    auto caps = (PFN_GetCaps)GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    auto shut = (PFN_Shutdown)GetProcAddress(core, "NVSDK_NGX_D3D12_Shutdown");
    if (init == nullptr || caps == nullptr) { std::printf("missing core exports\n"); return 1; }

    wchar_t here[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, here);
    std::printf("app dir : %S\n", here);
    std::printf("appId   : 0x%llX\n", appId);
    const char *dis = getenv("__NGX_DISABLE_UPDATER");
    std::printf("updater : %s\n\n", (dis && dis[0] == '1') ? "disabled by env" : "enabled");

    // One variant per process. A wrong argument order faults inside the core,
    // and a crash must not take the answer with it, so the caller picks which
    // combination to try and the shell runs them one at a time.
    const int variant = (argc > 2) ? atoi(argv[2]) : 0;
    const unsigned vers[] = { 0x15u, 0x14u, 0x13u };
    const unsigned ver = vers[variant % 3];
    const bool version_last = (variant / 3) == 0;
    const bool want_patch = (argc > 3 && std::strcmp(argv[3], "patch") == 0);
    if (want_patch)
        std::printf("loader hook : %s\n", arm_loader_hook() ? "armed" : "FAILED to arm");
    std::printf("variant %d: %s, sdkVersion 0x%X\n", variant,
                version_last ? "(appId, path, device, featureInfo, version)"
                             : "(appId, path, device, version, featureInfo)", ver);
    unsigned r;
    if (version_last) {
        r = init(appId, here, dev, nullptr, ver);
    } else {
        auto alt = reinterpret_cast<unsigned(__cdecl *)(unsigned long long, const wchar_t *,
                                                        ID3D12Device *, unsigned, const void *)>(init);
        r = alt(appId, here, dev, ver, nullptr);
    }
    std::printf("Init_Ext : 0x%08X%s\n", r,
                (r & 0xFFF00000) == 0xBAD00000 ? "  failed" : "  ok");
    if ((r & 0xFFF00000) == 0xBAD00000) { std::printf("giving up on init\n"); return 1; }

    if (want_patch)
        std::printf("gates rewritten at map time: %d\n", g_patched);

    void *params = nullptr;
    unsigned rc = caps(&params);
    std::printf("GetCapabilityParameters : 0x%08X\n", rc);
    if (params != nullptr) {
        unsigned max = 0xFFFFFFFF, model = 0;
        int avail = -1;
        unsigned r1 = get_uint(params, "DLSSG.MultiFrameCountMax", &max);
        unsigned r2 = get_uint(params, "DLSSG.ModelVersion", &model);
        unsigned r3 = get_int(params, "DLSSG.ReflexWarp.Available", &avail);
        std::printf("\n  DLSSG.MultiFrameCountMax : ");
        if ((r1 & 0xFFF00000) == 0xBAD00000) std::printf("not reported (0x%08X)\n", r1);
        else std::printf("%u   -> maximum multiplier %ux\n", max, max + 1);
        std::printf("  DLSSG.ModelVersion       : ");
        if ((r2 & 0xFFF00000) == 0xBAD00000) std::printf("not reported\n");
        else std::printf("0x%X\n", model);
        (void)r3;
    }
    if (shut) shut();
    return 0;
}
