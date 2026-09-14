// gate.cpp -- ver gate.h.
#define WIN32_LEAN_AND_MEAN
#include "gate.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace gp {

namespace {

std::atomic<Phase>  g_phase{Phase::Passthrough};
thread_local int    g_depth = 0;
std::mutex          g_log_mu;
FILE               *g_log = nullptr;
HMODULE             g_self = nullptr;
unsigned char      *g_self_base = nullptr;
size_t              g_self_size = 0;
PVOID               g_veh = nullptr;

void open_log() {
    if (g_log) return;
    char local[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH)) return;
    char dir[MAX_PATH * 2];
    std::snprintf(dir, sizeof dir, "%s\\gpuprobe", local);
    CreateDirectoryA(dir, nullptr);
    char path[MAX_PATH * 2];
    std::snprintf(path, sizeof path, "%s\\gpuprobe.log", dir);
    g_log = fopen(path, "wb");
}

// Donde vive nuestro modulo, para poder decir si una excepcion fue nuestra.
void locate_self() {
    if (g_self) return;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&locate_self), &g_self);
    if (!g_self) return;
    g_self_base = reinterpret_cast<unsigned char *>(g_self);
    const IMAGE_DOS_HEADER *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(g_self_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const IMAGE_NT_HEADERS *nt =
        reinterpret_cast<const IMAGE_NT_HEADERS *>(g_self_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    g_self_size = nt->OptionalHeader.SizeOfImage;
}

bool inside_self(const void *addr) {
    if (!g_self_base || !g_self_size) return false;
    const unsigned char *p = static_cast<const unsigned char *>(addr);
    return p >= g_self_base && p < g_self_base + g_self_size;
}

// El testigo. No traga nada y no intenta recuperarse: mira si la falla fue
// NUESTRA, y si lo fue, lo deja escrito y apaga gpuprobe para lo que quede de
// la corrida. Devuelve siempre CONTINUE_SEARCH, asi que el juego y el driver
// ven la excepcion exactamente igual que sin nosotros.
LONG CALLBACK witness(EXCEPTION_POINTERS *info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // Las de control de flujo son normales y ruidosas: breakpoints del
    // depurador, excepciones de C++, y las de primera oportunidad que usan
    // varios motores a proposito.
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP ||
        code == 0xE06D7363u /* C++ EH */ || code == 0x406D1388u /* nombre de hilo */)
        return EXCEPTION_CONTINUE_SEARCH;

    if (inside_self(info->ExceptionRecord->ExceptionAddress)) {
        const size_t off = static_cast<size_t>(
            static_cast<unsigned char *>(info->ExceptionRecord->ExceptionAddress) -
            g_self_base);
        gate_log("EXCEPCION NUESTRA code=0x%08lx en gpuprobe+0x%zx -- apagando",
                 static_cast<unsigned long>(code), off);
        gate_degrade("excepcion dentro de gpuprobe");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

Phase gate_phase() { return g_phase.load(std::memory_order_acquire); }
bool  gate_is(Phase p) { return gate_phase() == p; }

void gate_log(const char *fmt, ...) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    open_log();
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(g_log, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond,
                 st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

bool gate_arm(ID3D12Device *device, const char *exe) {
    if (!device) return false;
    Phase expected = Phase::Passthrough;
    if (!g_phase.compare_exchange_strong(expected, Phase::Armed)) {
        // Ya estamos armados, o ya nos apagamos. En los dos casos, no se
        // reintenta: reintentar algo que ya fallo adentro de un juego es como
        // se fabrica un crash intermitente.
        return gate_phase() == Phase::Armed || gate_phase() == Phase::Verified;
    }
    locate_self();
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, witness);
    gate_log("armado sobre %s (device %p)", exe ? exe : "?",
             static_cast<void *>(device));
    return true;
}

void gate_verify() {
    Phase expected = Phase::Armed;
    if (g_phase.compare_exchange_strong(expected, Phase::Verified))
        gate_log("verificado: el primer frame completo llego al colector");
}

void gate_degrade(const char *why) {
    const Phase before = g_phase.exchange(Phase::Degraded, std::memory_order_acq_rel);
    if (before != Phase::Degraded)
        gate_log("APAGADO (%s). El juego sigue sin gpuprobe.", why ? why : "?");
}

Reentry::Reentry() {
    if (g_depth == 0) {
        ok_ = true;
        g_depth = 1;
    }
}

Reentry::~Reentry() {
    if (ok_) g_depth = 0;
}

bool reentrant() { return g_depth != 0; }

}  // namespace gp
