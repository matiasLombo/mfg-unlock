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

// ------------------------------------------------- catching the dll load ---

struct DllNotifyData {
    ULONG Flags;
    const UNICODE_STRING *FullDllName;
    const UNICODE_STRING *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
typedef VOID(CALLBACK *PFN_Notify)(ULONG, const DllNotifyData *, PVOID);
typedef NTSTATUS(NTAPI *PFN_LdrRegister)(ULONG, PFN_Notify, PVOID, PVOID *);

static bool name_is(const UNICODE_STRING *s, const wchar_t *want) {
    if (s == nullptr || s->Buffer == nullptr) return false;
    const size_t n = s->Length / sizeof(wchar_t);
    size_t i = 0;
    for (; i < n && want[i] != 0; ++i) {
        wchar_t a = s->Buffer[i], b = want[i];
        if (a >= L'A' && a <= L'Z') a = (wchar_t)(a + 32);
        if (b >= L'A' && b <= L'Z') b = (wchar_t)(b + 32);
        if (a != b) return false;
    }
    return i == n && want[i] == 0;
}

static VOID CALLBACK on_dll_load(ULONG reason, const DllNotifyData *d, PVOID) {
    if (reason != 1 || d == nullptr) return;                 // 1 = LDR_DLL_NOTIFICATION_REASON_LOADED
    if (!name_is(d->BaseDllName, L"nvngx_dlssg.dll")) return;
    const int n = patch_gates(reinterpret_cast<unsigned char *>(d->DllBase));
    // NGX maps the snippet more than once; later passes find the immediates
    // already rewritten and report nothing, so keep the pass that did the work.
    if (n > 0 && g_gates <= 0) {
        g_gates = n;
        log_num("nvngx_dlssg.dll mapped, architecture gates rewritten: ", (unsigned)n);
    }
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
    log_line("--- mfg-unlock attached ---");

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
