// proxy.cpp -- el DLL que se copia al lado del juego.
//
// ENTRA: el cargador de Windows, que mapea este archivo como version.dll.
// SALE: los exports de version.dll reenviados al de sistema, y gpuprobe
//   enganchado sobre D3D12 cuando el juego lo use.
// DEPENDE DE: forwards_version.S (los stubs), hooks.cpp, gate.cpp.
//
// Por que version.dll y no dxgi.dll: dxgi.dll tiene exports privados que usan
// d3d11 y d3d12 por dentro (DXGID3D10CreateDevice y companiaa), y un proxy que
// no los reenvia rompe el arranque de cualquier juego. version.dll tiene 17
// exports, todos documentados, y nadie le pide nada raro. Es lo que ya usa
// mfg-unlock en este mismo repositorio, por la misma razon.
//
// Y por que no hace falta MinHook: las vtables de D3D12 y DXGI viven en el
// runtime y son las mismas para todo el proceso, asi que crear un device
// descartable propio y enganchar SU vtable engancha la del juego. Cero
// trampolines, cero parches sobre codigo ajeno, cero dependencias.
//
// Regla dura: si algo de esto falla, no pasa nada. El juego llama a
// GetFileVersionInfo, le contestamos lo que contestaria el de sistema, y
// gpuprobe se queda apagado.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

#include "d3d12/gate.h"
#include "d3d12/hooks.h"
#include "forwards_version_names.h"

namespace {

HMODULE g_real = nullptr;
FARPROC g_resolved[sizeof(kVersionExports) / sizeof(kVersionExports[0])] = {};

void load_real() {
    if (g_real) return;
    char path[MAX_PATH];
    const UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::snprintf(path + n, sizeof(path) - n, "\\version.dll");
    g_real = LoadLibraryA(path);
}

// El hilo de descubrimiento. NO se puede hacer nada de esto adentro de
// DllMain: crear un device de D3D12 desde el loader lock es colgar el proceso.
DWORD WINAPI discover(LPVOID) {
    // El juego tarda en cargar d3d12.dll. Se espera hasta dos minutos y se
    // revisa cada 200 ms; si no aparece, no es un juego de D3D12 y gpuprobe se
    // queda quieto para siempre.
    for (int i = 0; i < 600; ++i) {
        if (GetModuleHandleA("d3d12.dll") && GetModuleHandleA("dxgi.dll")) {
            // Un respiro: el juego acaba de cargar el runtime, todavia no creo
            // su device. Enganchar ahora es llegar antes sin correr carrera.
            Sleep(50);
            if (gp::hooks_steal_vtables()) {
                gp::gate_log("gpuprobe enganchado; esperando el primer Present");
            } else {
                gp::gate_log("no se pudieron robar las vtables: passthrough");
            }
            return 0;
        }
        Sleep(200);
    }
    gp::gate_log("no aparecio d3d12.dll en dos minutos: passthrough");
    return 0;
}

}  // namespace

// Lo llama cada stub de forwards_version.S con su indice. Resuelve una vez y
// cachea; si el export no existe en el de sistema -- no deberia pasar nunca --
// devuelve un puntero a una funcion que no hace nada en vez de saltar a cero.
extern "C" FARPROC forward_resolve(unsigned index) {
    const unsigned count = sizeof(kVersionExports) / sizeof(kVersionExports[0]);
    if (index >= count) return nullptr;
    if (g_resolved[index]) return g_resolved[index];
    load_real();
    if (!g_real) return nullptr;
    FARPROC p = GetProcAddress(g_real, kVersionExports[index]);
    if (!p)
        gp::gate_log("version.dll del sistema no exporta %s", kVersionExports[index]);
    g_resolved[index] = p;
    return p;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        load_real();
        gp::gate_log("gpuprobe cargado en %s", []() -> const char * {
            static char exe[MAX_PATH] = "?";
            char full[MAX_PATH];
            if (GetModuleFileNameA(nullptr, full, MAX_PATH)) {
                const char *slash = strrchr(full, '\\');
                std::snprintf(exe, sizeof exe, "%s", slash ? slash + 1 : full);
            }
            return exe;
        }());
        // Un archivo al lado del dll apaga gpuprobe sin desinstalarlo: es lo
        // primero que hay que probar cuando algo falla y no se sabe de quien
        // es la culpa.
        if (GetFileAttributesA("gpuprobe-off.txt") != INVALID_FILE_ATTRIBUTES) {
            gp::gate_degrade("gpuprobe-off.txt presente");
            return TRUE;
        }
        HANDLE t = CreateThread(nullptr, 0, discover, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
