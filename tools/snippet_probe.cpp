// Ask an NGX snippet, on this machine, how many frames it will let us generate.
//
// nvngx_dlssg.dll decides that in DLSSGInstanceManager::PopulateParameters by
// querying the adapter's architecture through NVAPI and writing the answer into
// the NGX parameter block as DLSSG.MultiFrameCountMax.  That path is reachable
// through the exported NVSDK_NGX_D3D12_PopulateParameters_Impl, so the whole
// question can be settled with a D3D12 device and no game at all.
//
// The snippet is MSVC-built and calls NVSDK_NGX_Parameter through a vtable, so
// the parameter object here is a hand-built vtable rather than a C++ class:
// MSVC emits same-name virtual overloads in reverse declaration order, and this
// file has to match that exactly.
//
//   Set  slot 0 void*   1 ID3D12Resource*  2 ID3D11Resource*  3 int
//        slot 4 uint    5 double           6 float            7 uint64
//   Get  slot 8 void**  9 ID3D12Resource** 10 ID3D11Resource** 11 int*
//        slot12 uint*  13 double*         14 float*          15 uint64*
//   slot 16 Reset
//
// The snippet refuses to initialise unless the module that called it has
// "nvngx.dll" somewhere in its path (GetModuleHandleExW on the return address,
// then wcsstr), so the executable has to be named accordingly.
//
// build: g++ -std=c++20 -O2 -o nvngx.dll.probe.exe snippet_probe.cpp -ld3d12 -ldxgi -static
// usage: nvngx.dll.probe.exe <path to nvngx_dlssg.dll> [appId]

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>

// ------------------------------------------------------------ recording ---

struct Rec { std::string name; std::string type; unsigned long long u = 0; double f = 0; };
static std::vector<Rec> g_sets;
static std::map<std::string, unsigned long long> g_ints;

static void record_u(const char *n, const char *t, unsigned long long v) {
    g_sets.push_back({n ? n : "(null)", t, v, 0});
    if (n) g_ints[n] = v;
}
static void record_f(const char *n, const char *t, double v) {
    g_sets.push_back({n ? n : "(null)", t, 0, v});
}

// The snippet calls these as member functions: `this` arrives in rcx, so every
// thunk takes a leading self pointer it ignores.
static void  set_pv (void *, const char *n, void *v)              { record_u(n, "void*",  (unsigned long long)v); }
static void  set_d12(void *, const char *n, void *v)              { record_u(n, "d3d12",  (unsigned long long)v); }
static void  set_d11(void *, const char *n, void *v)              { record_u(n, "d3d11",  (unsigned long long)v); }
static void  set_i  (void *, const char *n, int v)                { record_u(n, "int",    (unsigned long long)(long long)v); }
static void  set_u  (void *, const char *n, unsigned v)           { record_u(n, "uint",   v); }
static void  set_db (void *, const char *n, double v)             { record_f(n, "double", v); }
static void  set_fl (void *, const char *n, float v)              { record_f(n, "float",  v); }
static void  set_ull(void *, const char *n, unsigned long long v) { record_u(n, "uint64", v); }

// 1 = NVSDK_NGX_Result_Fail. Report "not present" for everything the snippet
// reads back; it only reads what it has set, and those come from our map.
static unsigned get_generic(const char *n, unsigned long long *out) {
    auto it = g_ints.find(n ? n : "");
    if (it == g_ints.end()) return 0xBAD00001;
    *out = it->second;
    return 1;   // NVSDK_NGX_Result_Success
}
static unsigned get_pv (void *, const char *n, void **o)              { unsigned long long v; unsigned r = get_generic(n, &v); if (r == 1) *o = (void *)v; return r; }
static unsigned get_d12(void *, const char *n, void **o)              { return get_pv(nullptr, n, o); }
static unsigned get_d11(void *, const char *n, void **o)              { return get_pv(nullptr, n, o); }
static unsigned get_i  (void *, const char *n, int *o)                { unsigned long long v; unsigned r = get_generic(n, &v); if (r == 1) *o = (int)v; return r; }
static unsigned get_u  (void *, const char *n, unsigned *o)           { unsigned long long v; unsigned r = get_generic(n, &v); if (r == 1) *o = (unsigned)v; return r; }
static unsigned get_db (void *, const char *n, double *o)             { (void)n; (void)o; return 0xBAD00001; }
static unsigned get_fl (void *, const char *n, float *o)              { (void)n; (void)o; return 0xBAD00001; }
static unsigned get_ull(void *, const char *n, unsigned long long *o) { return get_generic(n, o); }
static void     do_reset(void *) { g_sets.clear(); g_ints.clear(); }

static void *g_vtbl[17] = {
    (void *)&set_pv, (void *)&set_d12, (void *)&set_d11, (void *)&set_i,
    (void *)&set_u,  (void *)&set_db,  (void *)&set_fl,  (void *)&set_ull,
    (void *)&get_pv, (void *)&get_d12, (void *)&get_d11, (void *)&get_i,
    (void *)&get_u,  (void *)&get_db,  (void *)&get_fl,  (void *)&get_ull,
    (void *)&do_reset,
};
struct FakeParam { void **vtbl = g_vtbl; };

// ----------------------------------------------------------------- main ---

// Argument order read off the export at rva 0x178b0, not from a header:
//   rcx appId, rdx dataPath, r8 device, r9d sdkVersion, stack featureInfo.
typedef unsigned (__cdecl *PFN_Init_Ext)(unsigned long long, const wchar_t *,
                                         ID3D12Device *, unsigned, const void *);
typedef unsigned (__cdecl *PFN_Populate)(void *);
typedef unsigned (__cdecl *PFN_Shutdown)(void);
typedef unsigned (__cdecl *PFN_GetArch)(void);
// rcx command list, edx feature id, r8 parameters, r9 out handle
typedef unsigned (__cdecl *PFN_Create)(ID3D12GraphicsCommandList *, unsigned, void *, void **);
typedef unsigned (__cdecl *PFN_Release)(void *);

int main(int argc, char **argv) {
    if (argc < 2) { std::printf("usage: snippet_probe <nvngx_dlssg.dll> [appId] [featureId]\n"); return 2; }
    const unsigned long long appId = (argc > 2) ? strtoull(argv[2], nullptr, 0) : 241534723ull;
    const unsigned feature = (argc > 3) ? (unsigned)strtoul(argv[3], nullptr, 0) : 10u;

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
            SUCCEEDED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&dev))) {
            std::wprintf(L"adapter        : %s (vendor 0x%04X device 0x%04X)\n",
                         d.Description, d.VendorId, d.DeviceId);
            break;
        }
        ad->Release(); ad = nullptr;
    }
    if (dev == nullptr) { std::printf("no D3D12 device\n"); return 1; }

    HMODULE h = LoadLibraryA(argv[1]);
    if (h == nullptr) { std::printf("LoadLibrary(%s) failed: %lu\n", argv[1], GetLastError()); return 1; }

    auto arch = (PFN_GetArch)GetProcAddress(h, "NVSDK_NGX_GetGPUArchitecture");
    if (arch) std::printf("snippet built for: 0x%X\n", arch());

    auto init = (PFN_Init_Ext)GetProcAddress(h, "NVSDK_NGX_D3D12_Init_Ext");
    auto pop  = (PFN_Populate)GetProcAddress(h, "NVSDK_NGX_D3D12_PopulateParameters_Impl");
    auto shut = (PFN_Shutdown)GetProcAddress(h, "NVSDK_NGX_D3D12_Shutdown");
    if (init == nullptr || pop == nullptr) { std::printf("missing exports\n"); return 1; }

    // 0x13 is the NGXApiVersion the snippet advertises in its version resource.
    unsigned r = init(appId, L".", dev, 0x13, nullptr);
    std::printf("Init_Ext       : 0x%08X%s\n", r, (r & 0xFFF00000) == 0xBAD00000 ? "  (failed)" : "");

    FakeParam p;
    unsigned rp = pop(&p);
    std::printf("Populate       : 0x%08X\n\n", rp);

    std::printf("%-44s %-8s %s\n", "parameter", "type", "value");
    std::printf("%-44s %-8s %s\n", "--------------------------------------------", "--------", "-----");
    for (const auto &s : g_sets) {
        if (s.type == "float" || s.type == "double")
            std::printf("%-44s %-8s %g\n", s.name.c_str(), s.type.c_str(), s.f);
        else
            std::printf("%-44s %-8s %llu (0x%llX)\n", s.name.c_str(), s.type.c_str(), s.u, s.u);
    }

    // Creating the feature exercises EndpointCore::Create, which is where the
    // other architecture gate writes m_multiFrameSupported.  Nothing here needs
    // a tagged resource: EndpointCoreCreateParameters::ParseNGXParameters reads
    // only scalars, so a command list and a handful of numbers are enough.
    auto create  = (PFN_Create)GetProcAddress(h, "NVSDK_NGX_D3D12_CreateFeature");
    auto release = (PFN_Release)GetProcAddress(h, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (create != nullptr && (rp & 0xFFF00000) != 0xBAD00000) {
        ID3D12CommandAllocator *alloc = nullptr;
        ID3D12GraphicsCommandList *cl = nullptr;
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    __uuidof(ID3D12CommandAllocator), (void **)&alloc);
        if (alloc)
            dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                   __uuidof(ID3D12GraphicsCommandList), (void **)&cl);
        if (cl == nullptr) {
            std::printf("\n(no command list, skipping CreateFeature)\n");
        } else {
            const unsigned W = 2560, H = 1440, iW = 1280, iH = 720;
            set_u(&p, "DLSSG.Width",             W);
            set_u(&p, "DLSSG.Height",            H);
            set_u(&p, "DLSSG.InternalWidth",     iW);
            set_u(&p, "DLSSG.InternalHeight",    iH);
            set_u(&p, "Width",                   W);
            set_u(&p, "Height",                  H);
            set_u(&p, "DLSSG.BackbufferFormat",  DXGI_FORMAT_R16G16B16A16_FLOAT);
            set_u(&p, "DLSSG.DynamicResolution", 0);
            set_u(&p, "DLSSG.UseReflexMatrices", 0);
            set_u(&p, "CreationNodeMask",        1);
            set_u(&p, "VisibilityNodeMask",      1);
            const size_t before = g_sets.size();
            void *handle = nullptr;
            unsigned rc = create(cl, feature, &p, &handle);
            std::printf("\nCreateFeature  : 0x%08X%s   (feature %u, %ux%u from %ux%u)\n",
                        rc, (rc & 0xFFF00000) == 0xBAD00000 ? "  FAILED" : "  ok",
                        feature, W, H, iW, iH);
            for (size_t i = before; i < g_sets.size(); ++i)
                std::printf("    set by create: %-40s %s\n",
                            g_sets[i].name.c_str(), g_sets[i].type.c_str());
            if (handle != nullptr && release != nullptr) release(handle);
            cl->Close();
            cl->Release();
        }
        if (alloc != nullptr) alloc->Release();
    }

    auto it = g_ints.find("DLSSG.MultiFrameCountMax");
    std::printf("\n==> DLSSG.MultiFrameCountMax = %s\n",
                it == g_ints.end() ? "(not reported)" : std::to_string(it->second).c_str());
    if (it != g_ints.end())
        std::printf("==> maximum multiplier       = %llux\n", it->second + 1);

    if (shut) shut();
    return 0;
}
