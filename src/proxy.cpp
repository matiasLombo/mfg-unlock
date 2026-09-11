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
#include "policy.h"
#include "config.h"
#include "diag.h"
#include "controller.h"
#include "scheduler.h"
#include "sites.h"
#include "state.h"

static void dyn_control(double base_fps, double presented_fps);  // definida mas abajo
static void read_previous_verdict(void);
static void save_consent(int si);
static bool version_supported(const wchar_t *path, unsigned *major, unsigned *minor_v);
static void emit_verdict_if_due(void);   // M1, definida mas abajo
static bool state_path(wchar_t *out, int max);            // M1, idem
static void dyn_apply(double base_fps);                          // definida mas abajo
static void log_line(const char *text);
static void log_num(const char *label, unsigned long long v);
// Capa 0: cual copia EJECUTA, atada por el puntero que devuelve el interposer.
static void executing_copy(const void *fn, const char *name);

static inline bool phase_active(void) { return g_phase == (LONG)Phase::ACTIVE; }
static inline bool phase_passive(void) { return g_phase == (LONG)Phase::PASSIVE; }
static void evaluate_invariants(void);
// Alimenta g_present_count desde el runtime cuando no hay hook de Present.
static void runtime_presents(void);

// ------------------------------------------------------------------- log ---
//
// Raw file calls, no CRT: some of this runs under the loader lock.

static wchar_t g_log[MAX_PATH];

// True when a file of this name sits beside the dll. The switches are files
// because the person installing this has the dll and nothing else, and the
// name is measured rather than counted by hand -- the open-coded copies each
// ended in a literal length that had to agree with the string above it, and a
// count that does not agree leaves the path unterminated and the check reads
// whatever follows on the stack, silently.
// The path of a file beside the dll, into `out`.
static void beside_dll(wchar_t *out, const wchar_t *name) {
    int j = 0;
    while (g_log[j] != 0 && j < MAX_PATH - 1) { out[j] = g_log[j]; ++j; }
    while (j > 0 && out[j - 1] != 0x5C) --j;
    int i = 0;
    while (name[i] != 0 && j + i < MAX_PATH - 1) { out[j + i] = name[i]; ++i; }
    out[j + i] = 0;
}

static bool flag_file(const wchar_t *name) {
    wchar_t p[MAX_PATH];
    beside_dll(p, name);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}


// Se abre y se cierra por linea, a proposito. Se probo dejar el handle abierto
// -- seis llamadas al sistema por linea contra una sola apertura -- y en GTA V
// el log murio despues del attach: salieron las lineas del hilo del loader y
// ninguna de los otros hilos. sl.log seguia vivo y DLSS-G corriendo con la
// cuenta alternando, asi que la dll andaba y solo habia dejado de escribir.
// Revertido sin diagnosticar del todo: el cambio no compraba nada medido y
// rompia leer el log con el juego abierto, que es como se diagnostica todo aca.
// Rafaga: juntar el volcado de una ventana en UNA sola apertura de archivo.
//
// log_line abre y cierra el archivo por linea a proposito, para que el log se
// pueda leer en vivo ([[log-must-stay-readable-live]]). El costo se paga por
// APERTURA, no por linea, y al cerrar cada ventana de medicion se escriben unas
// sesenta seguidas -- histogramas, sondas, las dos bases -- en el hilo de
// render.
//
// Medido en una sesion de GTA V de 726 s: 63611 lineas, 88 por segundo de media
// y picos de 360. A base 40 son mas de dos aperturas por frame renderizado.
//
// Entre begin y end las lineas se acumulan y salen en una sola escritura. No se
// pierde una sola linea ni cambia el formato, y el archivo sigue siendo legible
// en vivo: una escritura por ventana es aproximadamente una por segundo. Si el
// buffer se llena se vuelca solo, asi que el peor caso es el de antes y no
// perder texto.
// El buffer es de UN hilo: el que abrio la rafaga.
//
// log_line lo llama cualquier hilo del proceso, incluido el que corre bajo el
// loader lock. Sin esta atadura, dos hilos harian read-modify-write sobre
// g_burst_n a la vez y el indice podria pasarse del buffer. El volcado de la
// ventana es del hilo de render y es el unico que necesita la rafaga; los
// demas siguen por el camino de siempre, una apertura por linea.
static char  g_burst[16384];
static int   g_burst_n = 0;
static bool  g_burst_on = false;
static LONG  g_burst_thread = 0;
static void  log_flush(void);

static void log_burst_begin(void) {
    g_burst_thread = (LONG)GetCurrentThreadId();
    g_burst_on = true;
}
static void log_burst_end(void) {
    log_flush();
    g_burst_on = false;
    g_burst_thread = 0;
}

static void log_escribir(const char *bytes, int len) {
    if (g_log[0] == 0) return;
    HANDLE h = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    SetFilePointer(h, 0, nullptr, FILE_END);
    WriteFile(h, bytes, (DWORD)len, &n, nullptr);
    CloseHandle(h);
}

static void log_flush(void) {
    if (g_burst_n <= 0) return;
    log_escribir(g_burst, g_burst_n);
    g_burst_n = 0;
}

static void log_line(const char *text) {
    if (g_log[0] == 0) return;
    // El camino de rafaga arma la linea en el buffer y no toca el disco.
    if (g_burst_on && (LONG)GetCurrentThreadId() == g_burst_thread) {
        static ULONGLONG tb = 0;
        if (tb == 0) tb = GetTickCount64();
        const unsigned long long ms = GetTickCount64() - tb;
        char ts[24];
        int k = 0, d[12], nd = 0;
        unsigned long long v = ms;
        ts[k++] = '[';
        if (v == 0) d[nd++] = 0;
        while (v > 0 && nd < 12) { d[nd++] = (int)(v % 10); v /= 10; }
        while (nd > 0) ts[k++] = (char)('0' + d[--nd]);
        ts[k++] = 'm'; ts[k++] = 's'; ts[k++] = ']'; ts[k++] = ' ';
        int len = 0;
        while (text[len] != 0) ++len;
        if (g_burst_n + k + len + 2 > (int)sizeof(g_burst)) log_flush();
        if (k + len + 2 <= (int)sizeof(g_burst)) {
            for (int i = 0; i < k; ++i) g_burst[g_burst_n++] = ts[i];
            for (int i = 0; i < len; ++i) g_burst[g_burst_n++] = text[i];
            g_burst[g_burst_n++] = '\r';
            g_burst[g_burst_n++] = '\n';
        }
        return;
    }
    HANDLE h = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    SetFilePointer(h, 0, nullptr, FILE_END);
    // Milliseconds since the dll attached, so this file can be lined up against
    // sl.log. Without it, "panel: shown" and "Out of order frame" cannot be put
    // on the same timeline, which is exactly the correlation under test: in GTA
    // V the three out-of-order frames follow a sync-state change, and the panel
    // is a separate window whose appearance can change the fullscreen flip
    // state. sl.log stamps every line; this one stamped none.
    {
        static ULONGLONG t0 = 0;
        if (t0 == 0) t0 = GetTickCount64();
        const unsigned long long ms = GetTickCount64() - t0;
        char ts[24];
        int k = 0, d[12], nd = 0;
        unsigned long long v = ms;
        ts[k++] = '[';
        if (v == 0) d[nd++] = 0;
        while (v > 0 && nd < 12) { d[nd++] = (int)(v % 10); v /= 10; }
        while (nd > 0) ts[k++] = (char)('0' + d[--nd]);
        ts[k++] = 'm'; ts[k++] = 's'; ts[k++] = ']'; ts[k++] = ' ';
        WriteFile(h, ts, (DWORD)k, &n, nullptr);
    }
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

#include "overlay.h"

static const unsigned char kDlssgOptionsGuid[16] = {
    0xcb, 0xf1, 0xc5, 0xfa,             // 0xfac5f1cb, little endian
    0xfd, 0x2d,                         // 0x2dfd
    0x36, 0x4f,                         // 0x4f36
    0xa1, 0xe6, 0x3a, 0x9e, 0x86, 0x52, 0x56, 0xc5
};

// Whether this Streamline knows DLSSGMode::eDynamic at all, decided from the
// plugin's own image rather than from the first slDLSSGSetOptions the game
// happens to make. Waiting for that call left the row greyed out on a build
// that supports it, for as long as the game had not touched its settings.

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


static const unsigned char kDlssgStateGuid[16] = {
    0xe1, 0xc8, 0x8a, 0xcc,             // 0xcc8ac8e1, little endian
    0x79, 0xa1,                         // 0xa179
    0xf5, 0x44,                         // 0x44f5
    0x97, 0xfa, 0xe7, 0x41, 0x12, 0xf9, 0xbc, 0x61
};

static PFN_slDLSSGGetState g_orig_getstate = nullptr;

static volatile LONG g_asked_state = 0;

// Probado en el sample del banco, que como Halo no pedia OTA: banderas 133 ->
// 205, de un sl.dlss_g mapeado se pasa a tres -- uno de ellos el 134656 de
// ProgramData, donde el parche del contador SI engancha -- y el fraccionario
// sigue entregando lo pedido: 3.50 pedido, 3.50x entregado (p10 3.44, p90
// 3.57) contra 3.51x sin OTA. Con tres copias mapeadas no se pierde nada.
#include "slinit.h"


// Con NUESTRO snippet, la cuenta ES el multiplicador.
//
// Medido en Halo, mismo dll, misma sesion:
//
//   snippet del juego   cuenta 1 -> 2X        (la cuenta son los GENERADOS)
//   snippet nuestro     cuenta 1 -> 1.2x      (la cuenta es el MULTIPLICADOR)
//                       cuenta 5 -> 5.0x
//
// La semantica de numFramesToGenerate cambio entre builds. Por eso el mapeo
// `modo v -> cuenta v-1` dejaba 2X sin generar y hacia que el modo 6 pidiera 5.
//
// Se decide por el flag leido AQUI y no por g_snippet_on: ese se llena en el
// bloque de flags, que corre DESPUES de restaurar los settings, y el modo fijo
// ya quedaba mapeado con el valor viejo. Leerlo bajo demanda saca el orden de
// la ecuacion. El archivo se mira una sola vez.
// Se enciende cuando el redirect del snippet REALMENTE ocurrio.
//
// Se probo decidirlo leyendo mfg-snippet.txt y no funciono: la primera consulta
// llega antes de que g_log tenga la ruta del dll, flag_file mira una ruta
// relativa y contesta que no. Un hecho observado -- la carga redirigida -- no
// tiene ese problema.
static volatile LONG g_snippet_loaded = 0;

static bool count_is_multiplier(void) {
    return g_snippet_loaded != 0;
}

// Que build de snippet quedo cargado, decidido por su CONTENIDO.
//
// Atarlo a un flag estaba mal por partida doble: el banco corre el snippet nuevo
// y no tiene ningun txt, asi que usaba la matematica vieja y el fraccional
// quedaba clavado -- pedido 2.55, entregado 2.00, mediana y p90 y max iguales.
//
// La build de julio decide su maximo por arquitectura con este patron:
//
//   41 B8 03 00 00 00    mov   r8d, 3
//   81 FF <imm32>        cmp   edi, 0x1b0
//   44 0F 4C C3          cmovl r8d, ebx
//
// En esa, numFramesToGenerate cuenta los GENERADOS. En las que no lo tienen,
// cuenta el MULTIPLICADOR. Se mira el modulo real mapeado, asi que da igual si
// llego por nuestra base, por la cache o por la carpeta del juego.
static void detect_semantics(unsigned char *base) {
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    auto *sec = IMAGE_FIRST_SECTION(nt);
    unsigned char *text = nullptr; size_t len = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const char *n = reinterpret_cast<const char *>(sec[i].Name);
        if (n[0]=='.'&&n[1]=='t'&&n[2]=='e'&&n[3]=='x'&&n[4]=='t') {
            text = base + sec[i].VirtualAddress; len = sec[i].Misc.VirtualSize; break;
        }
    }
    if (text == nullptr) return;
    bool julio = false;
    for (size_t i = 0; i + 16 <= len && !julio; ++i) {
        if (text[i] != 0x41 || text[i+1] != 0xB8 || text[i+2] != 3) continue;
        if (text[i+3] || text[i+4] || text[i+5]) continue;
        if (text[i+6] != 0x81 || text[i+7] != 0xFF) continue;
        if (text[i+12] != 0x44 || text[i+13] != 0x0F ||
            text[i+14] != 0x4C || text[i+15] != 0xC3) continue;
        julio = true;
    }
    const LONG before = g_snippet_loaded;
    g_snippet_loaded = julio ? 0 : 1;
    if (before != g_snippet_loaded)
        log_line(julio ? "semantica: la cuenta son los GENERADOS (snippet con tope 3)"
                       : "semantica: la cuenta es el MULTIPLICADOR");
}


static LONG count_cap(void) {
    (void)g_allow_x6;
    if (g_fixed_cap) return 5;
    // Lo que el plugin declara, si se pudo leer; 5 mientras tanto, que es como
    // venia. Ver leer_max_generados: pedir por encima de esto no entrega mas
    // frames, entrega CERO -- NGX rechaza la evaluacion entera.
    const LONG d = g_max_declared;
    // Hasta 6: el array de sub-frames tiene SEIS ranuras, asi que 6X es el
    // maximo estructural. El 5 de antes era un clamp nuestro y era el que
    // topaba una vez que el snippet y el plugin ya reportaban 6.
    // Sin mfg-seis.txt esto se comporta como siempre. El 6 es experimental y
    // dejarlo incondicional fue un error: rompio 2X sin que sacar el flag lo
    // devolviera.
    const LONG ceiling = (g_six || count_is_multiplier()) ? 6 : 5;
    return (d >= 1 && d <= ceiling) ? d : 5;
}
static void set_count_now(LONG n);   // definida mas abajo

// Que sigue [global+0x4168]: el techo declarado a la API, o lo generado?
//
// De eso depende el arreglo entero. En 0x52c80 -- la funcion que termina
// llamando a dlfgPresent con el indice que revienta -- el objeto sale de un
// puntero global en el RVA 0x8f1e8, y una funcion hermana en 0x52e31 valida su
// indice contra +0x4168 mientras la que falla no valida nada.
//
// Si el campo sigue al TECHO, entonces la presentacion cuenta con el numero que
// force_into declara (g_ciclo_techo) mientras la generacion sigue al byte de la
// cadencia, y en los frames bajos pide una ranura que nadie hizo. Ese es el
// mecanismo que hay que romper.
//
// Si sigue a lo GENERADO, entonces el indice sale de otro lado y toda esta linea
// de investigacion esta mal.
//
// Se lee, no se escribe. El RVA es de sl_dlss_g 134273; en otra build el numero
// que salga no significa nada y por eso se imprime crudo.
static bool read_ok(const void *src, void *dst, unsigned n) {
    if (src == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(src, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & readable) == 0) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    if ((ULONG_PTR)src + n > (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize) return false;
    memcpy(dst, src, n);
    return true;
}

// La marca de agua, bajada a la cuenta viva.
//
// Medido: [global+0x4168] se queda en el valor mas alto que se declaro alguna
// vez y NO baja. Con la cadencia alternando 2 y 3, el campo quedo clavado en 3
// durante 25 ventanas seguidas:
//
//     [global+0x4168]   API   byte
//            3           3     3
//            3           2     2      <- la generacion hace 2
//            3           2     2      <- y la presentacion recorre 3
//
// La presentacion recorre lo que dice el campo. En los frames de cadencia baja
// la ultima ranura no la hizo nadie, y ahi esta el puntero nulo. Coincide con
// el crash: el campo valia 3 y el indice pedido fue 3.
//
// Explica por que revienta DYNAMIC y no los modos fijos: un modo fijo nunca
// baja la cuenta, asi que la marca y la realidad coinciden siempre. DYNAMIC
// sube, la marca se queda arriba, y cada bajada posterior es un candidato.
//
// Bajarla es la direccion segura: recorrer menos ranuras de las que hay no
// rompe nada, recorrer una de mas es el crash. Nunca se sube -- si el plugin la
// quiere mas alta, alla el.
static void lower_4168(LONG v) {
    if (g_dlssg_base == nullptr || v < 1) return;
    unsigned char *pp = nullptr;
    if (!read_ok(g_dlssg_base + 0x8f1e8, &pp, sizeof(pp)) || pp == nullptr) return;
    LONG *field = (LONG *)(pp + 0x4168);
    LONG actual = 0;
    if (!read_ok(field, &actual, 4)) return;
    if (actual <= v) return;              // solo baja
    static LONG said = -1;
    if (said != v) {
        said = v;
        log_num("4168: la marca baja de ", (unsigned)actual);
        log_num("  a la cuenta viva ", (unsigned)v);
    }
    *field = v;
}

// A que apunta la llamada virtual que corta el bucle de generacion.
//
// En 0x3de16 el bucle hace `call [rax+0x40]` sobre r14 y, si devuelve falso,
// corta -- sin mirar nuestro bound. Es lo unico encontrado que puede detener la
// generacion en 4 con TODOS los contadores diciendo 5.
//
// r14 sale del mismo contexto que ya sabemos ubicar (el global de 0x8f1e8,
// que es el r15 de 0x3d930), elegido entre tres candidatos:
//
//     0x3da91  cmp  [r15+0x45a8], 1
//     0x3da99  lea  r14, [r15+0x20]
//     0x3da9f  mov  r14, [r15+0x18]
//     0x3dab2  mov  r14, [r15+0x20]
//     0x3dab8  mov  r14, [r15+0x38]
//
// Resolviendo su vtable desde aca sale el RVA de la funcion, y con el RVA se
// desensambla y se ve que recurso niega. Todo lectura: no se engancha nada en
// la ruta de render, que es lo que rompe el renderizado.
// Nombre y offset del modulo dueño de una direccion.
static void say_module(const char *tag, const void *fn) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)fn, &m) || m == nullptr) {
        log_line(tag);
        log_line("    no pertenece a ningun modulo cargado");
        return;
    }
    wchar_t name_w[MAX_PATH];
    GetModuleFileNameW(m, name_w, MAX_PATH);
    char a[260]; int k = 0;
    for (int q = 0; name_w[q] != 0 && k < 250; ++q)
        a[k++] = (char)(name_w[q] < 128 ? name_w[q] : '?');
    a[k] = 0;
    log_line(tag);
    log_line(a);
    log_num("    offset 0x", (unsigned long long)((ULONG_PTR)fn - (ULONG_PTR)m));
}

// A que apunta la llamada virtual que corta el bucle de generacion.
//
// En 0x3de16 el bucle hace `call [rax+0x40]` sobre r14 y, si devuelve falso,
// corta -- sin mirar nuestro bound.
//
// r14 sale del contexto del plugin, y hay DOS bloques que lo eligen:
//
//     0x3da91  cmp [r15+0x45a8], 1
//     0x3da99  lea r14, [r15+0x20]     ; si es 1
//     0x3da9f  mov r14, [r15+0x18]     ; si no
//     0x3daa3  mov r14, [r14]          ; y en los dos, un deref mas
//
//     0x3daa8  cmp [r15+0x45e1], 0
//     0x3dab2  mov r14, [r15+0x20]     ; si NO es cero
//     0x3dab8  mov r14, [r15+0x38]     ; si es cero
//
// La primera version de esta sonda resolvio solo el primer bloque y dio una
// funcion de sl.common que DEVUELVE UN PUNTERO (getAllocator, un pool de
// command allocators de d3d12 con dos entradas). El llamador hace `test al,al`,
// o sea espera un bool: no cierra. Asi que ese no era el objeto, o no siempre.
//
// Se imprimen los tres candidatos con su modulo. Adivinar cual es ya fallo una
// vez.
static void probe_vtable(void) {
    if (g_dlssg_base == nullptr) return;
    static bool said = false;
    if (said) return;
    unsigned char *ctx = nullptr;
    if (!read_ok(g_dlssg_base + 0x8f1e8, &ctx, sizeof(ctx)) || ctx == nullptr) return;
    said = true;
    log_line("vtable: resolviendo el corte del bucle de generacion");
    LONG sel1 = -1; unsigned char sel2 = 0xFF;
    read_ok(ctx + 0x45a8, &sel1, 4);
    read_ok(ctx + 0x45e1, &sel2, 1);
    log_num("  [ctx+0x45a8] ", (unsigned)sel1);
    log_num("  [ctx+0x45e1] ", (unsigned)sel2);

    struct Cand { const char *name_w; unsigned off; bool doble; };
    const Cand cands[3] = {
        { "  candidato A: **(ctx+0x18)", 0x18, true  },
        { "  candidato B:  *(ctx+0x20)", 0x20, false },
        { "  candidato C:  *(ctx+0x38)", 0x38, false },
    };
    for (int c = 0; c < 3; ++c) {
        unsigned char *obj = nullptr;
        if (!read_ok(ctx + cands[c].off, &obj, sizeof(obj)) || obj == nullptr) {
            log_line(cands[c].name_w); log_line("    nulo"); continue;
        }
        if (cands[c].doble) {
            unsigned char *o2 = nullptr;
            if (!read_ok(obj, &o2, sizeof(o2)) || o2 == nullptr) {
                log_line(cands[c].name_w); log_line("    segundo deref nulo"); continue;
            }
            obj = o2;
        }
        unsigned char *vt = nullptr, *fn = nullptr;
        if (!read_ok(obj, &vt, sizeof(vt)) || vt == nullptr) {
            log_line(cands[c].name_w); log_line("    vtable ilegible"); continue;
        }
        if (!read_ok(vt + 0x40, &fn, sizeof(fn)) || fn == nullptr) {
            log_line(cands[c].name_w); log_line("    slot +0x40 ilegible"); continue;
        }
        say_module(cands[c].name_w, fn);
    }
}
// El techo que el plugin declara para si mismo, leido (no escrito).
//
// El runtime de NVIDIA lo dijo por escrito con logLevel en verbose:
//
//   [slSetData] Input data numFramesToGenerate (4) greater than DLSS-G
//   supported numFramesToGenerateMax (3). Set input data count lower.
//   [evaluateNGXFeature] [sl.dlss_g] NGX evaluate feature failed 0xbad00005
//
// Pedir mas de 3 generados no genera de menos: la llamada se RECHAZA entera y
// la evaluacion NGX falla. Las ranuras de esos sub-frames no las llena nadie y
// nuestro bound forzado igual las recorre -> [nulo+0x40] en +0x3ED6F.
//
// El campo es [ctx+0x45e4]:
//
//     0x4fccd  mov  dword ptr [rdi + 0x45e4], 5     ; el plugin lo inicia en 5
//     0x56b5b  mov  r9d, dword ptr [r15 + 0x45e4]   ; y compara contra el
//     0x56b62  cmp  r8d, r9d
//     0x56b65  jbe  sigue
//
// SUBIRLO NO SIRVE, y esta medido: con el campo en 5 los rechazos de slSetData
// desaparecen (0) pero NGX falla 7749 veces con 0xbad00005 y el multiplicador
// entregado cae a mediana 1.00. Se cambia un crash por generacion apagada, que
// es peor. El 3 no es una opinion del plugin: es lo que NGX soporta.
//
// Asi que se LEE. No es un tope nuestro ni un numero inventado: es la capacidad
// que el dispositivo declara. Si un driver o una GPU futura declaran 5, se usan
// 5 sin tocar nada.
// g_max_declarado se define arriba, junto a tope_cuenta.

static void read_max_generated(void) {
    if (g_dlssg_base == nullptr) return;
    unsigned char *ctx = nullptr;
    if (!read_ok(g_dlssg_base + 0x8f1e8, &ctx, sizeof(ctx)) || ctx == nullptr) return;
    LONG v = 0;
    if (!read_ok(ctx + 0x45e4, &v, 4)) return;
    // Rango de cordura: si ahi no hay un numero chico, el offset no es el que
    // creemos y hacerle caso seria peor que ignorarlo.
    if (v < 1 || v > 8) return;
    // CADA cambio, con marca de tiempo. El plugin inicializa el campo en 5:
    //
    //     0x4fccd  mov dword ptr [rdi + 0x45e4], 5
    //
    // y en runtime se lee 3. Ese es el UNICO escritor que aparece en un escaneo
    // estatico de sl.dlss_g, asi que algo de afuera lo baja -- casi seguro una
    // consulta de capacidad de NGX escribiendo por puntero, que el escaneo no
    // ve. Saber CUANDO cambia dice quien: se contrasta el instante contra el
    // sl.log verbose y se ve que llamada ocurrio justo antes.
    //
    // Importa: si el 3 lo pone una consulta de capacidad de la GPU, es el techo
    // de Ada y no hay nada que subir. Si lo pone una tabla o una comprobacion
    // de version, si lo hay.
    if (g_max_declared != v) {
        log_num("max: numFramesToGenerateMax CAMBIO a ", (unsigned)v);
        // Los bytes parcheados, tal como estan AHORA en memoria.
        for (int q = 0; q < g_six_n; ++q) {
            if (g_six_sites[q] == nullptr) continue;
            log_num("  sitio del tope, byte vivo ", (unsigned)*g_six_sites[q]);
        }
        log_num("  venia de ", (unsigned)g_max_declared);
        g_max_declared = v;
    }
}

static LONG probe_4168(void) {
    if (g_dlssg_base == nullptr) return -1;
    const unsigned char *pp = nullptr;
    if (!read_ok(g_dlssg_base + 0x8f1e8, &pp, sizeof(pp)) || pp == nullptr)
        return -1;
    LONG v = -1;
    if (!read_ok(pp + 0x4168, &v, 4)) return -1;
    return v;
}

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

// force_into, options_for_call, hk_slDLSSGSetOptions y apply_override_now
// viven en writer.h, incluido mas abajo; el hook se usa antes de eso.
static unsigned hk_slDLSSGSetOptions(const void *viewport, const void *options);

#include "reflex.h"

static unsigned hk_slGetFeatureFunction(unsigned feature, const char *name, void *&fn) {
    const unsigned r = g_orig_getfeaturefn(feature, name, fn);
    // El interposer acaba de resolver una funcion de DLSS-G: ese puntero cae
    // dentro de UNA de las copias mapeadas, y esa es la que corre. Es un hecho
    // observado, no una deduccion por orden de carga -- que es justo lo que
    // fallaba: g_dlssg_base se asigna a la ULTIMA copia mapeada, no a la viva.
    // slDLSSG, no slDL. El filtro corto delataba a la copia equivocada: matchea
    // tambien slDLSS* -- la super resolucion, que vive en sl.dlss.dll -- y en
    // GTA V se engancho a slDLSSGetOptimalSettings, cuyo puntero no cae en
    // ninguna copia de sl.dlss_g porque es de otro modulo. El log lo dijo solo:
    // "el puntero NO cae en ninguna de las copias registradas".
    // Nombres EXACTOS, no un prefijo.
    //
    // "slDLSSG" tampoco alcanza: slDLSSGetOptimalSettings es slDLSS +
    // GetOptimalSettings, asi que comparte los primeros siete caracteres con
    // slDLSSG*. Esa funcion es de la super resolucion y vive en sl.dlss.dll, asi
    // que su puntero jamas cae en una copia de sl.dlss_g -- y con el filtro por
    // prefijo la Capa 0 se enganchaba a ella y concluia que no sabia cual copia
    // ejecuta. Peor: la fase creia el dato y ponia el mod en PASIVO en GTA V,
    // que andaba. Un PASIVO basado en un instrumento roto es peor que no tener
    // PASIVO.
    if (r == 0 && name != nullptr && fn != nullptr &&
        (strcmp(name, "slDLSSGSetOptions") == 0 ||
         strcmp(name, "slDLSSGGetState") == 0))
        executing_copy(fn, name);
    if (r == 0 && name != nullptr && fn != nullptr &&
        strcmp(name, "slDLSSGGetState") == 0 && g_orig_getstate == nullptr) {
        g_orig_getstate = (PFN_slDLSSGGetState)fn;
        log_line("state: slDLSSGGetState captured");
    }
    if (r == 0 && name != nullptr && fn != nullptr &&
        strcmp(name, "slReflexGetState") == 0 &&
        fn != (void *)&hk_slReflexGetState) {
        g_orig_reflexstate = (PFN_slReflexGetState)fn;
        fn = (void *)&hk_slReflexGetState;
        log_line("reflex: slReflexGetState wrapped (latency probe)");
    }
    if (r == 0 && name != nullptr && fn != nullptr && g_marker_every > 0.0) {
        if (strcmp(name, "slPCLSetMarker") == 0 && fn != (void *)&hk_slPCLSetMarker) {
            g_orig_pclmarker = (PFN_slSetMarker)fn;
            fn = (void *)&hk_slPCLSetMarker;
            log_line("bench: PCL markers wrapped (gap emulation armed)");
        } else if (strcmp(name, "slReflexSetMarker") == 0 &&
                   fn != (void *)&hk_slReflexSetMarker) {
            g_orig_reflexmarker = (PFN_slSetMarker)fn;
            fn = (void *)&hk_slReflexSetMarker;
            log_line("bench: Reflex markers wrapped (gap emulation armed)");
        }
    }
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
            // The selection is left alone: DYNAMIC is ours and works on a
            // plugin that has never heard of eDynamic. g_dynamic_known now
            // only records what the plugin itself can do.
        }
        fn = (void *)&hk_slDLSSGSetOptions;
        log_line("multiplier override armed (slDLSSGSetOptions wrapped)");
    }
    return r;
}

static void apply_override_now(void);

static void query_state(void);


#include "patches.h"




#include "measurement.h"

// How many generated frames it would take to reach the target from the rate
// the game is actually rendering at, with the ceiling the plugin gave us.
#include "writer.h"

static void settings_load(void);
static void settings_watch(void) {
    if (!g_watch_settings) return;
    static int n = 0;
    // Every 20 rendered frames. The measurement window is 45 frames, so the
    // poll has to be several times finer than that or the answer is the poll
    // interval rather than the response. At 120 it was 1.0 to 2.7 windows --
    // coarser than the thing being measured.
    if (++n < 20) return;
    n = 0;
    settings_load();
}

#include "frametoken.h"


static void settings_save(void) {
    wchar_t p[MAX_PATH];
    beside_dll(p, L"mfg-settings.txt");
    HANDLE h = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[160];   // cuatro lineas ahora: mode, target, dynfps, hud
    int k = 0;
    const char *l1 = "mode ";
    for (int i = 0; l1[i] != 0; ++i) buf[k++] = l1[i];
    {
        // Written as a number rather than a single digit: the row count has
        // already grown once, and a mode of 10 would otherwise be saved as 0,
        // which is AUTO -- a silent reset rather than an error.
        int v = (int)g_force_sel, n = 0, d[3];
        if (v <= 0) buf[k++] = '0';
        else {
            while (v > 0 && n < 3) { d[n++] = v % 10; v /= 10; }
            while (n > 0) buf[k++] = (char)('0' + d[--n]);
        }
    }
    buf[k++] = '\r'; buf[k++] = '\n';
    const char *l2 = "target ";
    for (int i = 0; l2[i] != 0; ++i) buf[k++] = l2[i];
    {
        int v = (int)g_dyn_target, n = 0, d[5];
        if (v <= 0) { buf[k++] = '0'; }
        else {
            while (v > 0 && n < 5) { d[n++] = v % 10; v /= 10; }
            while (n > 0) buf[k++] = (char)('0' + d[--n]);
        }
    }
    buf[k++] = '\r'; buf[k++] = '\n';
    {
        const char *l4 = "dynfps ";
        for (int i = 0; l4[i] != 0; ++i) buf[k++] = l4[i];
        int v = (int)g_dyn_fps, n = 0, d[5];
        if (v <= 0) buf[k++] = '0';
        else {
            while (v > 0 && n < 5) { d[n++] = v % 10; v /= 10; }
            while (n > 0) buf[k++] = (char)('0' + d[--n]);
        }
        buf[k++] = '\r'; buf[k++] = '\n';
    }
    {
        // El HUD se guarda como el resto: si alguien lo deja prendido,
        // sigue prendido la proxima vez que abre el juego.
        const char *l3 = "hud ";
        for (int i = 0; l3[i] != 0; ++i) buf[k++] = l3[i];
        buf[k++] = g_hud_on ? '1' : '0';
        buf[k++] = '\r'; buf[k++] = '\n';
    }
    DWORD w = 0;
    WriteFile(h, buf, (DWORD)k, &w, nullptr);
    CloseHandle(h);
}

static void settings_load(void) {
    wchar_t p[MAX_PATH];
    beside_dll(p, L"mfg-settings.txt");
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[256];
    DWORD n = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(h);
    if (!ok || n == 0) return;
    buf[n] = 0;
    // El parser vive en src/config.h; lo que no reconoce lo deja en -1 y aca
    // no se toca: una escritura truncada no puede volverse "AUTO, objetivo
    // nada". La semilla de la cuenta y el piso del objetivo son politica y
    // quedan aca.
    config::Settings s;
    config::parse_settings(buf, n, kPanRows, kMaxCustom, s);
    if (s.dynfps >= 0) g_dyn_fps = s.dynfps;
    if (s.hud >= 0) g_hud_on = s.hud == 1;
    if (s.mode >= 0) {
        const int v = s.mode;
        g_force_sel = v;
        // La cuenta ES el multiplicador, no los generados. Medido en Halo:
        // cuenta 3 entrega 3.00x y cuenta 5 entrega 5.0x.
        g_force_generated = (v >= 2 && v <= kSelMaxFixed)
                                ? (count_is_multiplier() ? v : v - 1) : 0;
        // La semilla de DYNAMIC es 2, no 1: con la semantica nueva 1 es 1X,
        // generacion encendida produciendo nada, y sin generacion no hay base
        // que medir -- el controlador se espera a si mismo. Con la vieja, 2
        // es 3X por una ventana, que se corrige solo. Ver
        // [[dynamic-se-espera-a-si-mismo]].
        if (v == kSelDynamic || v == kSelDynFuture)
            g_force_generated = 2;
    }
    if (s.target >= 0) {
        // Un ajuste guardado por una version anterior puede traer 150. Se
        // sube al piso en vez de aceptarlo: el panel ya no ofrece ese valor.
        // Con mfg-sub2.txt el piso baja a 110, que es lo mas chico que el
        // scheduler puede expresar; es para medir con el banco.
        const int floor_v = g_sub2 ? 110 : kMinCustom;
        g_dyn_target = s.target < floor_v ? floor_v : s.target;
    }
    // Only when something moved, so re-reading twice a second under
    // mfg-watch.txt leaves one marked line per change instead of two per
    // interval -- and that line is what the latency is counted from.
    {
        static LONG seen_sel = -1;
        static int  seen_tgt = -1;
        if (g_force_sel != seen_sel || g_dyn_target != seen_tgt) {
            seen_sel = g_force_sel;
            seen_tgt = g_dyn_target;
            log_num("settings: restored mode ", (unsigned)g_force_sel);
            log_num("  target fps ", (unsigned)g_dyn_target);
        }
    }
}

// Must run on the thread that presents -- the plugin's own log says so:
// "slDLSSGGetState must be synchronized with the present thread". That is the
// game thread, which is where the frame-token hook already runs.
static void query_state(void) {
    if (g_orig_getstate == nullptr || g_asked_state != 0 || g_opt_have == 0) return;
    if ((LONG)GetCurrentThreadId() != g_opt_thread) {
        static bool said = false;
        if (!said) {
            said = true;
            log_num("state: not asked -- frame tokens come from thread ",
                    (unsigned)GetCurrentThreadId());
            log_num("  but the game configured DLSS-G from thread ",
                    (unsigned)g_opt_thread);
            log_line("  and the plugin requires the present thread for this call");
        }
        return;
    }
    g_asked_state = 1;

    // Generous and zeroed: the plugin writes as much of this as its own
    // version defines, and a buffer sized to our reading of the header would
    // be a guess about how much that is.
    unsigned char st[256];
    for (int i = 0; i < 256; ++i) st[i] = 0;
    for (int i = 0; i < 16; ++i) st[8 + i] = kDlssgStateGuid[i];
    *(unsigned long long *)(st + 24) = 4;          // kStructVersion4

    const unsigned r = g_orig_getstate(g_vp_copy, st, g_opt_copy);
    if (r != 0) {
        log_num("state: slDLSSGGetState refused, code ", (unsigned)r);
        return;
    }
    const unsigned mx = *(unsigned *)(st + 52);
    const unsigned char dyn = st[80];
    log_num("state: the plugin accepts up to N generated frames: ", mx);
    log_num("  which is a maximum multiplier of ", mx + 1);
    log_num("  dynamic MFG supported (1 = yes): ", (unsigned)dyn);
    // Sanity before it is allowed to shape the interface: a plugin claiming
    // hundreds is a layout mistake on our side, not a discovery.
    if (mx >= 1 && mx <= 16) {
        g_frames_max = (LONG)mx;
    } else {
        log_line("  that is out of range, so it is ignored and the rows stay as they were");
    }
}


static void arm_slinit_early(void) {
    if (g_orig_slinit != nullptr) return;
    HMODULE si = GetModuleHandleW(L"sl.interposer.dll");
    if (si == nullptr) return;
    void *fi = (void *)GetProcAddress(si, "slInit");
    if (fi == nullptr) { log_line("  ! slInit no esta exportado aca"); return; }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        log_num("  ! MinHook no arranco para slInit, codigo ", (unsigned)init);
        return;
    }
    if (MH_CreateHook(fi, (void *)&hk_slInit, (void **)&g_orig_slinit) != MH_OK ||
        MH_EnableHook(fi) != MH_OK) {
        log_line("  ! no se pudo enganchar slInit");
        g_orig_slinit = nullptr;
        return;
    }
    log_line("slInit enganchado");
}




#include "recorder.h"

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

static wchar_t lower(wchar_t c) { return (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c; }

static bool name_is(const UNICODE_STRING *s, const wchar_t *want) {
    if (s == nullptr || s->Buffer == nullptr) return false;
    const size_t n = s->Length / sizeof(wchar_t);
    size_t i = 0;
    for (; i < n && want[i] != 0; ++i)
        if (lower(s->Buffer[i]) != lower(want[i])) return false;
    return i == n && want[i] == 0;
}

#include "loader.h"
// ------------------------------------------------------------ forwarding ---
//
// Resolved lazily rather than in DllMain: calling LoadLibrary under the loader
// lock is how proxies deadlock.

static HMODULE g_real = nullptr;
// El nombre con el que nos cargaron. El mismo dll sirve como version.dll o
// como winmm.dll: no todos los juegos importan version.dll (Metro Exodus EE
// importa winmm.dll y carga dxgi/d3d12 a mano), y el forwarding tiene que ir
// al dll de sistema que suplantamos. Lo llena DllMain.
static wchar_t g_own_name[64] = L"version.dll";

static FARPROC real(const char *name) {
    if (g_real == nullptr) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 80) return nullptr;
        path[n++] = L'\\';
        for (UINT i = 0; g_own_name[i] != 0 && n < MAX_PATH - 1; ++i) path[n++] = g_own_name[i];
        path[n] = 0;
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

// winmm.dll: los 180 exports del de sistema, como stubs en ensamblador
// (src/forwards_winmm.S, generado por tools/gen_forwards.py) que resuelven
// la direccion real la primera vez que alguien los llama. Cuatro forwarders a
// mano no alcanzaban: cualquier modulo que haga LoadLibrary("winmm.dll") nos
// recibe a nosotros, y un GetProcAddress que falle tira el proceso.
#include "forwards_winmm_names.h"
static HMODULE g_real_winmm = nullptr;
static FARPROC g_winmm_cache[kWinmmNamesN];
extern "C" FARPROC forward_resolve(int idx) {
    if (idx < 0 || idx >= kWinmmNamesN) return nullptr;
    if (g_winmm_cache[idx] != nullptr) return g_winmm_cache[idx];
    if (g_real_winmm == nullptr) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 16) return nullptr;
        const wchar_t *tail = L"\\winmm.dll";
        for (UINT i = 0; tail[i] != 0; ++i) path[n + i] = tail[i];
        path[n + 10] = 0;
        g_real_winmm = LoadLibraryW(path);
        if (g_real_winmm == nullptr) return nullptr;
    }
    g_winmm_cache[idx] = GetProcAddress(g_real_winmm, kWinmmNames[idx]);
    return g_winmm_cache[idx];
}

// ---------------------------------------------------------------- attach ---


#include "exceptions.h"

// Lo que DllMain hacia en un solo cuerpo de 288 lineas, en cuatro pasos con
// nombre. Los cuerpos son los mismos; solo cambia donde estan.

#include "config_apply.h"

// El reloj, el buffer de muestras y el hilo del grabador/panel.
static void start_recorder_thread(void) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        g_qpc_freq = f.QuadPart ? f.QuadPart : 1;
        g_samples = static_cast<Sample *>(VirtualAlloc(nullptr, sizeof(Sample) * kMaxSamples,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        HANDLE th = CreateThread(nullptr, 0, &recorder, nullptr, 0, nullptr);
        if (th != nullptr) CloseHandle(th);
}

// Las variables de entorno que Streamline y NGX leen al arrancar.
static void init_streamline_env(HMODULE self) {
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
        if (g_cfg.sllog) {
            wchar_t dir[MAX_PATH];
            DWORD m = GetModuleFileNameW(self, dir, MAX_PATH);
            while (m > 0 && dir[m - 1] != L'\\') --m;
            if (m > 1) { dir[m - 1] = 0; SetEnvironmentVariableW(L"SL_LOG_PATH", dir); }
            SetEnvironmentVariableW(L"SL_LOG_LEVEL", L"2");
            SetEnvironmentVariableW(L"SL_ENABLE_CONSOLE_LOGGING", L"0");
            g_sllog_on = true;
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
    {
        wchar_t p[MAX_PATH];
        int j = 0;
        while (g_log[j] != 0 && j < MAX_PATH - 1) { p[j] = g_log[j]; ++j; }
        while (j > 0 && p[j - 1] != 0x5C) --j;
        if (g_cfg.indicator) {
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
}

// La notificacion de carga de dlls, y el snippet si ya estaba cargado.
static void register_dll_notifications(void) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto reg = reinterpret_cast<PFN_LdrRegister>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    if (reg == nullptr) {
        log_line("LdrRegisterDllNotification is unavailable; nothing will be patched");
        return;                 // era `return TRUE` de DllMain: mismo efecto
    }
    PVOID cookie = nullptr;
    if (reg(0, &on_dll_load, nullptr, &cookie) == 0) {
        log_line("armed, waiting for nvngx_dlssg.dll");
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
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);
    {
        // Con que nombre nos cargaron (version.dll o winmm.dll): el forwarding
        // va al dll de sistema del mismo nombre.
        wchar_t me[MAX_PATH];
        DWORD m = GetModuleFileNameW(self, me, MAX_PATH);
        while (m > 0 && me[m - 1] != L'\\') --m;
        int k = 0;
        for (; me[m + k] != 0 && k < 63; ++k)
            g_own_name[k] = (me[m + k] >= L'A' && me[m + k] <= L'Z') ? (wchar_t)(me[m + k] + 32) : me[m + k];
        g_own_name[k] = 0;
    }

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
        apply_config();
        start_recorder_thread();
    }
    AddVectoredExceptionHandler(1, exception_witness);
    log_line("--- mfg-unlock attached ---  (F9 records)");

    init_streamline_env(self);

    // Antes de que nadie cargue plugins: si el sl.dlss_g del juego no tiene el
    // sitio de la cuenta, se carga uno de la cache que si lo tenga.
    arm_plugin_redirect();

    register_dll_notifications();
    return TRUE;
}
