// M7: oraculo externo SIN elevacion, usando el SDK de FrameView.
//
// PresentMon necesita una sesion ETW en tiempo real y por lo tanto elevacion.
// Pero el SDK de FrameView, que ya esta instalado en esta maquina, inicializa
// sin elevar -- su cliente de prueba imprime "InitializeFvSDKSession SUCCESS"
// desde un proceso comun. Es un oraculo igual de independiente de nuestro
// codigo: lo hizo NVIDIA y mide por su cuenta.
//
// La POC lanza `presentador.exe`, que presenta una cantidad conocida e imprime
// su propio contador, y le pide al SDK el FPS de ese proceso. Los dos numeros
// vienen de caminos que no comparten nada.
//
// El header se incluye de su ubicacion real en vez de redeclarar las
// estructuras: adivinar layouts es lo que dejo a M4 sin veredicto.
//
// Compilar:
//   g++ -std=c++17 -O2 -o oraculo.exe oraculo.cpp -luser32

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace fvh {
#include "C:/Program Files/NVIDIA Corporation/FrameViewSDK/SDK/Public_Release/FvSDK.h"
}
using fvh::FvStatus; using fvh::FvSession; using fvh::FvMetricType;
using fvh::FvSampleDataFlags; using fvh::Samples; using fvh::AvgFPS;
using fvh::FV_SUCCESS; using fvh::eAvgFPS; using fvh::FVSDK_SAMPLEDATA_DONOTWAIT;

// El dll exporta UNA sola funcion, fv_QueryInterface -- el mismo patron que
// NVAPI. Los stubs que resuelven cada entry point viven en el .lib del SDK.
//
// El .lib es de MSVC y sus simbolos estan mangleados a la manera de MSVC, que
// MinGW no genera. En vez de adivinar como llamar a fv_QueryInterface -- que
// seria inventar una convencion, el mismo error que dejo a M4 sin veredicto --
// se referencian los simbolos EXACTOS con etiquetas asm. Los nombres salieron de
// leer la tabla de simbolos del propio .lib.
//
// FvStatus es un enum, asi que vuelve en eax y no hay problema de ABI por
// devolver un objeto por valor.

FvStatus FvSDK_Initialize()
    asm("\"?FvSDK_Initialize@@YA?AW4FvStatus@@XZ\"");
FvStatus FvSDK_CreateSession(FvSession *)
    asm("\"?FvSDK_CreateSession@@YA?AW4FvStatus@@PEAPEAUFvSession__@@@Z\"");
FvStatus FvSDK_StartSession(FvSession)
    asm("\"?FvSDK_StartSession@@YA?AW4FvStatus@@PEAUFvSession__@@@Z\"");
FvStatus FvSDK_EnableMetrics(FvSession, FvMetricType *, unsigned int)
    asm("\"?FvSDK_EnableMetrics@@YA?AW4FvStatus@@PEAUFvSession__@@PEAW4_FvMetricType@@I@Z\"");
FvStatus FvSDK_SampleData(FvSession, FvSampleDataFlags)
    asm("\"?FvSDK_SampleData@@YA?AW4FvStatus@@PEAUFvSession__@@W4_FvSampleDataFlags@@@Z\"");
FvStatus FvSDK_ReadData(FvSession, Samples *, unsigned int)
    asm("\"?FvSDK_ReadData@@YA?AW4FvStatus@@PEAUFvSession__@@PEAUSamples_v1@@I@Z\"");
FvStatus FvSDK_StopSession(FvSession)
    asm("\"?FvSDK_StopSession@@YA?AW4FvStatus@@PEAUFvSession__@@@Z\"");
FvStatus FvSDK_DestroySession(FvSession)
    asm("\"?FvSDK_DestroySession@@YA?AW4FvStatus@@PEAUFvSession__@@@Z\"");
FvStatus FvSDK_Shutdown()
    asm("\"?FvSDK_Shutdown@@YA?AW4FvStatus@@XZ\"");

// El .lib viene compilado con MSVC y referencia intrinsecas de su CRT que MinGW
// no tiene: la proteccion de pila (/GS). Se les pone un cabo minimo.
//
// SALVEDAD: el cabo de __GSHandlerCheck no hace la comprobacion real. Para una
// POC de medicion alcanza -- si el SDK corrompiera la pila nos enteramos por
// otro lado -- pero NO es codigo para meter en el dll de produccion asi como
// esta.
extern "C" {
unsigned long long __security_cookie = 0x00002B992DDFA232ULL;
void __security_check_cookie(unsigned long long) {}
int __GSHandlerCheck(void *, void *, void *, void *) { return 1; }  // ExceptionContinueSearch
}

// strsafe: el .lib la usa y viene inline en el header de MSVC, asi que no esta
// en ninguna dll. Se implementa. S_OK = 0, STRSAFE_E_INSUFFICIENT_BUFFER es
// 0x8007007A.
long StringCchCopyW(wchar_t *dst, size_t cch, const wchar_t *src)
    asm("\"?StringCchCopyW@@YAJPEA_W_KPEB_W@Z\"");
long StringCchCopyW(wchar_t *dst, size_t cch, const wchar_t *src) {
    if (dst == nullptr || cch == 0) return (long)0x80070057L;   // E_INVALIDARG
    size_t i = 0;
    for (; src != nullptr && src[i] != 0 && i + 1 < cch; ++i) dst[i] = src[i];
    dst[i] = 0;
    const bool trunco = (src != nullptr && src[i] != 0);
    return trunco ? (long)0x8007007AL : 0L;
}

// StringCbCopyW cuenta BYTES, no caracteres.
long StringCbCopyW(wchar_t *dst, size_t cb, const wchar_t *src)
    asm("\"?StringCbCopyW@@YAJPEA_W_KPEB_W@Z\"");
long StringCbCopyW(wchar_t *dst, size_t cb, const wchar_t *src) {
    return StringCchCopyW(dst, cb / sizeof(wchar_t), src);
}

long StringCchCatW(wchar_t *dst, size_t cch, const wchar_t *src)
    asm("\"?StringCchCatW@@YAJPEA_W_KPEB_W@Z\"");
long StringCchCatW(wchar_t *dst, size_t cch, const wchar_t *src) {
    if (dst == nullptr || cch == 0) return (long)0x80070057L;
    size_t n = 0;
    while (n < cch && dst[n] != 0) ++n;
    if (n >= cch) return (long)0x80070057L;
    return StringCchCopyW(dst + n, cch - n, src);
}

int main(int argc, char **argv) {
    const int segundos = (argc > 1) ? atoi(argv[1]) : 15;

    printf("elevado: %s\n", "no (a proposito: se prueba que no hace falta)");

    FvStatus st = ::FvSDK_Initialize();
    printf("FvSDK_Initialize: %d\n", (int)st);
    if (st != FV_SUCCESS) { printf("NO ANDA: el SDK no inicializo\n"); return 1; }

    FvSession ses = nullptr;
    st = ::FvSDK_CreateSession(&ses);
    printf("FvSDK_CreateSession: %d\n", (int)st);

    FvMetricType metricas[] = { eAvgFPS };
    st = ::FvSDK_EnableMetrics(ses, metricas, 1);
    printf("::FvSDK_EnableMetrics(eAvgFPS): %d\n", (int)st);

    st = ::FvSDK_StartSession(ses);
    printf("FvSDK_StartSession: %d\n", (int)st);

    // El presentador: la verdad conocida.
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "presentador.exe %d", segundos + 4);
    STARTUPINFOA si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        printf("NO SE PUDO PROBAR: no se pudo lanzar presentador.exe (%lu)\n", GetLastError());
        return 2;
    }
    printf("presentador lanzado, pid %lu\n", pi.dwProcessId);
    Sleep(2500);

    double ultimo = 0.0;
    int leidas = 0;
    for (int i = 0; i < segundos; ++i) {
        Sleep(1000);
        ::FvSDK_SampleData(ses, FVSDK_SAMPLEDATA_DONOTWAIT);
        AvgFPS buf[32];
        Samples s;
        s.type = eAvgFPS;
        s.mData = buf;
        s.mNumSamples = 32;
        const FvStatus r = ::FvSDK_ReadData(ses, &s, 32);
        if (r != FV_SUCCESS) continue;
        for (size_t k = 0; k < s.mNumSamples && k < 32; ++k) {
            if (buf[k].mFPS.PID == (unsigned long long)pi.dwProcessId &&
                buf[k].mFPS.AvgFPS > 0.0) {
                ultimo = buf[k].mFPS.AvgFPS;
                ++leidas;
                printf("  t=%2ds  pid %llu  AvgFPS %.2f\n", i + 1,
                       (unsigned long long)buf[k].mFPS.PID, buf[k].mFPS.AvgFPS);
            }
        }
    }

    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);

    ::FvSDK_StopSession(ses);
    ::FvSDK_DestroySession(ses);
    ::FvSDK_Shutdown();

    printf("\nmuestras con el pid del presentador: %d\n", leidas);
    if (leidas == 0) {
        printf("NO ANDA: el SDK nunca reporto ese proceso\n");
        return 1;
    }
    printf("ultimo AvgFPS del oraculo: %.2f\n", ultimo);
    printf("(comparar contra la linea PRESENTADOR de la otra consola)\n");
    return 0;
}
