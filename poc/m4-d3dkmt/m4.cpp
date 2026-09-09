// M4: D3DKMT sin privilegios. Que estadisticas de presentacion da, y si sirven
// por proceso.
//
// Son los thunks de modo kernel que expone gdi32.dll. No son ETW, asi que en
// principio no necesitan elevacion -- esa es justamente la pregunta.
//
// La POC hace tres cosas, en orden de menor a mayor compromiso:
//
//   1. Comprueba que los entry points existan en gdi32.
//   2. Abre el adaptador del escritorio y consulta estadisticas del proceso.
//   3. Presenta 120 frames y vuelve a consultar, para ver si algun contador
//      sigue a la aplicacion.
//
// Las estructuras de D3DKMT son enormes y cambian entre versiones de Windows, y
// no estan en los headers de MinGW. Aca NO se declaran a mano campo por campo:
// se pasa un buffer grande y en cero y se mira el codigo de retorno y los
// primeros enteros. Inventar un layout seria adivinar, que es lo que este
// proyecto ya aprendio a no hacer.
//
// Compilar:
//   g++ -std=c++17 -O2 -o m4.exe m4.cpp -ld3d11 -ldxgi -lgdi32 -luser32 -lole32

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstring>

typedef LONG NTSTATUS_;

// D3DKMT_QUERYSTATISTICS es muy grande; se reserva de sobra.
struct BufferGrande { unsigned char b[4096]; };

typedef NTSTATUS_(WINAPI *PFN_QueryStatistics)(void *);
typedef NTSTATUS_(WINAPI *PFN_OpenAdapterFromHdc)(void *);
typedef NTSTATUS_(WINAPI *PFN_CloseAdapter)(void *);

int main(void) {
    HMODULE g = GetModuleHandleW(L"gdi32.dll");
    if (g == nullptr) g = LoadLibraryW(L"gdi32.dll");
    if (g == nullptr) { printf("NO SE PUDO PROBAR: no se pudo abrir gdi32\n"); return 2; }

    const char *nombres[] = {
        "D3DKMTQueryStatistics", "D3DKMTOpenAdapterFromHdc",
        "D3DKMTCloseAdapter", "D3DKMTGetPresentHistory",
        "D3DKMTQueryAdapterInfo", "D3DKMTGetDeviceState",
    };
    printf("=== entry points en gdi32:\n");
    for (const char *n : nombres)
        printf("  %-28s %s\n", n, GetProcAddress(g, n) ? "SI" : "no");

    PFN_QueryStatistics QueryStatistics =
        (PFN_QueryStatistics)GetProcAddress(g, "D3DKMTQueryStatistics");
    if (QueryStatistics == nullptr) {
        printf("\nNO ANDA: D3DKMTQueryStatistics no esta exportada\n");
        return 1;
    }

    // Layout del PRINCIPIO de D3DKMT_QUERYSTATISTICS, que es la parte estable:
    //   +0  D3DKMT_QUERYSTATISTICS_TYPE  Type
    //   +8  LUID                          AdapterLuid
    //   +16 HANDLE                        hProcess
    //   +24 union grande de resultados
    // El tipo 1 es D3DKMT_QUERYSTATISTICS_PROCESS.
    // El LUID del adaptador sale de DXGI, que no necesita nada especial.
    LUID luid = {};
    {
        IDXGIFactory1 *f = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f)) && f) {
            IDXGIAdapter1 *a = nullptr;
            if (SUCCEEDED(f->EnumAdapters1(0, &a)) && a) {
                DXGI_ADAPTER_DESC1 d = {};
                if (SUCCEEDED(a->GetDesc1(&d))) luid = d.AdapterLuid;
                a->Release();
            }
            f->Release();
        }
    }
    printf("\n  LUID del adaptador: %08lX-%08lX\n",
           (unsigned long)luid.HighPart, (unsigned long)luid.LowPart);

    BufferGrande q;
    memset(&q, 0, sizeof(q));
    *(unsigned *)(q.b + 0) = 1;                       // Type = PROCESS
    *(LUID *)(q.b + 8) = luid;
    *(void **)(q.b + 16) = (void *)GetCurrentProcess();

    const NTSTATUS_ st = QueryStatistics(&q);
    printf("\n=== D3DKMTQueryStatistics(PROCESS) sin elevar\n");
    printf("  NTSTATUS: 0x%08lX  %s\n", (unsigned long)st,
           st == 0 ? "(exito)" : "(fallo)");
    if (st != 0) {
        printf("\nNO ANDA sin mas contexto: la llamada fallo. Sin el LUID del\n");
        printf("adaptador el tipo PROCESS no alcanza, y declarar el resto de la\n");
        printf("estructura a mano seria adivinar el layout.\n");
        return 1;
    }

    printf("  primeros enteros del resultado:");
    for (int i = 0; i < 8; ++i) printf(" %u", *(unsigned *)(q.b + 24 + i * 4));
    printf("\n");

    // Si llego hasta aca, se mide contra una cantidad conocida.
    printf("\n=== ahora con 120 presentaciones de por medio\n");
    BufferGrande antes = q;
    (void)antes;
    printf("  (no implementado: solo tiene sentido si la consulta ya devolvio\n");
    printf("   algo util, y la forma del resultado hay que confirmarla primero)\n");
    return 0;
}
