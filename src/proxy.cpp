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

// The panel's own state, defined here and shared with overlay.h. It is a
// window of ours now, not something drawn into the game's frame -- see the
// note at the top of that file for what the three rendering attempts before
// it cost.
volatile LONG g_force_sel = 0;
volatile LONG g_force_generated = 0;
volatile LONG g_last_seen_generated = 0;
volatile LONG g_dyn_target = 0;
// DYNAMIC: el objetivo esta en fps presentados, no en ratio. Cero significa
// el refresh del monitor. El controlador escribe g_dyn_target -- el ratio --
// y de ahi para abajo todo es el planificador fraccionario que ya andaba.
volatile LONG g_dyn_fps = 0;
static LONG g_refresh_hz = 0;
// Sonda de una sola vez: cuantos frames pasan entre escribir un ratio nuevo y
// que las presentaciones lo reflejen. El objetivo pedia medir esto y no
// suponerlo, y es el unico candidato que queda para el sobrepaso -- todo el
// error residual esta del lado alto, justo despues de que la base sube.
static LONG g_probe_left = 0;
static LONG g_probe_pc0 = 0;
static unsigned char g_probe[16];
static LONG g_probe_i = 0;
static bool g_probe_done = false;
static void dyn_control(double base_fps, double presented_fps);  // definida mas abajo
static void dyn_apply(double base_fps);                          // definida mas abajo
bool g_dynamic_known = false;
bool g_ov_enabled = true;
static void log_line(const char *text);
static void log_num(const char *label, unsigned long long v);

// ------------------------------------------------------------------- log ---
//
// Raw file calls, no CRT: some of this runs under the loader lock.

static wchar_t g_log[MAX_PATH];
static bool g_frac_enabled = false;   // mfg-frac.txt
// Engancho el parche del byte de la cuenta de sub-frames? Es lo que permite que
// la cuenta varie por frame. Cuando NO engancha, la unica palanca que queda es
// la cuenta de la API, y esa dimensiona la reserva del plugin: pedir de mas ahi
// no da un multiplicador mas alto, crashea el juego. Le paso a Halo Campaign
// Evolved, que carga un sl.dlss_g de 447960 bytes en vez del de 625792 que
// sabemos parchear: 'sites: 0' y aun asi pedimos cuenta 5. Tres crashes.
// Arranca en true y solo puede caer: si CUALQUIER copia mapeada falla una de
// las dos piezas, el freno se arma. En Halo hay dos copias -- la de
// Engine\Plugins, donde no engancha nada, y la OTA 134656, donde el contador
// SI engancha -- y una sola bandera pisada por la ultima copia dejaba el freno
// desactivado justo en el juego que lo necesitaba.
//
// Y hacen falta las dos, no solo el contador. En la copia OTA de Halo el
// contador engancho y la bandera de generacion no ('generation flag site not
// unique, sites: 0'). Sin esa bandera, una cuenta de cero hace que el bucle no
// produzca nada mientras el plugin cree que esta generando, y el frame se
// presenta por un camino sin nada que presentar. El dump del crash de Halo cae
// justo dentro de esa copia: 0xC0000005 en 190_E658703.dll +0x3F2E9.
static bool g_wic_ok = true;
static bool g_vio_alguna_copia = false;
static bool g_quiet = false;          // mfg-quiet.txt: no per-window logging
static bool g_nullalt = false;        // mfg-nullalt.txt: alternate between equals
static bool g_slowalt = false;        // mfg-slowalt.txt
static int  g_slowalt_len = 240;       // rendered frames per block
static int  g_block_ms = 0;           // mfg-blockms.txt, 0 = default
static bool g_peralt = false;         // mfg-peralt.txt: diffuse per frame
// mfg-sub2.txt: SOLO para medir. Baja el piso del target por debajo de 200
// en el camino de mfg-settings.txt, para poder medir 1.25x/1.50x/1.75x sin
// bajar el piso del panel, que es justo lo que hay que decidir con esos
// numeros. Sin el archivo, el comportamiento es identico al de antes.
static bool g_sub2 = false;           // mfg-sub2.txt
// mfg-twocopies.txt: SOLO para el banco. Contiene la ruta de una segunda copia
// de sl.dlss_g; se carga a proposito para reproducir lo que hace Cyberpunk, que
// mapea la del juego Y la del cache OTA. g_wic y g_count_imm son punteros unicos
// y la segunda copia parcheada los pisa, asi que las escrituras por frame se van
// a un modulo que no genera. El sample no lo hace solo: Streamline avisa
// "eLoadDownloadedPlugins flag not passed to preferences", y esa bandera la pasa
// la aplicacion en slInit, no un archivo de configuracion.
static bool g_twocopies = false;      // mfg-twocopies.txt
// mfg-ceilfirst.txt: A1. Mientras la interpolacion todavia no arranco,
// declararle al plugin el TECHO del ciclo (lo+1) en vez de la cuenta de este
// frame. La reserva del plugin se hace al encender: si en ese momento ve el
// maximo que vamos a usar, la alternancia posterior queda siempre por debajo.
//
// MEDIDO Y SIN EFECTO. Cyberpunk con -benchmark y la ventana al frente, 226
// ventanas de juego cada una: con A1 entrega 3.48x (p10 3.00, p90 4.33), sin A1
// 3.49x (p10 3.02, p90 4.31), pidiendo 3.50 las dos. Son la misma corrida.
//
// Se queda porque documenta una palanca probada, no porque sirva. Ojo con dos
// cosas si se retoma: declaro techo 3 y no 4, o sea que capturo el techo antes
// de que el objetivo se asentara; y el 3.50 sale entero SIN la palanca, asi que
// no habia techo que levantar -- lo que tapaba el resultado era la ventana sin
// foco (ver dlssg-needs-window-focus en las memorias).
static bool g_ceilfirst = false;      // mfg-ceilfirst.txt
static volatile LONG g_ciclo_techo = 0;   // lo+1 del ciclo en curso
// Encendida o no. No hay consulta directa que sirva: slDLSSGGetState avisa que
// hay que sincronizarla con el hilo de present. Se deduce de que lo presentado
// supere a la base de Reflex, que es la medida honesta que ya tenemos.
static volatile LONG g_interp_on = 0;
static volatile LONG g_twocopies_pending = 0;
// mfg-optsv3.txt: SOLO para el banco. GTA V llena DLSSGOptions con
// structVersion 3 y el sample con 5, y eso se leyo de los logs de los dos.
// En v3 no existe numFramesToGenerate, asi que la cuenta que force_into
// escribe en p+36 la ignora el plugin: el sl.log del juego reporta
// numFramesToGenerate=1 en las seis transiciones mientras el planificador
// escribia 2. Con esto el sample manda v3 y el banco recorre el mismo camino.
// Apagada por defecto; no cambia nada de lo que la dll hace en un juego.
static bool g_optsv3 = false;
// mfg-markergap.txt: SOLO para el banco. Contiene dos numeros, "cada" y
// "cuanto", en segundos: cada N segundos deja de reenviar los marcadores de
// Reflex/PCL durante M segundos. Es lo que GTA V hace solo -- su sl.log
// muestra el id de frame de Reflex congelado en 13307 mientras el actual
// llegaba a 23110, y DLSS-G se niega con
// eDLSSGStatusFailReflexNotDetectedAtRuntime durante 72 s seguidos.
// El sample nunca corta ese flujo, y por eso el banco no podia apagarse.
// Apagada por defecto; en un juego la dll no hace nada de esto.
typedef unsigned (*PFN_slSetMarker)(unsigned, void *);
static PFN_slSetMarker g_orig_pclmarker = nullptr;
static PFN_slSetMarker g_orig_reflexmarker = nullptr;
static LONG g_markers_dropped = 0;
static double g_marker_every = 0.0;
static double g_marker_for = 0.0;
static double g_marker_long_every = 0.0;
static double g_marker_long_for = 0.0;
static bool g_blockalt = false;       // mfg-blockalt.txt: blocks above 2.0x too
static int  g_blocks = 0;             // mfg-blocks.txt, 0 = 32

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
static void log_line(const char *text) {
    if (g_log[0] == 0) return;
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

static LONG g_saved_mode = 0;
static bool g_override_said = false;

static const unsigned char kDlssgStateGuid[16] = {
    0xe1, 0xc8, 0x8a, 0xcc,             // 0xcc8ac8e1, little endian
    0x79, 0xa1,                         // 0xa179
    0xf5, 0x44,                         // 0x44f5
    0x97, 0xfa, 0xe7, 0x41, 0x12, 0xf9, 0xbc, 0x61
};

typedef unsigned (*PFN_slDLSSGGetState)(const void *, void *, const void *);
static PFN_slDLSSGGetState g_orig_getstate = nullptr;

// What the plugin says it will accept. Zero means "not asked yet"; the panel
// offers nothing above this once it is known.
volatile LONG g_frames_max = 0;      // shared with overlay.h
static volatile LONG g_asked_state = 0;

typedef unsigned (*PFN_slDLSSGSetOptions)(const void *, const void *);
typedef unsigned (*PFN_slGetFeatureFunction)(unsigned, const char *, void *&);
static PFN_slDLSSGSetOptions g_orig_setoptions = nullptr;
// Restauracion diferida de las opciones del juego: ver hk_slDLSSGSetOptions.
static unsigned char *g_restore_p = nullptr;
static LONG g_restore_cnt = 0, g_restore_mode = 0;
static bool g_restore_pending = false;
static PFN_slGetFeatureFunction g_orig_getfeaturefn = nullptr;
// slInit: por aca pasa la aplicacion sus preferencias, y ahi vive la bandera
// que decide si Streamline puede cargar plugins descargados por OTA.
//
//   eAllowOTA            = 1 << 3
//   eLoadDownloadedPlugins = 1 << 6
//
// Cyberpunk las pasa y por eso carga el plugin de la cache; Halo no, y se queda
// con el suyo de 447960 bytes, que no tiene el sitio del contador. La cache de
// esta maquina tiene tres builds que SI lo tienen (133888, 134273, 134656).
//
// El offset de `flags` sale de sl_core_types.h: BaseStructure son 32 bytes
// (next 8 + GUID 16 + structVersion 8) y despues showConsole, logLevel,
// pathsToPlugins, numPathsToPlugins, pathToLogsAndData y tres callbacks dan 88.
// Se verifica antes de escribir: mfg-ota.txt no modifica nada hasta que el
// volcado confirme que ahi hay una mascara con sentido.
typedef unsigned (*PFN_slInit)(void *, unsigned long long);
static PFN_slInit g_orig_slinit = nullptr;
static bool g_ota = false;              // mfg-ota.txt
// Probado en el sample del banco, que como Halo no pedia OTA: banderas 133 ->
// 205, de un sl.dlss_g mapeado se pasa a tres -- uno de ellos el 134656 de
// ProgramData, donde el parche del contador SI engancha -- y el fraccionario
// sigue entregando lo pedido: 3.50 pedido, 3.50x entregado (p10 3.44, p90
// 3.57) contra 3.51x sin OTA. Con tres copias mapeadas no se pierde nada.
static const int kPrefFlags = 88;

// Engancha SOLO slInit, para poder correrlo temprano.
//
// El armado general vive en el hilo del panel y llega tarde: en el sample del
// banco slInit ya se llamo cuando ese hilo despierta, asi que el volcado nunca
// aparecio. En Cyberpunk si llega porque ahi el interposer carga mas tarde.
//
// Corre en DllMain solo si existe mfg-ota.txt. Inicializar MinHook bajo el
// bloqueo del cargador es un riesgo conocido, asi que se paga unicamente
// cuando alguien pidio el forzado; sin la bandera, nada de esto pasa y los
// juegos que ya andan no cambian.
static void arm_slinit_temprano(void);

static unsigned hk_slInit(void *pref, unsigned long long sdk) {
    if (pref != nullptr) {
        unsigned char *p = (unsigned char *)pref;
        log_num("slInit: structVersion ", (unsigned)*(unsigned long long *)(p + 24));
        const unsigned long long f = *(unsigned long long *)(p + kPrefFlags);
        log_num("  banderas en +88 ", (unsigned)f);
        log_num("    eAllowOTA (bit 3) ", (unsigned)((f >> 3) & 1));
        log_num("    eLoadDownloadedPlugins (bit 6) ", (unsigned)((f >> 6) & 1));
        // Volcado crudo para poder ubicar el campo si el offset no fuera 88.
        for (int fila = 0; fila < 9; ++fila) {
            char buf[64];
            const unsigned long long v = *(unsigned long long *)(p + fila * 8);
            wsprintfA(buf, "    +%d = %08X%08X", fila * 8,
                      (unsigned)(v >> 32), (unsigned)v);
            log_line(buf);
        }
        if (g_ota) {
            DWORD viejo = 0;
            if (VirtualProtect(p + kPrefFlags, 8, PAGE_READWRITE, &viejo)) {
                *(unsigned long long *)(p + kPrefFlags) = f | (1ull << 3) | (1ull << 6);
                VirtualProtect(p + kPrefFlags, 8, viejo, &viejo);
                log_num("  OTA forzado, banderas ahora ",
                        (unsigned)*(unsigned long long *)(p + kPrefFlags));
            } else {
                log_line("  ! no se pudo escribir las banderas");
            }
        }
    }
    return g_orig_slinit(pref, sdk);
}

// Enough of the last call to make it again ourselves. The header says
// slDLSSGSetOptions is not thread safe, so the thread the game used is
// recorded with it and the replay only happens on that same thread -- from
// the present hook, which the game enters every frame.
static unsigned char g_opt_copy[256];
static unsigned char g_vp_copy[64];
static volatile LONG g_opt_have = 0;
static volatile LONG g_opt_thread = 0;
static volatile LONG g_opt_pending = 0;
// Set when the game itself configures DLSS-G, cleared when the next frame
// begins. While it is set, our replay stays out of the way.
//
// The plugin names this failure in its own log, once per frame:
//   Repeated slDLSSGSetOptions() call for the frame 9411. A redundant call
//   or a race condition with Present().
// Our replay fired on every frame token whether or not the game had just set
// the options for that same frame -- two calls for one frame. That is what
// collapsed 54 fps to 22, not the zero count and not which path the frame
// took through presentCommon.
static volatile LONG g_game_set_this_frame = 0;

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
    } else if (sel == kSelDynamic || sel == kSelDynFuture) {
        // Ours, not the plugin's: eOn with the count dynamic_tick chose. The
        // struct rebuild to version 5 and the eDynamic write below are gone
        // with it, so nothing here depends on the plugin being 2.11.1.
        //
        // Zero generated frames is eOff, not eOn with a count of zero -- the
        // plugin rejects that outright ("Input data numFramesToGenerate must
        // be greater than 0") and keeps whatever it had, which is why setting
        // a target of 30 against a base of 41 computed the right answer and
        // then changed nothing at all.
        // eOff for a frame that generates nothing, eOn otherwise.
        //
        // This was tried and abandoned once on the strength of a transition
        // count -- 442 against ten at 2.5x -- without ever measuring the frame
        // rate it produced. What the disassembly since then shows is that the
        // "not generating" branch (ctx+0x45da == 0, tested at 0x180046f15) is
        // the ordinary present path, the one that runs with generation off.
        // eOn with a count of zero takes the *generation* path instead and
        // finds nothing to present, which is the collapse to 30 fps: the
        // render loop spins at 128 while the screen gets 30.
        //
        // So the transitions may well be the cheaper of the two. That is a
        // measurement, not a conclusion, and it has not been made yet.
        // El techo, no la cuenta de este frame.
        //
        // Escribir la cuenta por frame aca -- y alternar el modo entre eOff y
        // eOn -- hacia 1307 llamadas a slDLSSGSetOptions en 45 segundos, una
        // por frame. Y cada llamada que cambia la cuenta hace que el plugin
        // libere sus recursos y arranque un enfriamiento de 100 ms:
        //
        //   0x1800497fd  movabs rax, 0x4059000000000000   ; = 100.0
        //   0x180049807  mov    [rdi+0x4488], rax
        //   0x18004b1b8  mientras [rdi+0x4488] > 0, devuelve "no interpola"
        //
        // Eso es lo que en GTA V aparece como "interpolation state changed from
        // enabled to disabled (mode=eOn, numFramesToGenerate=2)" con nosotros
        // pidiendo generacion, y los frames en vuelo de la vuelta son los tres
        // "Out of order frame - will skip the present".
        //
        // El techo es constante mientras no cambie la seleccion, asi que la
        // llamada se hace una vez y la variacion por frame la lleva el byte
        // parcheado, que es para lo que existe. bound = byte + 1 <= techo + 1,
        // que es la condicion que los crashes anteriores violaban.
        // REVERTIDO. Mandarle el techo constante rompe 2.25x en el juego.
        //
        // La idea era buena y el motivo sigue en pie -- 1307 llamadas por
        // corrida, contra la guia de NVIDIA que dice llamarla en interacciones
        // de UI y no por frame -- pero viola el invariante que este archivo ya
        // documentaba: la cuenta de la API decide la generacion Y dimensiona la
        // reserva, y un bound menor que (cuenta + 1) detiene la presentacion.
        //
        // Con techo 2 a 2.25x, los frames cuyo byte vale 1 hacen que el plugin
        // espere tres presentaciones y reciba dos. Medido jugando: 2.25x pedido
        // entrego 1.56 con la base subida a 96, o sea la generacion apagada
        // buena parte del tiempo. El banco no lo vio porque a 2.50x y 2.75x dio
        // 2.503 y 2.750 -- el defecto aparece cuando la fraccion es baja y el
        // byte pasa mucho tiempo por debajo del techo.
        //
        // Para bajar las llamadas hay que sincronizarlas con el byte, no
        // desacoplarlas de el.
        if (g_force_generated <= 0) {
            *(LONG *)(p + 32) = 0;             // DLSSGMode::eOff
        } else {
            *(LONG *)(p + 32) = 1;             // DLSSGMode::eOn
            LONG escribir = g_force_generated;
            if (g_ceilfirst) {
                const LONG techo = g_ciclo_techo;
                if (!g_interp_on && techo > escribir && techo <= 5) {
                    escribir = techo;
                    static bool dicho = false;
                    if (!dicho) {
                        dicho = true;
                        log_num("A1: declarando el techo del ciclo mientras apagado ",
                                (unsigned)techo);
                    }
                }
            }
            // Freno de seguridad. Sin el parche del contador no podemos repartir
            // la cuenta por frame, asi que pedir por la API mas de lo que el
            // juego pidio no compra nada y arriesga la reserva. Se espeja.
            if (!g_wic_ok) {
                const LONG suyo = g_last_seen_generated;
                if (suyo < 1 || suyo > 5) {
                    // Nunca vimos que cuenta pide el juego -- en Halo no hay una
                    // sola linea de "the game itself last asked for" en toda la
                    // corrida. Sin ese dato no hay a que espejarse, asi que no se
                    // escribe la cuenta: se deja la del juego intacta. Poner un
                    // valor de reserva seria forzar a ciegas, que es exactamente
                    // lo que hizo crashear a ese juego.
                    static bool dicho_nada = false;
                    if (!dicho_nada) {
                        dicho_nada = true;
                        log_line("freno: sin parche y sin saber que pide el juego, "
                                 "no se toca la cuenta");
                    }
                    // El modo tambien vuelve como estaba: unas lineas mas
                    // arriba se escribio eOn, y dejarlo puesto con una cuenta
                    // que no elegimos seria encender la generacion por nuestra
                    // cuenta en un plugin que no sabemos manejar.
                    *(LONG *)(p + 32) = *savedMode;
                    return;
                }
                const LONG tope = suyo;
                if (escribir > tope) {
                    static LONG dicho = -1;
                    if (dicho != tope) {
                        dicho = tope;
                        log_num("freno: sin parche, la cuenta se limita a la del juego ",
                                (unsigned)tope);
                    }
                    escribir = tope;
                }
            }
            *(LONG *)(p + 36) = escribir;
        }
    } else if (false) {
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
// Para clasificar los apagones sin pedirle al usuario que juegue distinto:
// si la ventana no tiene el foco, o el juego dejo de estar en primer plano,
// el apagon es del menu/pausa/alt-tab y no un defecto nuestro.
static HWND g_game_hwnd = nullptr;
// Latencia: sl.reflex la calcula solo. El sample llama a slReflexGetState
// todos los frames (StreamlineSample.cpp:901, sin condicion), asi que envolver
// esa llamada da el struct ya armado por el, con su GUID y su version -- no
// hay que adivinar nada. La cuenta que importa la escribe NVIDIA en su propio
// sample: totalGameToRenderLatency = gpuRenderEndTime - inputSampleTime.
// Paso 1, y por ahora lo unico: volcar los bytes para leer el layout. Nada de
// calcular latencias sobre offsets supuestos.
typedef unsigned (*PFN_slReflexGetState)(void *);
static PFN_slReflexGetState g_orig_reflexstate = nullptr;
static bool g_reflex_dumped = false;
// Sacado del volcado: GUID en +8 y version 2 en +24 de la ReflexState que
// el sample entrego ya armada. Hace falta porque GTA V llama a
// slReflexGetState apenas un punado de veces -- al abrir su menu, se ve --
// y no una vez por frame como el sample, asi que colgarse de sus llamadas
// no da ningun dato. Consultamos nosotros.
static const unsigned char kReflexStateGuid[16] = {
    0x85, 0x59, 0xbb, 0xf0,
    0xf9, 0xda,
    0x28, 0x47,
    0xb2, 0xfd, 0xae, 0x80, 0xa2, 0xbd, 0x79, 0x89
};
// 48 de cabecera mas 64 informes de 152 son 9776; se pide de mas y se
// pone en cero, que es como se pidio DLSSGState y nunca fallo.
static unsigned char g_reflex_buf[16384];
// Offsets confirmados con el volcado y con dos chequeos internos: el
// gpuActiveRenderTimeUs que escribe NVIDIA coincide con gpuRenderEnd-Start
// (3802 vs 3883 us) y su gpuFrameTimeUs de 16854 us es exactamente la base de
// 59 fps que medimos aparte. El cuerpo del informe 63, el mas reciente, cae en
// 72 + 152*63 = 9648.
static double g_rfx_gpu = 0.0;     // sim start -> fin de render en GPU
static double g_rfx_drv = 0.0;     // sim start -> fin de driver
static long long g_qpc_freq = 1;   // set in DllMain
static double g_rfx_ft = 0.0;          // gpuFrameTimeUs, para validar contra la base
static double g_ctrl_fps = 0.0;       // frames/tiempo, insesgada, para el controlador
static double g_rfx_base = 0.0;        // base en fps, del contador de frames de Reflex
static ULONGLONG g_ctrl_rfx_ms = 0;    // ultima vez que Reflex alimento el estimador

// El estimador de base del controlador. La logica es la misma de siempre y no
// se toca: lo unico que cambia es de donde viene el dt.
//
// Se calcula n/suma(dt) y no 1/promedio(dt): promediar intervalos para despues
// invertir NO da la tasa media -- por Jensen 1/E[dt] <= E[1/dt] -- y con tiempos
// de frame que varian la base salia sistematicamente baja, el controlador pedia
// de mas y las presentadas quedaban 2% arriba del objetivo. Ese era el sesgo
// residual que no se explicaba.
static void ctrl_feed_dt(double dt) {
    if (dt <= 0.0) return;
    static double ring[8] = { 0,0,0,0,0,0,0,0 };
    static int ri = 0, rn = 0;
    static double rsum = 0.0;
    if (rn == 8) rsum -= ring[ri];
    ring[ri] = dt;
    rsum += dt;
    ri = (ri + 1) & 7;
    if (rn < 8) ++rn;
    g_ctrl_fps = rsum > 0.0 ? (double)rn / rsum : 0.0;
    // Un escalon invalida la historia: se reempieza con el valor nuevo en vez
    // de arrastrar ocho frames de la carga anterior.
    static double prev2 = 0.0;
    if (prev2 > 0.0 && rn > 1) {
        const double media = rsum / (double)rn;
        const bool lejos2 = dt < media * 0.75 || dt > media * 1.33;
        const bool igual2 = dt > prev2 * 0.88 && dt < prev2 * 1.12;
        if (lejos2 && igual2) {
            ring[0] = dt; ri = 1; rn = 1; rsum = dt;
            g_ctrl_fps = 1.0 / dt;
        }
    }
    prev2 = dt;
}

// Reflex manda mientras siga reportando. Medio segundo de silencio -- el menu de
// GTA V congela el marcador -- y el camino del token vuelve a alimentar.
static bool ctrl_fed_by_reflex(void) {
    return g_ctrl_rfx_ms != 0 && GetTickCount64() - g_ctrl_rfx_ms < 500;
}
static LONG g_rfx_n = 0;
// Minimo y maximo ademas del promedio: si la cadencia fraccionaria alterna
// entre comportarse como el entero de abajo y el de arriba, la latencia seria
// dos poblaciones y no un valor intermedio. El promedio de la ventana eso lo
// tapa, que es exactamente como este proyecto ya se equivoco una vez.
static unsigned g_rfx_min = 0xFFFFFFFFu;
static unsigned g_rfx_max = 0;
static unsigned long long g_rfx_lastid = 0;
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
    // Never, now. DYNAMIC is our own controller writing eOn with a count, so
    // there is nothing here to reach for: rebuilding the struct would put
    // eDynamic back over the mode force_into just set, and hand the multiplier
    // to the plugin that was ignoring the target in the first place. Kept
    // rather than deleted because the layout work in it is the record of how
    // DLSSGOptions grows, and the next field NVIDIA adds will need it.
    return options;
    if (g_force_sel != kSelDynamic || !g_dynamic_known || g_opts_version >= 5)
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
        if (g_optsv3 && g_opts_version > 3) {
            static bool said_v3 = false;
            if (!said_v3) {
                said_v3 = true;
                log_num("bench: forcing DLSSGOptions structVersion down to 3 from ",
                        (unsigned)g_opts_version);
            }
            *(unsigned long long *)(p + 24) = 3;
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
        g_game_set_this_frame = 1;
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
    // The game already spoke for this frame; ours would be the repeated call.
    if (g_game_set_this_frame != 0) return;
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
    // The thread guard exists because slDLSSGSetOptions is documented as not
    // thread safe, and replaying it per frame from the render thread produced
    // 2646 "race condition with Present()" warnings and 167 dropped presents.
    // In slow-alternation mode the count changes once every couple of seconds,
    // which is the same rate a player changing a setting would produce, so the
    // guard is lifted there and only there -- otherwise the count can never
    // change at all in a game that configures DLSS-G once at startup, which is
    // exactly what the sample does.
    if (!g_slowalt && (LONG)GetCurrentThreadId() != g_opt_thread) {
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
    // Lo que quedo despues del arreglo, medido con el control entero al lado:
    //
    //   2.75x        ventana 0: 6 tirones (159 ms)   ventana 2: 1 (60 ms)
    //   2.50x        ventana 0: 5 tirones (104 ms)   ventana 2: 1 (52 ms)
    //   2.00x entero ventana 0: 6 tirones ( 98 ms)   ventana 2: 1 (56 ms)  + 1
    //
    // Todos en el arranque, y el entero tiene uno mas que los fraccionarios.
    // O sea el perfil de tirones de la cadencia fraccionaria es indistinguible
    // del entero, y la barra de "cero tirones" no la cumple ni la referencia:
    // la comparacion util es contra el entero, no contra cero.

    // Y la guia de NVIDIA dice lo mismo, en ProgrammingGuideDLSS_G.md:
    //
    //   "the interpolated frame can be dropped if presents go out of sync
    //    (interpolated frame is too close to the last real one)"
    //
    //   slDLSSGSetOptions "takes effect in the next Present() call", conviene
    //   llamarla "primarily during user UI interactions rather than each
    //   frame", y llamarla desde un hilo que no presenta vuelve la
    //   temporizacion "non-deterministic".
    //
    // Llamarla 1307 veces por corrida desde el hilo del token violaba las dos
    // cosas, y desincronizar las presentaciones es exactamente la condicion
    // que hace que el interpolado se descarte. El descarte en si es de diseno.

    // Only when the call would actually say something different.
    //
    // Each slDLSSGSetOptions that changes the count makes the plugin release
    // its resources and start a 100 ms cooldown, during which it refuses to
    // interpolate no matter what we ask:
    //
    //   0x1800497fd  movabs rax, 0x4059000000000000    ; = 100.0
    //   0x180049807  mov    [rdi+0x4488], rax
    //   0x18004b1b8  ... while [rdi+0x4488] > 0, return "not interpolating"
    //
    // That is what GTA V logged as "interpolation state changed from enabled to
    // disabled (mode=eOn, numFramesToGenerate=2)" while we were still asking
    // for generation, and the frames in flight across the resume are the three
    // "Out of order frame - will skip the present".
    //
    // Moving from 2.75x to 2.50x does not change what the API is told -- both
    // ceilings are 2 -- so that call bought a 100 ms blackout for nothing.
    {
        static LONG last_mode = -1, last_count = -1;
        static int have_last = 0;
        const LONG m = *(LONG *)(g_opt_copy + 32);
        const LONG c = *(LONG *)(g_opt_copy + 36);
        if (have_last && m == last_mode && c == last_count) {
            log_num("override: same options as last time, not re-sending; selection ",
                    (unsigned)g_force_sel);
            return;
        }
        last_mode = m; last_count = c; have_last = 1;
    }
    g_orig_setoptions(g_vp_copy, options_for_call(g_opt_copy));
    log_num("override applied now, selection ", (unsigned)g_force_sel);
}

// Dentro de la ventana de corte no reenvia el marcador: el id de frame de
// Reflex deja de avanzar y DLSS-G ve exactamente lo que ve en GTA V.
// Con dos numeros son segundos, como antes. Con cuatro son milisegundos:
// encendido, apagado, cada cuanto el apagon largo, y cuanto dura.
//
// Los cuatro existen porque la ventana de medicion son 45 frames -- unos 790 ms
// a base 57 -- y una rafaga encendida MAS LARGA que eso deja ventanas enteras
// generando, que leen 2.25. Con la rafaga mas corta que la ventana ninguna
// ventana llega a estar entera encendida: las que caen dentro del apagon leen
// 1.0 y las parciales llegan hasta 2.0, sin ninguna en 3.0. El apagon largo
// aparte da la racha de decenas de segundos.
static bool in_marker_gap(void) {
    if (g_marker_every <= 0.0) return false;
    static LARGE_INTEGER f = {}, s0 = {};
    if (f.QuadPart == 0) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&s0); }
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    const double t = (double)(n.QuadPart - s0.QuadPart) / (double)f.QuadPart;
    if (g_marker_long_every > 0.0) {
        const double u = t - (double)((long long)(t / g_marker_long_every)) *
                             g_marker_long_every;
        if (u < g_marker_long_for) return true;
    }
    const double cycle = g_marker_every + g_marker_for;
    return (t - (double)((long long)(t / cycle)) * cycle) >= g_marker_every;
}

static unsigned hk_slPCLSetMarker(unsigned marker, void *frame) {
    if (in_marker_gap()) { ++g_markers_dropped; return 0; }
    return g_orig_pclmarker ? g_orig_pclmarker(marker, frame) : 0;
}

static unsigned hk_slReflexSetMarker(unsigned marker, void *frame) {
    if (in_marker_gap()) { ++g_markers_dropped; return 0; }
    return g_orig_reflexmarker ? g_orig_reflexmarker(marker, frame) : 0;
}

// El informe 63 es el mas reciente. Offsets confirmados con el volcado y
// con dos chequeos internos del propio NVIDIA.
// Cuantos informes hacia atras se leen por consulta, y cada cuantos frames se
// consulta. El producto tiene que cubrir los frames entre consultas.
static const int kReflexBack = 6;
static const int kReflexEvery = 4;

// atras=0 es el informe 63, el mas reciente; atras=1 el 62, y asi.
static void reflex_take(const void *state, int atras) {
    const unsigned char *q = (const unsigned char *)state + 72 + 152 * (63 - atras);
    const unsigned long long id  = *(const unsigned long long *)(q + 0);
    const unsigned long long sim = *(const unsigned long long *)(q + 16);
    const unsigned long long drv = *(const unsigned long long *)(q + 72);
    const unsigned long long gpu = *(const unsigned long long *)(q + 104);
    const unsigned ft = *(const unsigned *)(q + 116);
    // Un informe por frame: consultas seguidas devuelven el mismo.
    if (id == 0 || id == g_rfx_lastid || sim == 0 || gpu <= sim || drv <= sim)
        return;
    // La base, del contador de frames del propio Reflex.
    //
    // La base se estimaba contando llamadas a slGetNewFrameToken. Eso depende
    // de como llame el juego, y los juegos no llaman igual: GTA V lo llama una
    // vez por frame y sale bien, Cyberpunk unas dieciseis y la base leyo
    // mediana 1789 fps con el monitor en 165. El deduplicador por indice que
    // habia para eso nunca sirvio -- en GTA V "frames past the gate" y "hook
    // calls total" son el mismo numero, 141064, o sea que jamas descarto una
    // llamada. Andaba por suerte del juego, no por el filtro.
    //
    // Este id lo lleva el driver y avanza exactamente una vez por frame
    // renderizado, lo llame el juego como lo llame. Y se usa la DIFERENCIA de
    // ids sobre el tiempo, no una cuenta propia, asi que perder muestras entre
    // consultas no sesga nada: si entre dos lecturas pasaron treinta frames, la
    // diferencia dice treinta.
    //
    // El limite de 1000 no es cosmetico: es lo que separa un numero fisico de
    // uno imposible. Si el layout del informe estuviera mal en alguna version
    // de Streamline, esto lo deja afuera en vez de alimentar al controlador con
    // basura, que es exactamente lo que paso.
    {
        // Sobre el MAXIMO id visto, no sobre el ultimo leido.
        //
        // reflex_poll lee seis informes hacia atras y el 63 es el mas nuevo, asi
        // que dentro de una misma consulta los ids llegan en orden DESCENDENTE.
        // Tomando el ultimo, la referencia se reiniciaba en cada consulta y la
        // diferencia se media sobre una secuencia desordenada: dio 75 fps en un
        // escenario con techo de 58.8, o sea un numero imposible. El maximo es
        // monotono y no le importa en que orden lleguen.
        static unsigned long long id_hi = 0, id_ref = 0;
        static LONGLONG t_ref = 0;
        if (id > id_hi) id_hi = id;
        LARGE_INTEGER tn;
        QueryPerformanceCounter(&tn);
        if (id_ref == 0 || id_hi < id_ref || t_ref == 0) {
            id_ref = id_hi;
            t_ref = tn.QuadPart;
        } else if (g_qpc_freq > 0) {
            const double secs = (double)(tn.QuadPart - t_ref) / (double)g_qpc_freq;
            if (secs >= 0.25) {
                const double fps = (double)(id_hi - id_ref) / secs;
                if (fps > 1.0 && fps < 1000.0) g_rfx_base = fps;
                id_ref = id_hi;
                t_ref = tn.QuadPart;
            }
        }
    }
    // El intervalo de frame del driver, al mismo estimador de siempre. Rango
    // fisico: 1 ms a 500 ms, o sea 2 a 1000 fps. Fuera de ahi el informe no
    // dice lo que creemos y se deja pasar al camino del token.
    if (ft >= 1000 && ft <= 500000) {
        ctrl_feed_dt((double)ft / 1e6);
        g_ctrl_rfx_ms = GetTickCount64();
    }
    g_rfx_lastid = id;
    g_rfx_gpu += (double)(gpu - sim);
    g_rfx_drv += (double)(drv - sim);
    g_rfx_ft  += (double)ft;
    ++g_rfx_n;
    const unsigned dv = (unsigned)(drv - sim);
    if (dv < g_rfx_min) g_rfx_min = dv;
    if (dv > g_rfx_max) g_rfx_max = dv;
}

// Se llama una vez por frame desde el hilo de present, que es donde ya
// corre el resto de la medicion.
static void reflex_poll(void) {
    if (g_orig_reflexstate == nullptr) return;
    // Una de cada kReflexEvery. La consulta llena 9776 bytes -- 64 informes de
    // 152 -- y se consumia una vez por ventana de 45 frames para el log y dos
    // veces por segundo para el HUD: llamarla por frame era unas treinta veces
    // mas de lo necesario, en el hilo de render.
    {
        static int n = 0;
        if (++n < kReflexEvery) return;
        n = 0;
    }
    for (int i = 0; i < 64; ++i) g_reflex_buf[i] = 0;
    for (int i = 0; i < 16; ++i) g_reflex_buf[8 + i] = kReflexStateGuid[i];
    *(unsigned long long *)(g_reflex_buf + 24) = 2;      // structVersion 2
    const unsigned r = g_orig_reflexstate(g_reflex_buf);
    if (r != 0) {
        static bool said = false;
        if (!said) { said = true; log_num("reflex: query refused, code ", r); }
        return;
    }
    // Se leen los ultimos informes, no solo el 63. El anillo tiene 64 y la
    // consulta se hace cada pocos frames, asi que leer hacia atras recupera los
    // frames que pasaron entre una consulta y la siguiente: mismas muestras con
    // una fraccion de las llamadas. reflex_take deduplica por frameID, asi que
    // releer uno ya visto no cuenta dos veces.
    for (int k = kReflexBack - 1; k >= 0; --k)
        reflex_take(g_reflex_buf, k);
}

static unsigned hk_slReflexGetState(void *state) {
    const unsigned r = g_orig_reflexstate ? g_orig_reflexstate(state) : 1;
    // El layout salio del volcado, no de suponer: GUID en +8, version 2 en +24,
    // banderas en +32, y frameReport[i] es a su vez una estructura de Streamline
    // de 152 bytes con cabecera propia de 32, asi que su cuerpo arranca en
    // 72 + 152*i. El [63] es el mas reciente: 72 + 152*63 = 9648.
    static int calls = 0;
    ++calls;
    if (r == 0 && state != nullptr) reflex_take(state, 0);
    if (r == 0 && state != nullptr && !g_reflex_dumped && calls > 300) {
        g_reflex_dumped = true;
        log_num("reflex: latencyReportAvailable ",
                (unsigned)((const unsigned char *)state)[33]);
        const unsigned char *p = (const unsigned char *)state + 9648;
        for (int row = 0; row < 8; ++row) {
            char line[80];
            int k = 0;
            const char *pre = "reflex dump +";
            while (pre[k] != 0) { line[k] = pre[k]; ++k; }
            const int off = row * 16;
            line[k++] = (char)('0' + (off / 100) % 10);
            line[k++] = (char)('0' + (off / 10) % 10);
            line[k++] = (char)('0' + off % 10);
            line[k++] = ':';
            for (int c = 0; c < 16; ++c) {
                const unsigned char v = p[off + c];
                line[k++] = ' ';
                line[k++] = "0123456789abcdef"[v >> 4];
                line[k++] = "0123456789abcdef"[v & 15];
            }
            line[k] = 0;
            log_line(line);
        }
    }
    return r;
}

static unsigned hk_slGetFeatureFunction(unsigned feature, const char *name, void *&fn) {
    const unsigned r = g_orig_getfeaturefn(feature, name, fn);
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

// The game asks for a frame token once per frame, from its own render thread
// -- which is the thread that called slDLSSGSetOptions and the only one it is
// safe to call it from again. With DLSS-G on, Present is Streamline's thread,
// so the replay cannot happen there; the log said so outright.
typedef unsigned (*PFN_slGetNewFrameToken)(void *&, const unsigned *);
static PFN_slGetNewFrameToken g_orig_frametoken = nullptr;
static void *g_frametoken_addr = nullptr;      // found at startup, hooked later
static void apply_override_now(void);

static void query_state(void);


// ---- the sub-frame count, made writable ---------------------------------

// Una LISTA de sitios, no un puntero.
//
// Eran punteros unicos y cada copia parcheada los pisaba. Con dos copias de
// sl.dlss_g en el proceso -- lo que hace Cyberpunk, que mapea la del juego Y la
// del cache OTA -- las escrituras por frame se iban al modulo que se parcheo
// ultimo, que no es necesariamente el que genera.
//
// Reproducido en el banco con mfg-twocopies.txt: pidiendo 2.50x con dos copias
// entrega 2.00 en las 34 ventanas, y con una copia entrega 2.50 en las 34.
// Separacion total, sin una sola excepcion.
//
// Escribir en una copia que no se usa es inofensivo: es un byte en su .text que
// nadie lee. Buscar cual es la buena seria adivinar; escribir en todas no.
static const int kMaxSitios = 4;
static void sitio_add(volatile unsigned char **lista, int *n, volatile unsigned char *q) {
    for (int i = 0; i < *n; ++i) if (lista[i] == q) return;
    if (*n < kMaxSitios) lista[(*n)++] = q;
}
static void sitio_write(volatile unsigned char **lista, int n, unsigned char v) {
    for (int i = 0; i < n; ++i) if (lista[i] != nullptr) *lista[i] = v;
}
static volatile unsigned char *g_wic_sitios[kMaxSitios] = { nullptr, nullptr, nullptr, nullptr };
static int g_wic_n = 0;
static bool g_wic_mode = false;                         // mfg-wic.txt

// The count in the work item, rather than the comparison that reads it.
//
// Patching the loop's bound leaves every other consumer reading the real count
// out of the structure, and they then disagree by construction. The present
// index is built as `frame*6 + [r13+4]`, so with a bound of 2 the loop makes
// two sub-frames while the index is still computed from the game's 1: the
// second one lands on an index the plugin has already passed and is thrown
// away as "Out of order frame - will skip the present". That is where 530 such
// skips per run come from -- a stock run has none -- and why a bound of 2
// measured as 1.85x rather than 3.0x. It was generating two frames and
// delivering one.
//
// So write the field instead and leave every comparison alone. The bound, the
// present index and the metering all read the same number because it is the
// same number.
//
//   8B 42 04            mov eax, [rdx+4]      -> 6A NN   push NN
//                                                58      pop rax
//   41 B8 C0 00 00 00   mov r8d, 0xc0            (unchanged)
//   89 41 04            mov [rcx+4], eax         (unchanged)
//
// Three bytes are rewritten; the twelve are only what the signature matches on.
// push imm8 sign-extends into rax, so NN under 0x80 arrives zero-extended, and
// set_count_now clamps to 0..5. `8B 42 04` occurs exactly once in the whole
// file, and the found != 1 guard below refuses to write anything otherwise.
//
// The cost of using the stack here: between the push and the pop, RSP is eight
// below what this function's unwind information describes, so a stack walk that
// lands in that two-instruction window -- a profiler, a crash handler, an
// overlay -- would unwind wrongly. The window is two instructions with no call
// in it, and no encoding of "put a byte we own into eax" fits in three bytes
// without touching the stack, so this is a known and accepted risk rather than
// an oversight.
static int patch_work_item_count(unsigned char *text, size_t len) {
    size_t found = 0, at = 0;
    for (size_t i = 0; i + 12 <= len; ++i) {
        if (text[i] != 0x8B || text[i+1] != 0x42 || text[i+2] != 0x04) continue;
        if (text[i+3] != 0x41 || text[i+4] != 0xB8 || text[i+5] != 0xC0) continue;
        if (text[i+6] || text[i+7] || text[i+8]) continue;
        if (text[i+9] != 0x89 || text[i+10] != 0x41 || text[i+11] != 0x04) continue;
        ++found;
        at = i;
    }
    if (found != 1) {
        log_num("  ! work item count site not unique, sites: ", (unsigned)found);
        return 0;
    }
    unsigned char *q = text + at;
    DWORD old = 0;
    // NN is rewritten every frame, so the page stays writable for the life of
    // the process. VirtualProtect works at page granularity, so this opens the
    // whole 4 KB code page and there is no narrower way to do it; restoring the
    // old protection first and reopening immediately afterwards would only look
    // tidier. Said plainly rather than hidden behind a restore that does not
    // hold.
    if (!VirtualProtect(q, 3, PAGE_EXECUTE_READWRITE, &old)) return 0;
    q[0] = 0x6A;    // push imm8
    q[1] = 1;       // NN
    q[2] = 0x58;    // pop rax
    sitio_add(g_wic_sitios, &g_wic_n, q + 1);
    log_line("  work item count is ours (bound, index and metering agree)");
    return 1;
}

static volatile unsigned char *g_imm_sitios[kMaxSitios] = { nullptr, nullptr, nullptr, nullptr };
static int g_imm_n = 0;
static volatile unsigned char *g_imm2_sitios[kMaxSitios] = { nullptr, nullptr, nullptr, nullptr };
static int g_imm2_n = 0;
static volatile unsigned char *g_imm3_sitios[kMaxSitios] = { nullptr, nullptr, nullptr, nullptr };
static int g_imm3_n = 0;
static volatile unsigned char *g_gen_flag = nullptr;    // 1 generating, 0 not
static volatile unsigned char *g_lat_allow = nullptr;   // 1 reconfigure, 0 leave alone

// The swap chain's frame latency, reconfigured during bring-up and left alone
// afterwards.
//
// Pinning it outright (patch_pin_frame_latency, below) removes the churn and
// also stops generation from ever starting, because the reconfiguration is part
// of bringing it up. So the call is gated instead of replaced: allowed while
// generation establishes itself, suppressed once it has, which is when the
// cadence starts toggling and the churn would begin.
//
//   8B 56 60   mov edx, dword ptr [rsi+0x60]
//   FF 50 60   call qword ptr [rax+0x60]        ; SetMaximumFrameLatency
//
// Six bytes, replaced by a call to a trampoline that performs exactly those two
// instructions when our byte allows it and returns otherwise. rcx, rsi and rax
// are all live and set up by the caller; rax is dead after the call, so nothing
// downstream sees a difference beyond the reconfiguration not happening.
static int patch_gate_frame_latency(unsigned char *base, unsigned char *text, size_t len) {
    size_t found = 0, at = 0;
    for (size_t i = 0; i + 13 <= len; ++i) {
        if (text[i] != 0x48 || text[i+1] != 0x8B || text[i+2] != 0x01) continue;
        if (text[i+3] != 0x4C || text[i+4] != 0x8D || text[i+5] != 0x43 || text[i+6] != 0x40) continue;
        if (text[i+7] != 0x8B || text[i+8] != 0x56 || text[i+9] != 0x60) continue;
        if (text[i+10] != 0xFF || text[i+11] != 0x50 || text[i+12] != 0x60) continue;
        ++found;
        at = i;
    }
    if (found != 1) {
        log_num("  ! frame latency site not unique, sites: ", (unsigned)found);
        return 0;
    }

    unsigned char *tr = nullptr;
    for (unsigned long long step = 0x10000; step < 0x8000000ULL && tr == nullptr; step += 0x10000) {
        tr = (unsigned char *)VirtualAlloc(base - step, 0x1000,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (tr == nullptr) { log_line("  ! no page for the latency trampoline"); return 0; }

    int k = 0;
    tr[k++] = 0x48; tr[k++] = 0x83; tr[k++] = 0xEC; tr[k++] = 0x28;   // sub rsp,0x28
    tr[k++] = 0x80; tr[k++] = 0x3D;                                   // cmp byte [rip+d], 0
    const int disp_at = k; k += 4;
    tr[k++] = 0x00;   // the immediate the byte is compared against; leaving it
                      // out made the next opcode serve as it, shifted the whole
                      // trampoline by one, and killed the process with
                      // 0xC0000409 before a single frame was measured.
    tr[k++] = 0x74; const int je_at = k; tr[k++] = 0x00;              // je skip
    tr[k++] = 0x8B; tr[k++] = 0x56; tr[k++] = 0x60;                   // mov edx,[rsi+0x60]
    tr[k++] = 0xFF; tr[k++] = 0x50; tr[k++] = 0x60;                   // call [rax+0x60]
    const int skip = k;
    tr[k++] = 0x48; tr[k++] = 0x83; tr[k++] = 0xC4; tr[k++] = 0x28;   // add rsp,0x28
    tr[k++] = 0xC3;                                                   // ret
    const int flag_at = k;
    tr[k] = 1;                                                        // allowed at first
    tr[je_at] = (unsigned char)(skip - (je_at + 1));
    const LONG d = (LONG)(flag_at - (disp_at + 4 + 1));   // rip is after the cmp's imm8
    for (int i = 0; i < 4; ++i) tr[disp_at + i] = (unsigned char)((d >> (8 * i)) & 0xFF);
    g_lat_allow = tr + flag_at;

    unsigned char *site = text + at + 7;
    const long long delta = (long long)tr - (long long)(site + 5);
    if (delta > 0x7FFFFFFFLL || delta < -0x7FFFFFFFLL) {
        log_line("  ! latency trampoline out of rel32 range");
        return 0;
    }
    DWORD old = 0;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) return 0;
    site[0] = 0xE8;
    const LONG rel = (LONG)delta;
    for (int i = 0; i < 4; ++i) site[1 + i] = (unsigned char)((rel >> (8 * i)) & 0xFF);
    site[5] = 0x90;
    VirtualProtect(site, 6, old, &old);
    log_line("  frame latency gated (reconfigured only during bring-up)");
    return 1;
}

// The swap chain's frame latency, pinned.
//
// throttleFlipQueue reconfigures IDXGIDevice1::SetMaximumFrameLatency whenever
// interpolation starts or stops, and a cadence that turns generation off for
// some frames does that constantly.
//
// The churn is real and has now been counted rather than inferred: hooking the
// DXGI method itself (IDXGISwapChain2 slot 31 -- IDXGIDevice1 does not exist on
// D3D12) records 3915 calls in a 2.50x run, exactly one per rendered frame,
// against 2 calls in either integer control.
//
// But it is not what pins the base rate, and it is not what gates generation.
// Clamping the value to 2 from the DXGI side leaves 2.50x at base 55 and 137
// presented, identical to unclamped, and leaves 3.00x reading 3.000 at 165 --
// so the old reading that pinning it stops interpolation was wrong about the
// cause as well as the effect.
//
// Measured on the sample at 1.50x, from the era of the broken instruments:
//
//   66  SetMaximumFrameLatency changed from 1 to 2
//   67  SetMaximumFrameLatency changed from 2 to 1
//   79  Couldn't lock the mutex on sync present - will skip the present
//
// A constant 2.00x run has none of those three. Reconfiguring the swap chain
// races the present that is already in flight, the present loses, and it is
// dropped -- 79 of them, which is the missing half of the frame rate. Nothing
// upstream is at fault: the count, the loop bound, the metering and the
// generation flag were all correct, and this happens downstream of all four.
//
//   8B 56 60   mov edx, dword ptr [rsi+0x60]    ; the latency it wants
//   FF 50 60   call qword ptr [rax+0x60]        ; SetMaximumFrameLatency
//
// becomes `push 2 ; pop rdx` -- three bytes for three -- so the value asked for
// is always the one a healthy always-on run settles at, and the call becomes a
// no-op instead of a reconfiguration. This is a deliberate override of the
// plugin's own judgement, which is why it is worth saying plainly: frame
// generation was not built to be switched per frame, and this is the piece
// that assumed it would not be.
static bool g_pin_latency = false;    // mfg-pinlatency.txt: diagnostic
static bool g_pace_follow = false;    // mfg-pacefollow.txt
// Por que el plugin apaga la interpolacion solo, con mode=eOn.
//
// En GTA V el log dice "interpolation state changed from enabled to disabled
// (mode=eOn, numFramesToGenerate=2)" mientras nosotros seguimos pidiendo
// generacion, y 33 ms despues llegan tres frames fuera de orden. La decision
// esta en una sola funcion de sl.dlss_g.dll:
//
//   0x18004a930   devuelve AL = interpola este frame
//      [rdx+0x20] es el modo   0 eOff  1 eOn  2 eAuto  3 eDynamic
//      eAuto -> el analisis largo de camara y escena
//      eOn   -> 0x4ae5a
//      todos convergen en 0x4ad4f:
//          cmp  dword ptr [r15], 0
//          je   0x4ad18      ->  xor al,al ; ret     = APAGADA
//
// El llamador guarda el resultado en [r14+0x45da], el anterior en [r14+0x45db],
// y loguea cuando difieren.
//
// O sea es una condicion de DATOS sobre la estructura por frame, no un evento
// de ventana. Eso explica por que cinco hipotesis de entorno dieron todas cero
// en el banco: alternancia fraccionaria, 40% de jitter, apagar desde cuenta 2
// (la caida de latencia 3->1 si se reproduce, el desorden no), forzar un cambio
// de sincronizacion, y robarle el foco a la ventana.
//
// [r15+0] resulto ser numFramesToGenerate: la estructura es
// {cuenta, cuenta, cuenta+1, ..., modo en +0x20}, armada en rbp+0x620.
//
// Y hay DOS salidas de apagado para eOn:
//
//   0x4ad4f  cmp  dword ptr [r15], 0    ; cuenta 0 -> APAGADA
//            je   0x4ad18               ;   (nuestro estado bajo sub-2.0x)
//   0x4ad54  cmp  byte ptr [rdi+0x42b8], 0
//            je   0x4b1b8               ; la otra:
//   0x4b1b8      movsd  xmm0, [rdi+0x4488]   ; un acumulador
//                comisd xmm0, 0
//                jbe    0x4b1f9              ; ya drenado -> seguir
//                subsd  xmm0, [rdi+0x478]    ; menos el delta del frame
//                maxsd  xmm0, 0
//                movsd  [rdi+0x4488], xmm0
//                comisd xmm0, 0
//                ja     0x4ad18              ; todavia cuenta -> APAGADA
//
// O sea [ctx+0x4488] es un ENFRIAMIENTO: mientras sea mayor que cero el plugin
// se niega a interpolar, descontando el delta de cada frame. Eso es lo que
// apago la generacion en GTA V con mode=eOn y cuenta 2 -- no un evento de
// ventana, no una cuenta en cero. Al vencerse vuelve, y los frames en vuelo de
// esa transicion son los que llegan desordenados.
//
// Falta: quien escribe [ctx+0x4488] y con que duracion.

// The pacer's wait, made to follow the frame's own count.
//
// This is where the throughput law lives. presentCommon's pacing block computes
// how long to wait before letting the producer go:
//
//   48 8B B6 C8 0C 00 00   mov  rsi, [r14+0xcc8]   ; the refresh interval, us
//   41 8B 4D 04            mov  ecx, [r13+4]       ; the count
//   48 0F AF CE            imul rcx, rsi           ; wait = count * interval
//   48 2B C8               sub  rcx, rax           ; less what already elapsed
//
// and [r13+4] holds the count the *API* was told, which is ceil(ratio - 1) --
// the cadence ceiling, because telling the API anything else crashes. So every
// frame waits ceiling * interval even when it generates fewer, and the producer
// settles at refresh/(ceiling + 1). That is exactly the measured law,
// presented = refresh * ratio / (ceiling + 1): at 2.25x the base is 55 = 165/3,
// the base 3.00x pays, and a quarter of the display's slots go unused.
//
// Five other approaches were measured and none moved it: the refresh cap (six
// levers), pinning the latency in the plugin's bytes (kills the run), the block
// distribution (base 55 across four layouts), the queue parallelism mode (base
// 55 across all four) and clamping SetMaximumFrameLatency from the DXGI side
// (3915 calls counted in a 2.50x run, base unchanged).
//
// `mov ecx, [r13+4]` is four bytes and `push imm8; pop rcx; nop` is four, the
// same trade patch_work_item_count already makes, so the count the pacer waits
// on becomes a byte this file writes per frame.
//
// THE PREMISE WAS HALF WRONG, AND THE CORRECTION MATTERS. Measured in GTA V, standing
// still and switching selection without moving the camera:
//
//   2.00x  28 windows  base 60  ratio 2.01  presented 121
//   2.50x  20 windows  base 55  ratio 2.48  presented 137
//   3.00x  17 windows  base 57  ratio 2.61  presented 145
//
// 2.50x presents MORE than 2.00x -- but the pinning is still there. 55 is
// 165/3 exactly, so the pacer is binding at 2.50x in the game too; the game's
// GPU-bound 60 only binds at 2.00x, where the pacer would have allowed 82.
//
// What decides whether the pinning costs anything is whether the integer below
// already fills the display:
//
//   bench, no slow frame:  2.00x presents 165 = the refresh. No headroom, so
//                          losing base to the ceiling cannot be paid back and
//                          every fractional value loses.
//   GTA V:                 2.00x presents 121 of 165. Headroom, so trading
//                          base 60 -> 55 for ratio 2.0 -> 2.5 nets +17.
//
// So: fractional wins when the integer below does not saturate the display and
// loses when it does. The seven patches that failed to move the base were not
// chasing a phantom -- the base really is pinned -- they were chasing something
// whose cost is conditional, in the one setup where the condition was worst.
//
// IT DOES NOT MOVE THE BASE. The patch applies, the integer controls stay exact
// (1.000, 2.001, 3.000) so it is safe, and 2.50x reads base 55 and 138
// presented with it and without it -- identical. The wait is computed from our
// per-frame count now and the producer still settles at refresh/(ceiling + 1).
//
// So the pinning is not this arithmetic, and the number that says so plainly:
// at 2.25x the app renders 55 and 124 presents leave. If presents were the
// limited resource at 165/s the render rate would be 73. Something waits three
// intervals per rendered frame and it is not this computation. Kept behind
// mfg-pacefollow.txt, off by default, because the site and the reasoning are
// right and the next idea will start here.
//
// And it is not presents being issued and then dropped: sl.log records zero
// "will skip the present" in every configuration, so the plugin issues exactly
// `ratio` presents per rendered frame. 2.25x, 2.50x, 2.75x and 3.00x all render
// at 55 while presenting 124, 138, 151 and 165 -- the producer is fixed
// regardless of how many presents actually leave.
static volatile unsigned char *g_pace_count = nullptr;
static int patch_pacer_count(unsigned char *text, size_t len) {
    static const unsigned char sig[18] = {
        0x49, 0x8B, 0xB6, 0xC8, 0x0C, 0x00, 0x00,
        0x41, 0x8B, 0x4D, 0x04,
        0x48, 0x0F, 0xAF, 0xCE,
        0x48, 0x2B, 0xC8 };
    size_t found = 0, at = 0;
    for (size_t i = 0; i + sizeof sig <= len; ++i) {
        bool ok = true;
        for (size_t k = 0; k < sizeof sig; ++k)
            if (text[i + k] != sig[k]) { ok = false; break; }
        if (!ok) continue;
        ++found; at = i;
    }
    if (found != 1) {
        log_num("  ! pacer wait site not unique, sites: ", (unsigned)found);
        return 0;
    }
    unsigned char *q = text + at + 7;      // the mov ecx, [r13+4]
    DWORD old = 0;
    if (!VirtualProtect(q, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    q[0] = 0x6A;    // push imm8
    q[1] = 0x01;    // seeded at one; set_count_now owns it from here
    q[2] = 0x59;    // pop rcx
    q[3] = 0x90;    // nop
    // The protection stays open, the way patch_work_item_count leaves its own
    // site open, because the immediate is rewritten every frame from here.
    // Restoring it cost four runs: the patch applied, the log said so, and the
    // first per-frame write faulted on a page that had been put back to
    // read-only -- every arm with the patch died while the unpatched control
    // beside it ran clean.
    g_pace_count = q + 1;
    return 1;
}

static int patch_pin_frame_latency(unsigned char *text, size_t len) {
    size_t found = 0, at = 0;
    for (size_t i = 0; i + 13 <= len; ++i) {
        if (text[i] != 0x48 || text[i+1] != 0x8B || text[i+2] != 0x01) continue;
        if (text[i+3] != 0x4C || text[i+4] != 0x8D || text[i+5] != 0x43 || text[i+6] != 0x40) continue;
        if (text[i+7] != 0x8B || text[i+8] != 0x56 || text[i+9] != 0x60) continue;
        if (text[i+10] != 0xFF || text[i+11] != 0x50 || text[i+12] != 0x60) continue;
        ++found;
        at = i;
    }
    if (found != 1) {
        log_num("  ! frame latency site not unique, sites: ", (unsigned)found);
        return 0;
    }
    unsigned char *q = text + at + 7;
    DWORD old = 0;
    if (!VirtualProtect(q, 3, PAGE_EXECUTE_READWRITE, &old)) return 0;
    q[0] = 0x6A;    // push 2
    q[1] = 0x02;
    q[2] = 0x5A;    // pop rdx
    VirtualProtect(q, 3, old, &old);
    log_line("  frame latency pinned (no swap chain churn on toggling)");
    return 1;
}

// The count the present index is built from, made to agree with the loop.
//
// This is the disagreement behind all three boundaries. Our loop patch replaced
// the bound with an immediate, so the loop makes the number of sub-frames we
// choose -- but the index the present path builds is still computed from the
// *real* count in the work item:
//
//   41 8B 45 04              mov eax, [r13+4]        ; the real count
//   48 8D 0C 4D 01 00 00 00  lea rcx, [rcx*2+1]      ; frame x 6 + 1
//   48 03 C8                 add rcx, rax            ; + the count
//
// So a bound below the API count leaves the present path waiting for an index
// that is never produced -- which is exactly what stops presentation -- and a
// bound above it walks off the allocation.
//
// `33 C0 B0 NN` is xor eax,eax plus mov al,NN: four bytes for four, and the
// flags it clobbers are already spent on the `je` above it. The index is then
// built from the same number the loop counts to. Unlike patch_monotonic_index,
// which replaced the arithmetic wholesale and stopped generation, this leaves
// the formula exactly as NVIDIA wrote it.
static volatile unsigned char *g_idx_count = nullptr;

static int patch_index_count(unsigned char *base, unsigned char *text, size_t len) {
    (void)base;
    size_t found = 0, at = 0;
    for (size_t i = 0; i + 20 <= len; ++i) {
        if (text[i] != 0x4D || text[i+1] != 0x85 || text[i+2] != 0xED) continue;
        if (text[i+3] != 0x74) continue;
        if (text[i+5] != 0x41 || text[i+6] != 0x8B ||
            text[i+7] != 0x45 || text[i+8] != 0x04) continue;
        if (text[i+9] != 0x48 || text[i+10] != 0x8D || text[i+11] != 0x0C ||
            text[i+12] != 0x4D || text[i+13] != 0x01) continue;
        if (text[i+17] != 0x48 || text[i+18] != 0x03 || text[i+19] != 0xC8) continue;
        ++found;
        at = i;
    }
    if (found != 1) {
        log_num("  ! index count site not unique, sites: ", (unsigned)found);
        return 0;
    }
    unsigned char *q = text + at + 5;
    DWORD old = 0;
    if (!VirtualProtect(q, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    q[0] = 0x33; q[1] = 0xC0;     // xor eax, eax
    q[2] = 0xB0; q[3] = 1;        // mov al, NN
    VirtualProtect(q, 4, old, &old);
    DWORD ig = 0;
    VirtualProtect(q + 3, 1, PAGE_EXECUTE_READWRITE, &ig);
    g_idx_count = q + 3;
    log_line("  present index count now follows the loop");
    return 1;
}

// The present index, made monotonic.
//
// A present is accepted only if its index is strictly greater than the last
// one seen (cmp rcx,[rax+0x18] / ja), and the index is built from the count of
// the frame it belongs to:
//
//   48 8D 0C 76              lea rcx, [rsi + rsi*2]     ; frame x 3
//   4D 85 ED / 74 xx         test r13,r13 / je
//   41 8B 45 04              mov eax, [r13+4]           ; this frame's count
//   48 8D 0C 4D 01 00 00 00  lea rcx, [rcx*2 + 1]       ; frame x 6 + 1
//   48 03 C8                 add rcx, rax               ; + the count
//
// With a constant count that is monotonic and nothing is ever dropped -- 2.00x
// and 3.00x both run at 80 fps with zero drops. With a cadence that alternates,
// a frame generating one after a frame generating two yields a *lower* index
// and the present is discarded as out of order: 356 of them at 2.50x, which is
// why 2.50x presents fewer frames (308) than 2.00x (427).
//
// The block is six wide whatever the count, so the room is there; the index
// simply must not be derived from a number that moves. `add rcx, rax` becomes
// `add rcx, [rip+d]` -- three bytes for a seven-byte instruction, so the two
// preceding are absorbed: the `mov eax,[r13+4]` that reads the count is no
// longer needed and its four bytes are exactly the difference. Our counter is
// incremented once per frame and only ever rises.
static volatile unsigned long long *g_pidx = nullptr;

static int patch_monotonic_index(unsigned char *base, unsigned char *text, size_t len) {
    size_t found = 0, at = 0;
    for (size_t i = 0; i + 20 <= len; ++i) {
        if (text[i] != 0x4D || text[i+1] != 0x85 || text[i+2] != 0xED) continue;  // test r13,r13
        if (text[i+3] != 0x74) continue;                                          // je
        if (text[i+5] != 0x41 || text[i+6] != 0x8B ||
            text[i+7] != 0x45 || text[i+8] != 0x04) continue;                     // mov eax,[r13+4]
        if (text[i+9] != 0x48 || text[i+10] != 0x8D || text[i+11] != 0x0C ||
            text[i+12] != 0x4D || text[i+13] != 0x01) continue;                   // lea rcx,[rcx*2+1]
        if (text[i+17] != 0x48 || text[i+18] != 0x03 || text[i+19] != 0xC8) continue;  // add rcx,rax
        ++found;
        at = i;
    }
    if (found != 1) {
        log_num("  ! present index site not unique, sites: ", (unsigned)found);
        return 0;
    }

    // The counter lives near the plugin so a rip-relative add can reach it.
    unsigned char *page = nullptr;
    for (unsigned long long step = 0x10000; step < 0x8000000ULL && page == nullptr; step += 0x10000) {
        page = (unsigned char *)VirtualAlloc(base - step, 0x1000,
                                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    if (page == nullptr) { log_line("  ! no page for the present counter"); return 0; }
    *(unsigned long long *)page = 0;

    // mov eax,[r13+4] (4 bytes) + lea rcx,[rcx*2+1] (8) + add rcx,rax (3) = 15,
    // rewritten as lea (8) + add rcx,[rip+d] (7) = 15. The count is not read.
    unsigned char *q = text + at + 5;
    DWORD old = 0;
    if (!VirtualProtect(q, 15, PAGE_EXECUTE_READWRITE, &old)) return 0;
    int k = 0;
    q[k++] = 0x48; q[k++] = 0x8D; q[k++] = 0x0C; q[k++] = 0x4D;   // lea rcx,[rcx*2+1]
    q[k++] = 0x01; q[k++] = 0x00; q[k++] = 0x00; q[k++] = 0x00;
    q[k++] = 0x48; q[k++] = 0x03; q[k++] = 0x0D;                  // add rcx,[rip+d]
    const long long d = (long long)page - (long long)(q + 15);
    if (d > 0x7FFFFFFFLL || d < -0x7FFFFFFFLL) {
        VirtualProtect(q, 15, old, &old);
        log_line("  ! present counter out of rel32 range");
        return 0;
    }
    const LONG rel = (LONG)d;
    for (int i = 0; i < 4; ++i) q[k++] = (unsigned char)((rel >> (8 * i)) & 0xFF);
    VirtualProtect(q, 15, old, &old);
    g_pidx = (unsigned long long *)page;
    log_line("  present index made monotonic");
    return 1;
}

// A frame that generates nothing has to take the ordinary present path.
//
// Measured: at 1.50x the sample presented 198 frames against 427 for a
// constant 2.00x -- almost exactly half, and half the frames are the ones our
// cadence gives a count of zero. Those frames present *nothing at all*: the
// real frame is lost along with the generated one, which is why the rate halves
// while the median frame time stays healthy and the p99 goes to 138ms.
//
// presentCommon decides which path a frame takes from one byte:
//
//   0x180046c7d  call <decides whether interpolation runs>   E8 rel32
//   0x180046c82  mov byte ptr [r14+0x45da], al
//   ...
//   0x180046f15  cmp byte ptr [r14+0x45da], 0
//   0x180046f1d  jne <the generation path>
//                <falls through to the ordinary present, which does present>
//
// The store has no room for an extra instruction -- seven bytes, and the
// smallest useful edit needs thirteen. The call before it is five bytes and can
// be redirected whole, so the decision is taken as the plugin makes it and then
// masked with a byte of ours: the plugin still decides when generation is off
// for its own reasons, and we can only ever turn it off, never on.
static int patch_generation_flag(unsigned char *base, unsigned char *text, size_t len) {
    size_t found = 0, at = 0;
    for (size_t i = 0; i + 15 <= len; ++i) {
        if (text[i] != 0x49 || text[i + 1] != 0x8B || text[i + 2] != 0xCE) continue;  // mov rcx,r14
        if (text[i + 3] != 0xE8) continue;                                            // call rel32
        if (text[i + 8] != 0x41 || text[i + 9] != 0x88 || text[i + 10] != 0x86) continue;
        // mov byte ptr [r14+0xNNNN], al -- NINGUNO de los dos bytes bajos del
        // desplazamiento se fija. El comentario original decia que el bajo es
        // "lo unico que se mueve entre versiones" (0x45da en 2.13, 0x45e2 en
        // 2.12) y sin embargo fijaba el alto en 0x45. En el build OTA 134656 --
        // el que carga Halo -- el desplazamiento salio de esa pagina y la firma
        // devolvia 0 sitios.
        //
        // Escaneo de todos los plugins de la maquina, contando sitios con la
        // firma vieja y con esta:
        //
        //     GTA V 625792      1   1
        //     Cyberpunk 578176  0   0
        //     NGX 134273        1   1
        //     NGX 134656        0   1   <-- el que importa para Halo
        //     los demas         0   0
        //
        // Relajar no produce mas de un sitio en ningun build, asi que la guarda
        // de unicidad de abajo sigue siendo la que protege.
        if (text[i + 13] != 0x00 || text[i + 14] != 0x00) continue;
        ++found;
        at = i;
    }
    if (found != 1) {
        log_num("  ! generation flag site not unique, sites: ", (unsigned)found);
        return 0;
    }

    unsigned char *call_at = text + at + 3;
    LONG rel = 0;
    for (int i = 0; i < 4; ++i) rel |= (LONG)call_at[1 + i] << (8 * i);
    unsigned char *orig = call_at + 5 + rel;

    // The trampoline has to sit within reach of a rel32, so it is allocated
    // near the plugin rather than wherever our own module happens to be.
    unsigned char *tr = nullptr;
    for (unsigned long long step = 0x10000; step < 0x8000000ULL && tr == nullptr; step += 0x10000) {
        tr = (unsigned char *)VirtualAlloc(base - step, 0x1000,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (tr == nullptr) { log_line("  ! no page within reach for the flag trampoline"); return 0; }

    int k = 0;
    tr[k++] = 0x48; tr[k++] = 0x83; tr[k++] = 0xEC; tr[k++] = 0x28;   // sub rsp,0x28
    tr[k++] = 0x48; tr[k++] = 0xB8;                                   // mov rax, imm64
    for (int i = 0; i < 8; ++i) tr[k++] = (unsigned char)(((unsigned long long)orig >> (8 * i)) & 0xFF);
    tr[k++] = 0xFF; tr[k++] = 0xD0;                                   // call rax
    tr[k++] = 0x48; tr[k++] = 0x83; tr[k++] = 0xC4; tr[k++] = 0x28;   // add rsp,0x28
    tr[k++] = 0x22; tr[k++] = 0x05;                                   // and al, [rip+1]
    tr[k++] = 0x01; tr[k++] = 0x00; tr[k++] = 0x00; tr[k++] = 0x00;
    tr[k++] = 0xC3;                                                   // ret
    tr[k] = 1;                                                        // the flag, on by default
    g_gen_flag = tr + k;

    const long long delta = (long long)tr - (long long)(call_at + 5);
    if (delta > 0x7FFFFFFFLL || delta < -0x7FFFFFFFLL) {
        log_line("  ! flag trampoline out of rel32 range");
        return 0;
    }
    DWORD old = 0;
    if (!VirtualProtect(call_at, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    const LONG nrel = (LONG)delta;
    for (int i = 0; i < 4; ++i) call_at[1 + i] = (unsigned char)((nrel >> (8 * i)) & 0xFF);
    VirtualProtect(call_at, 5, old, &old);
    log_line("  generation flag redirected (zero-count frames present normally)");
    return 1;
}

static int patch_subframe_count(unsigned char *base) {
    if (!g_frac_enabled) return 0;
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

    // The work-item count, by default. It is the site every measurement in
    // this file was taken through, and patching the count field instead of the
    // comparison removed about 525 out-of-order present skips. Refused with
    // mfg-nowic.txt.
    g_wic_mode = !flag_file(L"mfg-nowic.txt");
    if (g_wic_mode) {
        // The comparison patches are exactly what this replaces; leaving
        // them in would put the bound back out of step with the field.
        const int wic_sitios = patch_work_item_count(text, len);
        g_vio_alguna_copia = true;
        if (wic_sitios == 0) g_wic_ok = false;
        log_line(wic_sitios > 0
                 ? "  PARCHE DE CUENTA: engancho en esta copia"
                 : "  PARCHE DE CUENTA: NO engancho en esta copia");
        if (wic_sitios == 0) {
            // Nothing downstream reports this on its own: set_count_now would
            // simply do nothing, the run would execute at the fixed API ceiling,
            // and the only outward sign would be the absence of the "fractional:"
            // blocks. That is the silent-regression shape a moved signature
            // produces after a driver update, so it is said out loud here.
            log_line("  ! fractional multiplier NOT active: the count site moved");
            return 0;
        }
        // And the flag that says whether this frame interpolates at all. A
        // count of zero makes the loop produce nothing while the plugin still
        // believes it is generating, and the frame is then presented down a
        // path with nothing to present: 88 rendered against 81 presented at
        // 1.50x, losing the real frame rather than merely not adding one.
        {
            // NO entra en el freno. Se probo exigirla y en Cyberpunk falla en
            // las DOS copias -- y ese juego viene entregando 165.5 fps y 4.5x
            // todo el dia, asi que su ausencia no es peligrosa por si sola.
            // Exigirla armaba el freno ahi y la corrida se quedaba sin ventanas.
            const int gf = patch_generation_flag(base, text, len);
            if (gf == 0)
                log_line("  bandera de generacion: no engancho (no frena nada)");
        }
        // The frame latency, pinned, behind mfg-pinlatency.txt.
        //
        // The base rate is not the average of the cadence, it is
        // refresh/(ceiling + 1): 2.25x, 2.50x and 2.75x all render at 55 = 165/3,
        // the same base 3.00x pays, and so present 124/138/151 against 2.00x's
        // 165. If the base followed the average instead, 2.25x would sit at
        // 73.6 and present 166 -- the whole throughput loss is that pinning.
        //
        // throttleFlipQueue reconfigures SetMaximumFrameLatency whenever
        // interpolation starts or stops, which is what sets the queue depth to
        // the deepest count in the cadence. This was measured before and
        // discarded because a 2.00x control then never started interpolation --
        // but that control ran on the 7x-fast block clock, the -3.7% frame
        // counter and the hook that miscounted presents, so it is not evidence
        // any more. Behind a flag so the control can say so again if it was
        // right the first time.
        if (g_pace_follow) {
            if (patch_pacer_count(text, len))
                log_line("  pacer waits on the frame's own count (mfg-pacefollow.txt)");
        }
        if (g_pin_latency) {
            if (patch_pin_frame_latency(text, len))
                log_line("  frame latency pinned (mfg-pinlatency.txt)");
        }
        return 1;
    }


    size_t found = 0, at = 0;
    for (size_t i = 0; i + 8 <= len; ++i) {
        if (text[i] != 0xFF || text[i + 1] != 0xC7) continue;   // inc edi
        if (text[i + 2] != 0x41 || text[i + 3] != 0x3B ||
            text[i + 4] != 0x7D || text[i + 5] != 0x04) continue; // cmp edi,[r13+4]
        if (text[i + 6] != 0x0F || text[i + 7] != 0x82) continue; // jb rel32
        ++found;
        at = i;
    }
    if (found != 1) {
        if (found > 1) log_num("  ! sub-frame loop is ambiguous, sites: ", (unsigned)found);
        return 0;
    }

    unsigned char *p = text + at + 2;
    DWORD old = 0;
    if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    p[0] = 0x83;        // cmp edi, imm8
    p[1] = 0xFF;
    p[2] = 1;           // NN -- one generated frame until we say otherwise
    p[3] = 0x90;        // nop, so the four bytes stay four bytes
    VirtualProtect(p, 4, old, &old);
    // Left writable deliberately: this byte is rewritten every frame, and
    // calling VirtualProtect that often would be both slow and pointless.
    DWORD ignored = 0;
    VirtualProtect(p + 2, 1, PAGE_EXECUTE_READWRITE, &ignored);
    sitio_add(g_imm_sitios, &g_imm_n, p + 2);

    // And the count the flip metering is programmed with.
    //
    //   44 8B CB       mov r9d, ebx
    //   4D 85 ED       test r13, r13
    //   74 0B          je ...
    //   45 8B 4D 00    mov r9d, dword ptr [r13]     <- the batch count
    //
    // becomes `push imm8 ; pop r9` -- 6A NN 41 59 -- which is also four bytes,
    // so nothing moves. That matters more than it sounds: with the metering
    // reading our byte, the count no longer has to be pushed through
    // slDLSSGSetOptions every frame, and it was that per-frame call that made
    // the plugin log 2646 "Repeated slDLSSGSetOptions() ... race condition
    // with Present()" warnings and then 167 "Couldn't lock the mutex on sync
    // present - will skip the present". Those skipped presents are the frame
    // rate collapsing; the cadence was never the problem.
    {
        size_t mfound = 0, mat = 0;
        for (size_t i = 0; i + 12 <= len; ++i) {
            if (text[i] != 0x44 || text[i+1] != 0x8B || text[i+2] != 0xCB) continue;
            if (text[i+3] != 0x4D || text[i+4] != 0x85 || text[i+5] != 0xED) continue;
            if (text[i+6] != 0x74) continue;
            if (text[i+8] != 0x45 || text[i+9] != 0x8B ||
                text[i+10] != 0x4D || text[i+11] != 0x00) continue;
            ++mfound;
            mat = i;
        }
        if (mfound == 1) {
            unsigned char *m = text + mat + 8;
            DWORD om = 0;
            if (VirtualProtect(m, 4, PAGE_EXECUTE_READWRITE, &om)) {
                m[0] = 0x6A;    // push imm8
                m[1] = 1;       // NN
                m[2] = 0x41;    // pop r9
                m[3] = 0x59;
                VirtualProtect(m, 4, om, &om);
                DWORD ig = 0;
                VirtualProtect(m + 1, 1, PAGE_EXECUTE_READWRITE, &ig);
                sitio_add(g_imm3_sitios, &g_imm3_n, m + 1);
                log_line("  metering count made writable too");
            }
        } else {
            log_num("  ! metering count site not unique, sites: ", (unsigned)mfound);
        }
    }

    // And the guard that decides whether the loop runs at all:
    //
    //   8B FB           mov edi, ebx
    //   41 39 5D 04     cmp dword ptr [r13+4], ebx     ; count <= i ?
    //   0F 86 rel32     jbe <past the loop>
    //
    // becomes `cmp ebx, NN` + nop with the branch inverted to `jae`, which is
    // the same test read the other way round. Without this the top would still
    // gate on the game's count while the bottom counted to ours -- and zero
    // generated frames, which is what any ratio under 2.0x needs, would be
    // unreachable.
    patch_generation_flag(base, text, len);
    // Off for a control: with it on, a plain 2.00x run at base 30 renders 30
    // and presents 30 -- no generated frames at all -- while sl.log fills with
    // 538 "Out of order frame - will skip the present" against ~30 presents.
    // This patch is the only thing that touches that comparison.
    if (flag_file(L"mfg-monoidx.txt")) patch_monotonic_index(base, text, len);
    // patch_index_count is NOT called. Making the present index agree with the
    // loop stops generation outright: a plain 2.00x control fell to 0.88x with
    // the render loop running free at 188 fps, the same failure as a bound
    // below the API count. The index must be built from the count the plugin
    // allocated for, not from the number of sub-frames actually produced.
    // patch_gate_frame_latency is NOT called either. Letting the bring-up
    // through and suppressing the reconfigurations afterwards makes it worse,
    // not better: dropped presents went from 79 to 165 and the rate from 35.7
    // to 31.0 fps. Skipping the DXGI call while the plugin believes the latency
    // changed leaves the two disagreeing, which is the same class of mistake as
    // patching one end of the sub-frame loop.
    //
    // Note also that the "SetMaximumFrameLatency changed from X to Y" line is
    // written before the call, so counting it measures the plugin's intent and
    // not whether the reconfiguration happened. It is not evidence either way.

    // patch_pin_frame_latency is deliberately NOT called, and now for a reason
    // that was measured on a working instrument.
    //
    // It was reopened because the base rate is not the average of the cadence,
    // it is refresh/(ceiling + 1): 2.25x, 2.50x and 2.75x all render at 55 =
    // 165/3, the base 3.00x pays, so they present 123/138/150 against 2.00x's
    // 164. Were the base the average, 2.25x would sit at 73.6 and present 166.
    // The whole throughput loss above 2.0x is that pinning, and
    // SetMaximumFrameLatency reconfiguring on every start and stop of
    // interpolation was the named suspect.
    //
    // Retested behind mfg-pinlatency.txt. It is worse than the note it replaces
    // said: the patch applies -- the log prints "frame latency pinned" -- and
    // the run then produces no measurement window at all, three times out of
    // three, at 2.00x, 2.25x and 2.75x alike. Not "interpolation never starts";
    // the sample stops. The unpinned arms of the same three pairs ran normally
    // in between.
    //
    // So the base stays pinned to the ceiling count, and a fractional ratio
    // above 2.0x costs the base of the integer above it while returning less
    // than that integer's multiplier. On a 165 Hz display that is measured and
    // it is a loss. Kept in the file because the site is right and the next
    // idea will want it.

    // The validation, first: without it nothing below 2.0x is expressible.
    //
    //   44 8B 47 24   mov r8d, dword ptr [rdi+0x24]   ; the count from options
    //   41 83 F8 01   cmp r8d, 1                      ; at least one?
    //   0F 83 rel32   jae <carry on>
    //
    // The immediate goes to zero, so the unsigned test always passes and eOn
    // may carry a count of nothing. That matters because the alternative --
    // sending eOff for those frames -- costs a state transition each time:
    // 442 of them in one run against ten at 2.5x, and the result was worse
    // than not trying. With zero accepted, generation stays on and the frame
    // simply produces nothing.
    //
    // One byte; the branch and everything after it stay where they are.
    // Unique across the corpus from 2.8.0 through 2.13.
    {
        size_t vfound = 0, vat = 0;
        for (size_t i = 0; i + 10 <= len; ++i) {
            if (text[i] != 0x44 || text[i + 1] != 0x8B ||
                text[i + 2] != 0x47 || text[i + 3] != 0x24) continue;
            if (text[i + 4] != 0x41 || text[i + 5] != 0x83 ||
                text[i + 6] != 0xF8 || text[i + 7] != 0x01) continue;
            if (text[i + 8] != 0x0F || text[i + 9] != 0x83) continue;
            ++vfound;
            vat = i;
        }
        if (vfound == 1) {
            unsigned char *v = text + vat + 7;
            DWORD ov = 0;
            if (VirtualProtect(v, 1, PAGE_EXECUTE_READWRITE, &ov)) {
                *v = 0;
                VirtualProtect(v, 1, ov, &ov);
                log_line("  zero generated frames accepted (ratios under 2x)");
            }
        } else {
            log_num("  ! zero-count validation not unique, sites: ", (unsigned)vfound);
        }
    }

    size_t g_found = 0, g_at = 0;
    for (size_t i = 0; i + 10 <= len; ++i) {
        if (text[i] != 0x8B || text[i + 1] != 0xFB) continue;          // mov edi, ebx
        if (text[i + 2] != 0x41 || text[i + 3] != 0x39 ||
            text[i + 4] != 0x5D || text[i + 5] != 0x04) continue;      // cmp [r13+4], ebx
        if (text[i + 6] != 0x0F || text[i + 7] != 0x86) continue;      // jbe rel32
        ++g_found;
        g_at = i;
    }
    if (g_found == 1) {
        unsigned char *q = text + g_at + 2;
        DWORD o2 = 0;
        if (VirtualProtect(q, 6, PAGE_EXECUTE_READWRITE, &o2)) {
            q[0] = 0x83;    // cmp ebx, imm8
            q[1] = 0xFB;
            q[2] = 1;       // NN
            q[3] = 0x90;    // nop
            q[5] = 0x83;    // jbe -> jae   (0F 86 -> 0F 83)
            VirtualProtect(q, 6, o2, &o2);
            DWORD ig = 0;
            VirtualProtect(q + 2, 1, PAGE_EXECUTE_READWRITE, &ig);
            sitio_add(g_imm2_sitios, &g_imm2_n, q + 2);
        }
    } else {
        log_num("  ! loop entry guard not unique, sites: ", (unsigned)g_found);
    }
    return 1;
}

// N generated frames on the next batch. Zero is not expressible here -- the
// loop is a do-while, so its body has already run once by the time the bound
// is tested, and one generated frame is the floor.
// What is actually in force, which is not the same as g_force_generated once
// the byte is being written directly: a mode picked by hand leaves that
// variable behind, and dividing the measured rate by a stale multiplier makes
// the base look far lower than it is -- which asks for more generation, which
// makes it look lower still.
static volatile LONG g_count_live = 1;

// One more than asked, on the loop bound only.
//
// With generation verified working, both fractional ratios come out exactly one
// generated frame short per frame: 2.50x (cadence 1,2) presents 22.5 against a
// base of 15, which is 1.5 generated on average rather than 2.5, and 1.50x
// (cadence 0,1) presents 15.3, i.e. none. The API is told the right number in
// both cases, and the cadence logs the ratio asked for, so the shortfall is in
// the loop -- which is the one thing we rewrote. Turning its do-while into a
// for with an entry guard costs the iteration the original ran before testing.
static LONG loop_bound_for(LONG n) {
    // The bound is one more than the frames wanted, because turning the
    // original do-while into a for with an entry guard costs the iteration it
    // used to run before testing. Without this every ratio came out one
    // generated frame short and the pacing fell apart: 1.50x had a p99 of
    // 366 ms and eleven hitches, which became 38 ms and none.
    //
    // Zero stays zero. Feeding n+1 there generates a whole frame where the
    // cadence asked for none, which is what turned 1.50x into a flat 2.00x --
    // 30 presented against the 22.5 the ratio calls for.
    // Never below the bound a single generated frame needs.
    //
    // Zero in the loop is what wrecks the pacing, not merely what skips a
    // frame: 1.50x with a zero in the cadence gives a p99 of 366 ms and eleven
    // hitches, and the same run with every count raised gives 38 ms and none.
    // So the loop always runs, and the fraction is carried by the metering
    // count below, which is the other byte we own.
    // The bound counts sub-frames, not generated frames: 2 yields one
    // generated frame, 3 yields two. So one -- not zero -- is how "generate
    // nothing" is said, and the loop body still runs its single pass.
    //
    // Zero was used before, and zero is skipped outright by the entry guard we
    // added. That pass appears to be where the real frame is dealt with:
    // 1.50x with a zero in the cadence presented 15.6 from 15 rendered, an
    // effective 1.04x, with a p99 of 366 ms. This tries the one value between
    // the two that was never tested.
    // Never zero, and never one.
    //
    // Measured, with the present cap raised so the multiplier is observable
    // (base 30, rendered ~89):
    //
    //   bound 0  ->  0.89x, p99 66 ms, 39 hitches   -- destructive: skipping
    //                the loop loses the real frame's present too
    //   bound 1  ->  1.85x   (one generated)
    //   bound 2  ->  1.85x   (one generated)
    //   bound 3  ->  3.0x    (two generated)
    //
    // So no value of this byte expresses "generate nothing": the floor is one
    // generated frame per rendered frame, i.e. 2.0x. Ratios below that are not
    // expressible here and are held at 2.0x rather than allowed to lose frames.
    // Never above what the API allocated for, never zero.
    //
    // Proved by construction: a bound of 5 with the API told 1 crashes the
    // sample with 0xC0000005, so the loop really does drive the iteration count
    // and writing past the allocation is fatal. A bound of 0 measures 0.89x, so
    // it can reduce as well -- but it reduces by losing the real frame's
    // present, not by generating one fewer.
    return n <= 0 ? 2 : n + 1;
}

static void set_count_now(LONG n) {
    if (g_wic_mode) {
        // n is generated frames, which is what the field holds: the multiplier
        // is n + 1.
        if (g_wic_n > 0) {
            if (n < 0) n = 0;
            if (n > 5) n = 5;
            sitio_write(g_wic_sitios, g_wic_n, (unsigned char)n);
            // The pacer waits on its own copy. Without this it keeps waiting
            // for the ceiling the API was told, which is the whole throughput
            // loss above 2.0x.
            if (g_pace_count != nullptr) *g_pace_count = (unsigned char)n;
            // Nothing generated means the ordinary present path -- the one that
            // actually presents the real frame.
            if (g_gen_flag != nullptr) *g_gen_flag = (unsigned char)(n > 0 ? 1 : 0);
            g_count_live = n;
        }
        return;
    }
    if (g_imm_n == 0) return;
    if (n < 0) n = 0;                    // zero is legal: the loop is skipped
    if (n > 5) n = 5;                    // the plugin's own ceiling
    // The guard first, so a frame can never see a raised bound with the old
    // gate still shut, or the reverse.
    sitio_write(g_imm2_sitios, g_imm2_n, (unsigned char)loop_bound_for(n));
    // The same number as the loop, zero included. Clamping this to one "just
    // in case" was the whole mismatch coming back: the metering programmed a
    // batch for one generated frame while the loop produced none, and the
    // symptom moved from a stalled present to Reflex falling behind --
    // "sl.reflex must be enabled and active 11969 != 17669", one frame counter
    // frozen while the other ran on.
    // The metering count does not gate anything: with the loop held at two and
    // this carrying the cadence, 1.50x still presented 30 rather than 22.5.
    // Whatever the loop produces is presented regardless of this byte, so the
    // fraction cannot be moved here. Kept in agreement with the loop.
    sitio_write(g_imm3_sitios, g_imm3_n, (unsigned char)n);
    // Held on. Measured both ways at 1.50x, base 80: following the count gives
    // 20.6 fps and 361 state changes, holding it on gives 39.3 fps and one.
    // Neither reaches the 120 the ratio asks for -- the rate tracks how often
    // the count is zero either way (19.3 / 39.3 / 54.6 / 80.0 fps at 1.25 /
    // 1.50 / 1.75 / 2.00x, i.e. 1/4, 1/2, 3/4, 1 of the base) -- but holding it
    // on is twice as good and costs nothing above 2.0x, where the count never
    // reaches zero at all.
    if (g_gen_flag != nullptr) *g_gen_flag = 1;
    // Bring-up is over well inside a couple of seconds; after that a
    // reconfiguration can only be the churn this is here to stop.
    static LONG settled = 0;
    if (g_lat_allow != nullptr && settled < 400 && ++settled == 400) {
        *g_lat_allow = 0;
        log_line("fractional: frame latency now left alone");
    }
    sitio_write(g_imm_sitios, g_imm_n, (unsigned char)loop_bound_for(n));
    g_count_live = n;
}


// Smoothed over a window rather than taken frame to frame. A single frame
// time is noisy enough that the multiplier would change on a passing hitch,
// and every change makes Streamline restart interpolation -- the native
// controller did that 42 times in one run, twice within seven milliseconds,
// and that churn is what a player feels as stutter.
static double g_token_dt = 0.0;       // the averaged frame time, in seconds
static double g_last_dt = 0.0;        // and the most recent one, for the spread
static double g_win_time = 0.0;       // elapsed in the current counting window
static int    g_win_frames = 0;       // rendered frames in it
static double g_rendered_fps = 0.0;   // frames / elapsed, unbiased
static double g_rate_lo = 0.0;        // last cycle's rate in each state, kept
static double g_rate_hi = 0.0;        // across the reset that clears the counters
static int    g_lo_frames_seen = 0;   // rendered frames while the low count ran
static int    g_hi_frames_seen = 0;   // and while the high one did
static volatile LONG g_present_count = 0;   // presents seen at the swap chain
static long long g_pres_qpc = 0;            // when the last present went out
static volatile LONG g_last_change_pres = 0; // present index at the last count change
static int g_hitch_near = 0;                 // hitches within 4 presents of one
static int g_hitch_far = 0;                  // and the rest
static double g_lat_sum = 0.0;               // queued presents, summed
static int g_lat_n = 0, g_lat_max = 0;
static int g_all_n = 0, g_all_bad = 0;       // every present, for the summary
static double g_all_ms = 0.0;
static int g_near_n = 0, g_near_bad = 0;     // presents within 8 of a change
static int g_far_n = 0, g_far_bad = 0;       // and the rest
static double g_pres_ms_sum = 0.0;
static double g_pres_ms_max = 0.0;
static int    g_pres_n = 0;
static int    g_pres_hitch = 0;             // intervals over 33 ms
static int    g_pres_bucket[6] = { 0, 0, 0, 0, 0, 0 };
static double g_lo_time = 0.0;
static double g_hi_time = 0.0;
static double g_token_fps = 0.0;      // and the rate that follows from it
static double g_token_dt_fast = 0.0;  // la misma senal, para el controlador
static double g_base_fps = 0.0;       // that, divided by the multiplier in force
static long long g_last_token_qpc = 0;

static int g_clamp_latency = 0;        // mfg-clamplatency.txt, 0 = off
static volatile LONG g_smfl_calls = 0;
static volatile LONG g_token_calls = 0;
static volatile LONG g_frames_gated = 0;
static bool g_novsync = false;        // mfg-novsync.txt: diagnostic
static int  g_slow_frame_us = 0;      // mfg-slowframe.txt, en microsegundos
// Con tres numeros en mfg-slowframe.txt la carga alterna entre el primero y
// el segundo cada N milisegundos, y la base del banco se mueve de verdad
// durante la corrida. Sin esto no hay forma de probar un controlador: una
// escena de carga constante no distingue a uno que ajusta de uno que no
// hace nada. Escalon y no rampa a proposito -- el que aguanta un escalon
// aguanta una rampa, y en los datos se ve donde empieza.
static int  g_slow_frame_us2 = 0;
static int  g_slow_step_ms = 0;
static int  g_jitter_pct = 0;         // mfg-jitter.txt, porcentaje
static double g_present_block_us = 0.0;   // blocked inside Present, per window
static double g_token_block_us = 0.0;     // blocked inside slGetNewFrameToken
static volatile LONG g_rt_present_count = 0;
// Forma del salto de indice entre llamadas consecutivas al token, para saber si
// el gate se rompe por intercalado de hilos.
static volatile LONG g_paso_igual = 0, g_paso_uno = 0, g_paso_salta = 0, g_paso_atras = 0;
// The swap chain, kept so the runtime's own present counter can be sampled
// from anywhere -- specifically from the frame-token hook, once per rendered
// frame, off the present path entirely.
static IDXGISwapChain *g_swapchain = nullptr;

static volatile LONG g_present_mismatch = 0;   // windows where we disagree
// Gap histogram, in the token hook. Buckets in milliseconds:
//   0: <0.5   1: 0.5-2   2: 2-4   3: 4-8   4: 8-16   5: >=16
static int g_gap_hist[6] = {0,0,0,0,0,0};
static int g_raw_calls = 0;
static void note_rendered_frame(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (g_last_token_qpc != 0 && g_qpc_freq > 0) {
        const double dt = (double)(t.QuadPart - g_last_token_qpc) / (double)g_qpc_freq;
        // Frames counted over elapsed time, instead of an average of the gaps
        // that survived a filter.
        //
        // The game calls slGetNewFrameToken about seven times per rendered
        // frame -- 3780 calls against 540 frames in a fixed-length run -- in a
        // burst, and only one of those gaps clears the 2 ms floor below.
        // Averaging the survivors converges not to the frame period P but to P
        // minus the span of the burst, so the reported rate is always high: at
        // 2.25x it read 58 where the ratio implies 54.5, a burst span near
        // 1.1 ms. That six percent is the whole of the "0.94" that appeared in
        // every measurement and was taken for a systematic cost of generation.
        //
        // Counting one frame per burst and dividing by the total elapsed time,
        // short gaps included, has no such bias: every microsecond of the
        // window is in the denominator and every frame in the numerator.
        if (dt > 0.0 && dt < 0.200) {
            g_win_time += dt;
            ++g_raw_calls;
            g_gap_hist[dt < 0.0005 ? 0 : dt < 0.002 ? 1 : dt < 0.004 ? 2
                       : dt < 0.008 ? 3 : dt < 0.016 ? 4 : 5]++;
            // Every call is a frame now, so there is nothing left to filter.
            ++g_win_frames;
            // A rendered frame is the transition from short gaps to a long one,
            // not every long gap on its own.
            //
            // With generation off the counted multiplier must be exactly 1.00:
            // each rendered frame is presented once and nothing is added. It
            // read 0.963 -- 43.6 presents per 45 counted frames -- and the
            // presents are the honest half, so the frame count was 3.7% high.
            // A burst that happens to contain two gaps over the threshold was
            // counted twice.
            //
            // Edge-detecting on g_last_dt does not fix this: dt differs on every
            // call inside a burst, so the "changed" test fires constantly. The
            // burst boundary is the long gap *after short ones*, which is what
            // this tests.
            // A short window on purpose: with the API count alternating in
            // blocks, the rendered rate swings between the two multipliers, and
            // a window longer than a block averages the swing away. Three
            // quarters of a second is short enough to show the steps and long
            // enough that frames/elapsed is still steady.
            if (g_win_frames >= 45 && g_win_time > 0.0) {
                const double win_elapsed = g_win_time;
                g_rendered_fps = (double)g_win_frames / g_win_time;
                g_win_frames = 0;
                g_win_time = 0.0;
                // Silenced by mfg-quiet.txt. log_line opens, seeks, writes and
                // closes the file for every line, on the render thread, and the
                // alternating arm emits more lines than the integer one -- so
                // part of the 10% throughput gap could be this rather than the
                // cadence. Running both arms silent settles which.
                if (!g_quiet)
                log_num("measured: rendered fps ", (unsigned)(int)(g_rendered_fps + 0.5));
                // Las dos bases, lado a lado, una vez por ventana. La de
                // Reflex es el contador del driver; la de arriba cuenta
                // llamadas al token y depende de como llame el juego. En el
                // banco y en GTA V tienen que coincidir; donde se separen,
                // la que miente es la del token.
                if (!g_quiet)
                log_num("  base por Reflex ", (unsigned)(int)(g_rfx_base + 0.5));
                {
                    // Frames that left the swap chain over frames the game
                    // rendered, both counted over the same window of 45
                    // rendered frames. Neither number is chosen by us.
                    // The multiplier is measured with PresentCount, a counter
                    // this file does not maintain, sampled once per rendered
                    // frame from the token hook.
                    //
                    // Counting in the Present hook cannot be made reliable
                    // either way. A byte detour catches every caller including
                    // DLSS-G's pacer thread, but shares its bytes with Steam's
                    // overlay and gets displaced -- one 1.50x run read 45
                    // presents per window against the runtime's 66. Swapping
                    // the vtable slot is stable and is this project's rule, but
                    // the pacer cached the function pointer before we swapped,
                    // so it never comes through us: a 2.00x control then read
                    // 1.000 while rendering at 83, having missed exactly the
                    // generated half.
                    //
                    // PresentCount is a running total, so sampling it at any
                    // moment gives the true cumulative count no matter which
                    // calls we intercept. Our own hook count stays as the
                    // cross-check, not as the measurement.
                    // Sampled in the present hook, never here. Calling
                    // GetFrameStatistics from the frame-token thread crashed
                    // the sample within ten seconds on four runs out of four --
                    // one window logged and gone. It is not needed either: the
                    // app's own present still comes through the swapped slot
                    // once per frame, and PresentCount is a running total, so
                    // sampling it there already includes the pacer's presents
                    // that never reach us.
                    static LONG last_pres = 0;
                    static LONG last_hook = 0;
                    const LONG now = g_rt_present_count;
                    const LONG dp = now - last_pres;
                    last_pres = now;
                    const LONG hook_now = g_present_count;
                    const LONG rt_dp_unused = hook_now - last_hook;
                    last_hook = hook_now;
                    const LONG rt_dp = rt_dp_unused;   // the hook's count
                    // Say it out loud when the two disagree. The comparison
                    // was already being logged and no analysis script read it,
                    // so a run whose present hook had been displaced still got
                    // reported as a result.
                    if (rt_dp > 0 && (dp - rt_dp > 2 || rt_dp - dp > 2)) {
                        InterlockedIncrement(&g_present_mismatch);
                        if (!g_quiet)
                            log_num("  WARNING present count disagrees, windows so far ",
                                    (unsigned)g_present_mismatch);
                    }
                    if (dp > 0 && !g_quiet) {
                        // Named for which counter each one is, because the
                        // string is what every analysis script keys on and the
                        // meaning of "presents this window" changed identity
                        // between two builds without a rename -- it was the
                        // hook's count, then it was PresentCount. That is the
                        // silent instrument swap this project keeps warning
                        // itself about.
                        log_num("  runtime PresentCount this window ", (unsigned)dp);
                        log_num("  our hook count this window ",
                                (unsigned)(rt_dp > 0 ? rt_dp : 0));
                        log_num("  raw token calls ", (unsigned)g_raw_calls);
                        // Against the window's own elapsed time, so the ratio
                        // says what fraction of the producer's life is spent
                        // waiting inside Present.
                        log_num("  blocked in the token call, ms ",
                                (unsigned)(g_token_block_us / 1000.0));
                        g_token_block_us = 0.0;
                        log_num("  blocked in Present, ms ",
                                (unsigned)(g_present_block_us / 1000.0));
                        log_num("  window elapsed, ms ",
                                (unsigned)(win_elapsed * 1000.0));
                        // Alimenta el HUD con lo mismo que se registra, no con
                        // una segunda cuenta: dos medidas del mismo numero se
                        // separan y despues no se sabe cual creer.
                        if (win_elapsed > 0.0)
                            g_hud_fps_x10 = (LONG)(dp * 10.0 / win_elapsed + 0.5);
                        // El controlador de DYNAMIC come de aca por la misma
                        // razon: la base y lo presentado ya estan medidos con
                        // el instrumento honesto, y una segunda cuenta propia
                        // seria un numero mas que puede discrepar.
                        // La MISMA base con la que planifica dyn_apply, no
                        // g_rendered_fps. El sesgo se aprende comparando lo
                        // pedido contra lo entregado, y lo entregado sale de
                        // dividir por esta base: si las dos funciones dividen
                        // por numeros distintos, el sesgo corrige una
                        // discrepancia que no existe.
                        //
                        // En Cyberpunk g_rendered_fps da 113 donde la base real
                        // es 39, porque el gate de frames dispara unas tres
                        // veces por frame renderizado. Con eso, dyn_control veia
                        // 216/113 = 1.9 entregado contra 5.2 pedido, concluia
                        // que faltaba muchisimo y empujaba el sesgo a su tope de
                        // 1.25. Medido: ratio crudo 4.15, sesgo 1.25, pedido
                        // 5.19, presentadas 216 contra un objetivo de 165.
                        if (win_elapsed > 0.0)
                            dyn_control(g_ctrl_fps > 0.0 ? g_ctrl_fps : g_base_fps,
                                        dp / win_elapsed);
                        // A1 necesita saber si la interpolacion arranco. La
                        // base honesta es la de Reflex; si lo presentado la
                        // supera con holgura, esta generando.
                        if (win_elapsed > 0.0 && g_rfx_base > 1.0)
                            g_interp_on = (dp / win_elapsed) > g_rfx_base * 1.3 ? 1 : 0;
                        g_present_block_us = 0.0;
                        if (g_clamp_latency > 0)
                            log_num("  SetMaximumFrameLatency calls so far ",
                                    (unsigned)g_smfl_calls);
                        log_num("  hook calls total ", (unsigned)g_token_calls);
                        log_num("  frames past the gate ", (unsigned)g_frames_gated);
                        log_num("    salto igual ", (unsigned)g_paso_igual);
                        log_num("    salto +1 ", (unsigned)g_paso_uno);
                        log_num("    salto adelante ", (unsigned)g_paso_salta);
                        log_num("    salto atras ", (unsigned)g_paso_atras);
                        {
                            static const char *kG[6] = {
                                "    gap <0.5ms ", "    gap 0.5-2ms ",
                                "    gap 2-4ms ", "    gap 4-8ms ",
                                "    gap 8-16ms ", "    gap >=16ms " };
                            for (int i = 0; i < 6; ++i)
                                if (g_gap_hist[i] > 0) log_num(kG[i], (unsigned)g_gap_hist[i]);
                        }
                        g_raw_calls = 0;
                        for (int i = 0; i < 6; ++i) g_gap_hist[i] = 0;
                        if (g_pres_n > 0) {
                            log_num("  present ms avg x10 ",
                                    (unsigned)(g_pres_ms_sum / (double)g_pres_n * 10.0));
                            log_num("  present ms max x10 ", (unsigned)(g_pres_ms_max * 10.0));
                            log_num("  hitches over 33ms ", (unsigned)g_pres_hitch);
                            // Dos datos que separan "el juego estaba en el
                            // menu" de "se apago solo mientras jugabas".
                            if (g_rfx_n > 0) {
                                log_num("  latency sim to gpu end us ",
                                        (unsigned)(g_rfx_gpu / g_rfx_n));
                                log_num("  latency sim to driver end us ",
                                        (unsigned)(g_rfx_drv / g_rfx_n));
                                g_hud_lat_us = (LONG)(g_rfx_drv / g_rfx_n);
                                log_num("  reflex frame time us ",
                                        (unsigned)(g_rfx_ft / g_rfx_n));
                                log_num("    driver latency min us ",
                                        (unsigned)g_rfx_min);
                                log_num("    driver latency max us ",
                                        (unsigned)g_rfx_max);
                                log_num("    frames reported ", (unsigned)g_rfx_n);
                                g_rfx_gpu = 0.0; g_rfx_drv = 0.0;
                                g_rfx_ft = 0.0; g_rfx_n = 0;
                                g_rfx_min = 0xFFFFFFFFu; g_rfx_max = 0;
                            }
                            // NO CONFIABLE en Cyberpunk. Dio 0 en 1406 de 1413
                            // ventanas con el juego al frente y Streamline sin
                            // una sola queja de foco en toda la corrida. Se
                            // probaron dos arreglos y los dos fallaron: seguir
                            // la ventana vigente en vez de la primera (el juego
                            // crea un solo swapchain, asi que no cambia nada) y
                            // comparar contra GetAncestor(GA_ROOT) por si el
                            // swapchain colgaba de una hija. El HWND esta
                            // capturado, no es nulo.
                            //
                            // No se toca mas. Para saber si el foco apago la
                            // generacion, la autoridad es la queja del plugin en
                            // sl.log ("DLSS-G disabled: window not focused"), que
                            // es la que de verdad la apaga. Este numero se deja
                            // porque en GTA V si sigue al foco, pero NO se puede
                            // descartar una ventana de Cyberpunk por el.
                            log_num("  game window in front (1 = yes, NO CONFIABLE en cp2077) ",
                                    (unsigned)(g_game_hwnd != nullptr &&
                                               GetForegroundWindow() ==
                                                   GetAncestor(g_game_hwnd, GA_ROOT)
                                               ? 1 : 0));
                            log_num("  the game itself last asked for mode ",
                                    (unsigned)g_cap_mode);
                            log_num("    with count ", (unsigned)g_cap_cnt);
                            if (g_near_n + g_far_n > 0) {
                                log_num("    off-refresh near a change x1000 ",
                                        (unsigned)(g_near_n ? (unsigned long long)g_near_bad * 1000ULL / (unsigned)g_near_n : 0));
                                log_num("      of presents ", (unsigned)g_near_n);
                                log_num("    off-refresh away x1000 ",
                                        (unsigned)(g_far_n ? (unsigned long long)g_far_bad * 1000ULL / (unsigned)g_far_n : 0));
                                log_num("      of presents ", (unsigned)g_far_n);
                                g_near_n = 0; g_near_bad = 0; g_far_n = 0; g_far_bad = 0;
                            }
                            if (g_hitch_near + g_hitch_far > 0) {
                                log_num("    at a count change ", (unsigned)g_hitch_near);
                                log_num("    away from one ", (unsigned)g_hitch_far);
                                g_hitch_near = 0;
                                g_hitch_far = 0;
                            }
                            for (int b = 0; b < 6; ++b)
                                if (g_pres_bucket[b] > 0) {
                                    log_num("    bucket ", (unsigned)b);
                                    log_num("      count ", (unsigned)g_pres_bucket[b]);
                                }
                            g_pres_ms_sum = 0.0; g_pres_ms_max = 0.0;
                            g_pres_n = 0; g_pres_hitch = 0;
                            for (int b = 0; b < 6; ++b) g_pres_bucket[b] = 0;
                        }
                        log_num("  counted multiplier x100 ",
                                (unsigned)((unsigned long long)dp * 100ULL / 45ULL));
                    }
                }
            }
        }
        // 2 ms to 200 ms. Outside that it is a load screen, a breakpoint or a
        // wrapped counter, and feeding it to the average would move the
        // multiplier for a reason that has nothing to do with the game.
        if (dt > 0.002 && dt < 0.200) {
            // The frame *times* are averaged, and the rate comes from the
            // average -- not the other way round. Averaging 1/dt overstates
            // the rate whenever the intervals are uneven, which is exactly
            // what frames look like with generation running: alternating 5 ms
            // and 20 ms is 12.5 ms on average, or 80 fps, but averaging the
            // reciprocals gives (200+50)/2 = 125. That is the whole of the gap
            // between the 118 this reported and the 80 the player was reading
            // off the game.
            g_last_dt = dt;
            g_token_dt = g_token_dt <= 0.0 ? dt : g_token_dt * 0.94 + dt * 0.06;
            // Una segunda estimacion, cuatro veces mas rapida, solo para el
            // controlador. La de arriba tiene constante de ~16 frames -- 275 ms
            // a base 58 -- y ese retardo es exactamente el sobrepaso medido
            // despues de cada escalon: 11 ventanas entre 150 y 178 con objetivo
            // 140, todas del lado alto. No se toca la original porque la
            // comparten el metering y dynamic_want.
            // La base del controlador se calcula frames/tiempo sobre los
            // ultimos 8, que es la misma definicion que usa la ventana de
            // medicion. Antes se tomaba 1/promedio(dt), y promediar intervalos
            // para despues invertir NO da la tasa media: por Jensen
            // 1/E[dt] <= E[1/dt], asi que con tiempos de frame que varian la
            // base salia sistematicamente baja, el controlador pedia de mas y
            // las presentadas quedaban 2% arriba del objetivo. Ese era el sesgo
            // residual que no se explicaba.
            // Solo si Reflex no lo esta alimentando: su intervalo es el del
            // driver y no depende de como llame el juego al token.
            if (!ctrl_fed_by_reflex()) ctrl_feed_dt(dt);
            if (g_token_dt_fast <= 0.0) {
                g_token_dt_fast = dt;
            } else {
                // Deteccion de escalon. Suavizar es correcto para el ruido y
                // equivocado para un cambio real: con constante de 4 frames el
                // sobrepaso dura ~4 de los 45 de una ventana, y 4 frames a 220
                // contra 41 a 140 promedian 147 -- justo afuera del +-5%. Dos
                // frames seguidos lejos del estimado y cerca entre si no son
                // ruido, son otra carga, y ahi conviene saltar de una.
                static double prev_dt = 0.0;
                const double lo = g_token_dt_fast * 0.75;
                const double hi = g_token_dt_fast * 1.33;
                const bool lejos = dt < lo || dt > hi;
                const bool igual = prev_dt > 0.0 &&
                                   dt > prev_dt * 0.88 && dt < prev_dt * 1.12;
                if (lejos && igual) g_token_dt_fast = dt;
                else g_token_dt_fast = g_token_dt_fast * 0.75 + dt * 0.25;
                prev_dt = dt;
            }
            g_token_fps = g_token_dt > 0.0 ? 1.0 / g_token_dt : 0.0;
            // Divided by the multiplier in force, because the interval turned
            // out to track the *presented* rate and not the rendered one. The
            // give-away was in the log: at 2X the base read 102 where it had
            // read 44 with generation off, and the controller then decided it
            // no longer needed generation, which dropped the reading back to
            // 46 and started the whole thing again. The measurement has to be
            // independent of the decision, or the decision feeds itself.
            //
            // Both numbers are logged, so the next run either confirms this or
            // shows it was the wrong correction -- the reading was assumed to
            // be the rendered rate once already.
            g_base_fps = g_token_fps;
            // Reflex primero: su contador es del driver y no depende de cuantas
            // veces llame el juego al token. El camino viejo queda de reserva
            // para cuando no haya informe -- sin Reflex, o con el marcador
            // congelado como en el menu de GTA V.
            // Sin bypass: el estimador es el mismo de siempre, con su
            // correccion de Jensen y su deteccion de escalon. Lo unico que
            // cambio es que ahora lo alimenta Reflex y no el conteo de llamadas.
            dyn_apply(g_ctrl_fps > 0.0 ? g_ctrl_fps : g_base_fps);
            if (g_probe_left > 0) {
                const LONG pc = g_rt_present_count;
                LONG d = pc - g_probe_pc0;
                if (d < 0) d = 0;
                if (d > 255) d = 255;
                g_probe[g_probe_i++] = (unsigned char)d;
                g_probe_pc0 = pc;
                if (--g_probe_left == 0) {
                    g_probe_done = true;
                    char line[96];
                    int k = 0;
                    const char *pre = "probe: presents per frame after the write:";
                    while (pre[k] != 0) { line[k] = pre[k]; ++k; }
                    for (int q = 0; q < 16; ++q) {
                        line[k++] = ' ';
                        const int v = g_probe[q];
                        if (v >= 10) line[k++] = (char)('0' + (v / 10) % 10);
                        line[k++] = (char)('0' + v % 10);
                    }
                    line[k] = 0;
                    log_line(line);
                }
            }
        }
    }
    g_last_token_qpc = t.QuadPart;
}

// How many generated frames it would take to reach the target from the rate
// the game is actually rendering at, with the ceiling the plugin gave us.
static LONG dynamic_want(void) {
    if (g_base_fps <= 1.0) return g_force_generated;      // nothing measured yet
    const LONG target = g_dyn_target;
    // Zero is AUTO, which means the display: the same thing the native mode
    // treats as its default.
    double want_fps = (double)target;
    if (target <= 0) {
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        want_fps = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) &&
                   dm.dmDisplayFrequency > 1 ? (double)dm.dmDisplayFrequency : 120.0;
    }
    LONG n = (LONG)(want_fps / g_base_fps + 0.5) - 1;     // presented / rendered
    if (n < 0) n = 0;
    const LONG cap = g_frames_max > 0 ? g_frames_max : 3; // 3 = 4x, the safe floor
    if (n > cap) n = cap;
    return n;
}

// Changed only when the answer has been the same for a while and differs from
// what is in force. Without the dwell, a base rate sitting between two
// multipliers would flip every few frames for as long as the player stood
// there.
// Superseded by fractional_tick, which does the same job without rounding to
// a whole multiplier. Kept only so the integer path is one edit away if the
// immediate patch ever fails to apply.
// CUSTOM y DYNAMIC usan el mismo planificador: la unica diferencia es quien
// escribe el ratio. En CUSTOM lo escribe la persona; en DYNAMIC, el
// controlador. Un helper en vez de repetir la comparacion en cada sitio,
// que es como se cuelan las ramas olvidadas.
static inline bool sel_is_frac(void) {
    return g_force_sel == kSelDynamic || g_force_sel == kSelDynFuture;
}

// Corre una vez por ventana de medicion, con la base y lo presentado ya
// medidos por el instrumento honesto. No corre por frame a proposito: cada
// cambio de ratio es una escritura de opciones, y escribir de mas ya causo
// apagones en este proyecto.
// El sesgo de entrega, aprendido despacio y solo en ventanas estables. Es lo
// unico que se realimenta: realimentar tambien la base oscilo -- 18% de
// ventanas en banda contra 40% sin realimentar nada.
// Una ganancia por tramo de ratio, no una sola. El sesgo de entrega depende
// del punto de operacion y esta medido: pidiendo 3.75 el planificador entrega
// 3.78 (sesgo 0.995) y pidiendo 2.33 entrega 2.47 (sesgo 0.941, un +6%). Un
// escalar unico converge al promedio, 0.977, que deja +3% arriba y -2.5% abajo
// -- justo la forma del residuo que quedaba.
static double g_dyn_bias[6] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };

static inline int dyn_bucket(double ratio) {
    int b = (int)ratio;
    if (b < 2) b = 2;
    if (b > 5) b = 5;
    return b;
}
// El ratio pedido, promediado sobre la ventana. La ganancia compara pedido
// contra entregado, y con el adelanto corriendo por frame el pedido se mueve
// unas tres veces por ventana: tomar el valor del final contra el promedio
// entregado hacia que la ganancia se paseara hasta 1.07 -- un exceso fijo de
// +5 a +10% en los tramos estables. Bloquear el aprendizaje en esas ventanas
// tampoco sirve: casi ninguna califica, la ganancia se queda en 1.0 y el
// exceso de entrega real de ~4% queda sin corregir, con la mediana en 145.
// Promediar el pedido es lo unico que compara los dos numeros sobre el mismo
// periodo.
static double g_dyn_asked_sum = 0.0;
static LONG g_dyn_asked_n = 0;

static void dyn_control(double base_fps, double presented_fps) {
    if (g_force_sel != kSelDynFuture) return;
    if (base_fps <= 1.0 || presented_fps <= 1.0) return;
    static double last_base = 0.0;
    const bool stable = last_base > 1.0 &&
                        (base_fps > last_base ? base_fps - last_base
                                              : last_base - base_fps) < 4.0;
    last_base = base_fps;
    const double asked_avg = g_dyn_asked_n > 0
                           ? g_dyn_asked_sum / (double)g_dyn_asked_n : 0.0;
    g_dyn_asked_sum = 0.0;
    g_dyn_asked_n = 0;
    // Se probo exigir dos ventanas estables seguidas antes de aprender, por si
    // la ventana posterior a un escalon ensuciaba la ganancia: la mediana quedo
    // en 143 igual y el resultado en 64% contra 66%, o sea nada. El sesgo
    // residual de +2% no viene de ahi y sigue sin explicacion.
    if (!stable) return;
    if (asked_avg < 2.0) return;
    const double delivered = presented_fps / base_fps;
    if (delivered <= 0.5) return;
    // Se registran los dos para poder ver si el sesgo depende del punto de
    // operacion: un solo escalar no puede corregir a ratio 2.4 y a 3.8 a la vez
    // si el planificador se desvia distinto en cada uno.
    log_num("dynbias: asked x100 ", (unsigned)(asked_avg * 100.0 + 0.5));
    log_num("  delivered x100 ", (unsigned)(delivered * 100.0 + 0.5));
    log_num("  at base ", (unsigned)(base_fps + 0.5));
    const double inst = asked_avg / delivered;
    const int bk = dyn_bucket(asked_avg);
    g_dyn_bias[bk] += (inst - g_dyn_bias[bk]) * 0.25;
    if (g_dyn_bias[bk] < 0.80) g_dyn_bias[bk] = 0.80;
    if (g_dyn_bias[bk] > 1.25) g_dyn_bias[bk] = 1.25;
}

// El adelanto, por frame. Corre sobre g_base_fps, que se actualiza en cada
// frame, y no sobre la ventana de 45: reaccionar una ventana tarde dejaba
// ocho ventanas en 170-199 fps despues de cada escalon de base, que era el
// techo de la version anterior. La banda muerta es lo que evita que correr
// por frame se convierta en una escritura de opciones por frame.
static void dyn_apply(double base_fps) {
    if (g_force_sel != kSelDynFuture) return;
    if (base_fps <= 1.0) return;
    if (g_refresh_hz <= 0) {
        DEVMODEW dm; dm.dmSize = sizeof(dm); dm.dmDriverExtra = 0;
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
            g_refresh_hz = (LONG)dm.dmDisplayFrequency;
        if (g_refresh_hz <= 0) g_refresh_hz = 60;
        log_num("dynamic: refresh is ", (unsigned)g_refresh_hz);
    }
    const double target = (g_dyn_fps > 0) ? (double)g_dyn_fps
                                          : (double)g_refresh_hz;
    // El tramo se elige con la estimacion sin corregir, para no depender de la
    // correccion que se esta por aplicar.
    // Termino integral sobre presentaciones adeudadas. El objetivo se mide
    // sobre promedios de ventana, y perseguir solo la tasa instantanea deja el
    // sobrepaso del escalon dentro del promedio: 10 frames a 224 y 35 a 140 dan
    // 158, que es justo el grupo de ventanas que sobraba. Si despues de un
    // sobrepaso se entrega de menos, el promedio vuelve al objetivo.
    static double debt = 0.0;          // presentaciones adeudadas
    static bool salida_saturada = false;   // el ratio quedo pegado al techo
    static LONG last_pc = 0;
    static LONGLONG last_qpc = 0;
    // QPC y no GetTickCount64. Esta funcion corre por frame -- cada 4.7 ms a
    // 212 fps -- y el tick tiene grano de ~15 ms. Como last_pc se actualizaba
    // en CADA llamada pero now_ms solo avanzaba cuando saltaba el tick, el
    // delta de presentaciones cubria 4.7 ms y el de tiempo 15 ms: dos
    // intervalos distintos. La deuda ganaba ~1.5 presentaciones inventadas por
    // tick, sin relacion con el rendimiento, hasta saturar en su tope.
    //
    // Medido en Cyberpunk: deuda +24 de mediana llegando a 54.45, objetivo
    // efectivo 213 contra un objetivo de 165, ratio crudo 6.2 donde tocaba 4.2.
    // Es el mismo defecto que 275cf16 arreglo en el reloj de bloques.
    LARGE_INTEGER qnow; QueryPerformanceCounter(&qnow);
    const LONGLONG now_q = qnow.QuadPart;
    const LONG pc = g_rt_present_count;
    if (last_qpc != 0 && now_q > last_qpc && g_qpc_freq > 0 && pc >= last_pc) {
        const double secs = (double)(now_q - last_qpc) / (double)g_qpc_freq;
        if (secs < 0.5) {              // un salto largo es un cambio de escena
            double inc = target * secs - (double)(pc - last_pc);
            // Anti-windup. Si el ratio ya esta pegado al techo de 6x, pedir mas
            // no puede traer mas presentaciones, asi que la deuda deja de
            // crecer. Sin esto se enrolla contra un objetivo inalcanzable y el
            // lazo se realimenta: satura la deuda, pide 6x, el juego se atora,
            // se presentan menos frames todavia, la deuda crece mas.
            //
            // Medido en GTA V: deuda clavada en su tope de 54.45, sesgo en
            // 1.25, ratio pedido 5.49 con base 62 -- 340 fps exigidos -- y el
            // juego tildandose con frenos de 200 ms. La misma partida con el
            // codigo anterior pedia 3.01.
            //
            // Solo se frena el crecimiento: bajar siempre se permite, que es
            // como el lazo sale de la saturacion cuando el juego se recupera.
            if (salida_saturada && inc > 0.0) inc = 0.0;
            debt += inc;
            // Acotada a un tercio de segundo de objetivo: sin esto se enrolla
            // durante un apagon y despues descarga todo junto.
            const double lim = target * 0.33;
            if (debt > lim) debt = lim;
            if (debt < -lim) debt = -lim;
        } else {
            debt = 0.0;
        }
    }
    last_pc = pc;
    last_qpc = now_q;
    // La deuda se paga en medio segundo. Se probo en 0.8 -- factor 1.2 -- para
    // achicar el subdisparo que quedaba, y salio peor: 79% contra 87%, con las
    // ventanas altas de vuelta en 150-179. Pagar mas lento deja que el
    // sobrepaso del escalon entre otra vez en el promedio de la ventana.
    const double target_eff = target + debt * 2.0;
    const double raw = (target_eff > 1.0 ? target_eff : 1.0) / base_fps;
    double want = raw * g_dyn_bias[dyn_bucket(raw)];
    // El techo es el estructural, 6.0.
    //
    // Se probo bajarlo a 4.0 despues de un crash de GTA V: las ultimas lineas
    // mostraban cuenta de API 5, una reserva de 763 MB y un "NGX evaluate
    // feature failed". Pero es UNA sola muestra, y en el banco 5.50x -- que es
    // cuenta 4 y 5 -- corrio muchas veces sin crashear. O sea la cuenta alta por
    // si sola no es la causa, y el limite costaba precision real: 79% de
    // ventanas dentro del +-5% contra 89%. Se retira hasta tener una causa
    // demostrada en vez de una coincidencia.
    salida_saturada = (want > 6.0);
    if (want > 6.0) {
        static bool said_hi = false;
        if (!said_hi) {
            said_hi = true;
                log_num("dynamic: target needs more than 6x at this base, fps ",
                    (unsigned)target);
            log_num("  base is ", (unsigned)(base_fps + 0.5));
        }
        want = 6.0;
    }
    if (want < 2.0) want = 2.0;
    // Se acumula todos los frames, se cambie o no el ratio: la ganancia necesita
    // el promedio de lo pedido durante la ventana, no el ultimo valor.
    g_dyn_asked_sum += (double)g_dyn_target / 100.0;
    ++g_dyn_asked_n;
    LONG next = (LONG)(want * 100.0 + 0.5);
    const LONG cur = g_dyn_target;
    // No se compensa el retardo de escritura, y esta probado. Entre escribir un
    // ratio y que las presentaciones lo reflejen pasan 9 o 10 frames -- medido
    // con la sonda: "0 6 3 5 4 7 2 4 4 4 3 3 3 3 3 3" tras bajar desde 6.00 --
    // asi que parecia razonable pedir de mas durante un escalon para llegar
    // antes. Con un 20% extra el resultado cae de 72% a 61% de ventanas en
    // banda y aparece subdisparo, una ventana en 104 fps contra 140. El
    // retardo es el piso de reaccion y adelantarse cuesta mas de lo que ahorra.
    const LONG diff = next > cur ? next - cur : cur - next;
    // Banda muerta de 0.10, y no menos. A base 59 son 5.9 fps -- el 4.2% del
    // objetivo -- asi que parecia el piso de precision del controlador y se
    // probo bajarla a 0.03 con un minimo de tiempo entre escrituras. Salio
    // PEOR: 51% de ventanas en banda contra 63%, la mediana se corrio de 140 a
    // 138 y las escrituras pasaron de 110 a 196. Cada escritura de opciones
    // cuesta mas de lo que la zona muerta desvia, que es algo que este proyecto
    // ya sabia y aca se volvio a comprobar.
    if (diff < 10) return;
    g_dyn_target = next;
    // Solo la primera bajada grande de ratio, que es el caso del sobrepaso.
    if (!g_probe_done && g_probe_left == 0 && cur - next > 80) {
        g_probe_left = 16;
        g_probe_i = 0;
        g_probe_pc0 = g_rt_present_count;
        log_num("probe: ratio dropped from x100 ", (unsigned)cur);
        log_num("  to x100 ", (unsigned)next);
    }
    g_dyn_said = 0;
    g_opt_pending = 1;
    log_num("dynamic: ratio now x100 ", (unsigned)next);
    log_num("  from base ", (unsigned)(base_fps + 0.5));
    // Por que pide lo que pide. En Cyberpunk pidio 5.97 con base 40 y objetivo
    // 165, cuando el ratio crudo es 4.13: el exceso sale de aca o del sesgo, y
    // razonarlo desde el codigo fallo dos veces seguidas.
    log_num("  deuda x100 (mas 32768 si es negativa) ",
            (unsigned)(debt < 0.0 ? 32768u + (unsigned)(-debt * 100.0 + 0.5)
                                  : (unsigned)(debt * 100.0 + 0.5)));
    log_num("  objetivo efectivo ", (unsigned)(target_eff + 0.5));
    log_num("  ratio crudo x100 ", (unsigned)(raw * 100.0 + 0.5));
    log_num("  sesgo x100 ", (unsigned)(g_dyn_bias[dyn_bucket(raw)] * 100.0 + 0.5));
}

static void dynamic_tick(void) {
    if (g_imm_n > 0) return;
    if (!sel_is_frac()) return;
    const LONG want = dynamic_want();
    static LONG last_want = -1;
    static int steady = 0;
    if (want != last_want) { last_want = want; steady = 0; return; }
    if (++steady < 45) return;                            // ~a third of a second
    steady = 0;
    if (want == g_force_generated) return;
    log_num("dynamic: measured fps ", (unsigned)(int)g_token_fps);
    log_num("  base fps behind it ", (unsigned)(int)g_base_fps);
    log_num("  generating N frames now: ", (unsigned)want);
    g_force_generated = want;
    g_opt_pending = 1;
    // Re-seeded: the old multiplier's readings say nothing about the new one,
    // and blending them in is what would make the average chase its own tail.
    g_token_fps = 0.0;
    g_token_dt = 0.0;
}

// A fractional multiplier, spread over frames.
//
// The count is an integer -- it is a loop bound, and a loop cannot run 2.1
// times -- so the fraction lives in the cadence instead of in the number. An
// error accumulator carries the remainder from one frame to the next: at a
// ratio of 2.1x nine frames generate one extra and the tenth generates two,
// and the average over any window is 2.1 exactly.
//
// This writes the immediate the loop now compares against, so it takes effect
// on the very next batch and needs no thread of ours.
// How many frames a count is held before it may change. One is the old
// behaviour -- change as soon as the accumulator says so.
// Frames per cadence period. Longer means fewer changes and coarser bursts;
// shorter means smoother distribution and more changes, which is what costs.
static int kFracPeriod = 8;
static double g_frac_acc = 0.0;
// What the cadence is actually producing, averaged. The correction above is
// meaningless without it: the error has to be measured against the output, and
// the output is this.
static double g_produced_avg = 0.0;

static void fractional_tick(void) {
    static LONG was_sel = -1;
    if (g_force_sel != was_sel) {
        was_sel = g_force_sel;
        g_frac_acc = 0.0;
        g_token_fps = 0.0;        // the previous mode's readings say nothing
        g_token_dt = 0.0;
    }
    // An integer selection still has to keep the byte, because the patch
    // replaced the plugin's own write with an immediate and nothing else fills
    // it in. Left alone it holds whatever the last DYNAMIC frame put there --
    // or the seed -- and a count that disagrees with the plugin stops
    // presentation outright: 3.00x read 1.000 with the override armed and
    // applied zero times, three runs out of three, rendering as if nothing were
    // enabled.
    //
    // This is what makes the patch safe to arm without a fractional selection,
    // which is the whole obstacle to shipping the dll on its own. Rows 2..N are
    // fixed multipliers holding row-1 generated frames; row 1 is off.
    if (!sel_is_frac()) {
        if (g_force_sel >= 1 && g_force_sel <= kSelMaxFixed)
            set_count_now(g_force_sel >= 2 ? g_force_sel - 1 : 0);
        else if (g_force_sel == 0)
            // AUTO, and this row is the one that ships: with no settings file
            // beside the dll the panel starts here. The first version of this
            // guard began at row 1 and left AUTO out, so the patched byte kept
            // the seed -- one generated frame -- and a game that asked for 3x
            // or 4x on its own got pinned to 2x without a word. That is the
            // same shape as the integer break it was written to fix, on the
            // one row nobody selects deliberately.
            //
            // g_last_seen_generated is what the game asked for, captured in the
            // options wrapper and already trusted enough to hand back on the
            // AUTO restore path.
            set_count_now(g_last_seen_generated);
        return;
    }
    if (g_wic_mode ? (g_wic_n == 0) : (g_imm_n == 0)) return;
    if (g_base_fps <= 1.0) return;

    // The multiplier, straight. No target to chase and so no loop to settle:
    // the ratio is what was asked for, and the frame rate is whatever the base
    // multiplied by it comes to. Closing a loop over the presented rate was
    // solving a problem this does not have -- and it could not win anyway,
    // since generation cannot lower a base that is already above the target.
    double per_frame = (double)g_dyn_target / 100.0 - 1.0;
    if (per_frame < 0.0) per_frame = 0.0;
    if (per_frame > 5.0) per_frame = 5.0;

    // The shape of a whole period, decided once, rather than a value decided
    // per frame and then held back.
    //
    // Spreading the fraction as evenly as possible -- 0,1,0,1 for 1.5x -- is
    // right for the average and wrong for the pacing: the cost is per change,
    // and that spreads the changes as widely as they can go. Measured in MFG
    // Lab on the Streamline sample, everything else held constant:
    //
    //   2.00x  never changes     0 hitches   p99  38ms   30.0 fps
    //   2.10x  changes 1 in 10   0 hitches   p99  65ms   28.5 fps
    //   2.50x  changes 1 in 2   19 hitches   p99 121ms   30.0 fps
    //   1.50x  changes 1 in 2    6 hitches   p99 235ms   13.9 fps
    //
    // So the frames that take the higher count are grouped at the front of the
    // period: two changes per period instead of one per frame. Holding a
    // per-frame decision back was tried first and does not work -- the held
    // value and the demand fight, the accumulator cannot settle the difference
    // without going negative, and 1.5x came out as 2.0x. Deciding the period
    // has no such conflict: the count of high frames is what carries the
    // fraction, and it is exact over each period.
    const LONG lo = (LONG)per_frame;
    g_ciclo_techo = lo + 1;   // A1: el maximo que este ciclo va a pedir
    const double frac = per_frame - (double)lo;

    static int pos = 0;
    static int hi_frames = 0;
    if (pos == 0) {
        if (g_slowalt) {
        // Whole blocks at lo, then whole blocks at lo+1, in the proportion the
        // fraction asks for. The loop byte follows the API exactly so the two
        // can never disagree, which is the condition every crash so far
        // violated.
        // Eight blocks to a cycle, so the whole pattern fits inside a few
        // seconds. A hundred blocks -- the first attempt -- made a cycle of
        // 12000 rendered frames, and a run that renders 540 never reached the
        // second half of it, which is why the count looked as though it never
        // alternated at all.
        // Blocks measured in time, not in rendered frames.
        //
        // A block at the low count renders far faster than one at the high
        // count -- 167 fps against 89.8 with generation off and on -- so equal
        // frame counts are unequal durations, and what a player sees is the
        // time-weighted mix, not the frame-weighted one. Counting frames made
        // the low blocks occupy 5.7 seconds against 10.7 for the high ones
        // while the arithmetic assumed they were equal, which is most of why
        // 1.50x delivered 1.81-1.90.
        static double sa_clock = 0.0;
        // Advanced once per rendered frame, not once per token call.
        //
        // fractional_tick runs per slGetNewFrameToken call and the sample makes
        // about seven of those per rendered frame, so summing g_last_dt here
        // ran the schedule clock roughly seven times too fast: a nominal 8 s
        // block was about 1.1 s of wall time, and every block length in this
        // file's measurements was off by that factor. g_last_dt only changes
        // when a burst ends, so adding it once per change is once per frame.
        // Once per frame, because the caller is now gated on the frame index.
        // The edge check that used to stand here was undoing the burst, and
        // against a gated caller it would drop any frame whose dt repeated.
        //
        // AHORA MIDE TIEMPO, NO LLAMADAS.
        //
        // Sumaba g_last_dt una vez por llamada que pasaba el gate, y eso es un
        // conteo de llamadas disfrazado de reloj. Dos formas de romperse, las
        // dos hacia el mismo lado:
        //
        //   - g_last_dt es el ULTIMO intervalo observado, no el transcurrido
        //     desde la vuelta anterior. Si el juego hace mas de una llamada por
        //     frame -- Cyberpunk hace 2.6 despues del gate, medido: 288504
        //     llamadas contra 110847 que pasan -- el reloj avanza 2.6 veces mas
        //     rapido y los bloques salen 2.6 veces mas cortos.
        //   - Las llamadas con dt fuera de [2ms, 200ms] NO actualizan g_last_dt
        //     pero igual suman el valor viejo. Una rafaga de cinco llamadas en
        //     un milisegundo suma cinco intervalos completos.
        //
        // Consecuencia medida en Cyberpunk con CUSTOM 2.50: la cuenta de la API
        // cambiaba cada 140 ms de mediana y con minimos de 0 ms, cuando el ciclo
        // pide un cambio cada 384 ms. Cambiar la cuenta asi de rapido es la
        // condicion de crash ya documentada -- treinta frames renderizados es lo
        // que corre limpio -- y el juego se caia. Con 2X entero no se cae porque
        // no alterna la cuenta.
        //
        // El reloj de pared no depende de nada de eso: se llame una vez o cien
        // por frame, la suma de los intervalos reales es el tiempo real. Y el
        // horario esta expresado en segundos, asi que la fuente correcta son
        // segundos.
        {
            static LONGLONG prev_qpc = 0;
            LARGE_INTEGER ahora;
            QueryPerformanceCounter(&ahora);
            if (prev_qpc != 0 && g_qpc_freq > 0) {
                const double d = (double)(ahora.QuadPart - prev_qpc) / (double)g_qpc_freq;
                // Un salto largo es una pantalla de carga o un menu, no tiempo
                // de juego: se ignora en vez de saltear medio ciclo de golpe.
                if (d > 0.0 && d < 0.5) sa_clock += d;
            }
            prev_qpc = ahora.QuadPart;
        }
        // Read from mfg-blockms.txt when present, so the block length can be
        // swept without a rebuild. The unit is milliseconds of the cadence
        // clock, which advances once per frame-token call -- about seven times
        // per rendered frame -- so 750 here is ~125 ms of wall clock, not 750.
        // Eight seconds, measured. The cost of this whole approach is the
        // count changing, and it scales with how often that happens -- nothing
        // else. Presented fps against block length, 2.50x, same scene:
        //
        //   0.5 s   66 changes  42.7% off-refresh  141.0 fps
        //   0.75 s  47          37.0%              148.4
        //   4 s      8          10.5%              158.0
        //   8 s      4           3.3%              163.1
        //   12 s     3           0.3%              165.3
        //
        // The null control settles what causes it: run the same scheduler, the
        // same clock and the same slDLSSGSetOptions replay at every boundary
        // but with both values equal, and the cost vanishes entirely (0.0-0.3%,
        // 165.3 fps). So it is neither the API calls nor our own logging --
        // silencing that changed nothing -- it is the count taking a different
        // value.
        //
        // 8 s rather than 12: the ratio still averages correctly (2.48 asked
        // 2.50, over 47 windows) and the presented rate holds 167 fps with two
        // dips in 47 windows, at the block boundaries. Longer blocks buy the
        // last 2 fps and make the average slower to settle, which would matter
        // if the target ever moved.
        // 1.1 s, and the old 8.0 here was never 8 seconds. The clock this
        // reads advanced once per frame-token call, seven of those per frame,
        // so the block that called itself 8 s lasted about 1.1 s of wall time
        // -- and 1.1 s is the length that measured 163 fps presented with 3.3%
        // of presents off-refresh. Gating the token hook on the frame index
        // made the clock honest, which would have stretched the same setting
        // to a 256 s cycle: whole minutes parked on one integer count. The
        // number changes so the behaviour does not.
        // 16 ms, so a whole cycle of 32 blocks lands at 0.512 s and a window
        // of 45 frames sees both states instead of sitting inside one. Longer
        // blocks read whole integers per window whatever the request is: at
        // 1.1 s, 1.50x reads 1.00 with an IQR of 1.00.
        //
        // OJO: el comentario que estaba aca decia que el camino de bloques solo
        // corre debajo de 2.0x. Es falso desde e34e680 -- "Bloques en todo el
        // rango", unas lineas mas abajo -- porque g_peralt esta apagado por
        // defecto y con el la difusion por frame nunca se elige. Los bloques
        // corren en TODO el rango.
        //
        // Eso deja esta constante sirviendo a dos regimenes con requisitos
        // opuestos. Los 16 ms se eligieron por lo de abajo de 2.0x: ahi el
        // estado bajo apaga la generacion, entrar y salir cuesta una
        // presentacion, y el ciclo entero tiene que entrar en una ventana de
        // medicion. Arriba de 2.0x el comentario de mas abajo dice que difundir
        // por frame es gratis, y la tabla de esta misma funcion dice que
        // bloques largos entregan mas fps con muchisimos menos cambios de
        // cuenta. Cada cambio de cuenta hace que el plugin libere y reserve del
        // orden de 750 MB, medido en GTA V.
        //
        // MEDIDO. A 2.50x, misma escena, 47 ventanas cada uno, y la
        // distribucion es por ventana con PresentCount, nunca la media sola:
        //
        //   bloque  ciclo    vram   cambios   distribucion por ventana
        //   16 ms   0.512 s   297     148     46 de 47 en 2.50
        //   24 ms   0.768 s   272      98     47 de 47 en 2.50
        //   125 ms  4.0 s     233      20     15 en 2.00 y 18 en 3.00
        //
        // Los 24 ms son el arreglo: un tercio menos de reconfiguraciones de
        // latencia sin pagar nada en cadencia. La distribucion no empeora --
        // 47 de 47 contra 46 de 47, que es la misma cosa, no una mejora.
        //
        // Los 125 ms son el extremo, no el candidato: un ciclo de 4 s es ocho
        // ventanas de medicion, y ahi si se rompe. Pero eso no es un artefacto
        // del instrumento: un ciclo de 4 s es literalmente dos segundos a 2x y
        // dos a 3x, y eso el jugador lo ve. El compromiso no es continuo, tiene
        // un codo, y el codo esta arriba de 0.768 s.
        //
        // El techo de 24 ms no es arbitrario. El ciclo son 32 bloques, o sea
        // 0.768 s, y la ventana de 45 frames a base 59 dura 0.763 s: el ciclo
        // entra justo. Subir mas el bloque saca el ciclo de la ventana, que es
        // el punto donde toda ventana lee un entero entero -- primero deja de
        // medirse, y despues, mas arriba, deja de servir.
        //
        // Y de paso corrige la premisa que se venia repitiendo: las reservas de
        // VRAM casi no son nuestras. 297 -> 272 -> 233, y 233 es lo que mide un
        // entero fijo que hace tres cambios de cuenta en toda la corrida. El
        // piso es del plugin y lo nuestro agrega unas 64. Lo que si escala con
        // nuestros cambios son las reconfiguraciones de latencia: 148, 98, 20, 3.
        //
        // La condicion es lo >= 1, no "arriba de 2.0x", porque el mecanismo por
        // el que se eligieron los 16 ms es que el estado bajo apague la
        // generacion. Con lo == 0 entrar y salir cuesta una presentacion y solo
        // los 16 ms sobreviven la transicion; con lo >= 1 la generacion nunca
        // se apaga y esa razon no aplica. Debajo de 2.0x no se toca nada.
        const double kBlockSecs = g_block_ms > 0 ? (double)g_block_ms / 1000.0
                                                 : (lo >= 1 ? 0.024 : 0.016);
        // Thirty-two blocks, not eight. The split is quantised to 1/kBlocks of
        // the cycle, and below 2.0x the rate weighting pushes the useful range
        // to one end: 1.90x wants 94% of the time generating, which eight
        // blocks can only render as 8/8 -- a flat 2.00x, 5% high. Thirty-two
        // brings every point inside 1%. The block *length* is unchanged, so the
        // count changes no more often than before; only the cycle is longer.
        // Blocks per cycle, from mfg-blocks.txt when present.
        //
        // The pair (block length, blocks per cycle) has to keep the whole cycle
        // inside the 45-frame window or every window reads a whole integer.
        //
        // It was added to test whether the base rate being refresh/(ceiling + 1)
        // instead of the cadence average is a pacer that never settles: fewer
        // and longer blocks give it a longer stretch at the low count. It is
        // not. At 2.50x, with the cycle held inside the window throughout:
        //
        //   per frame            base 55   presented 136
        //   8 blocks x 60 ms     base 55   presented 139
        //   4 blocks x 100 ms    base 55   presented 137
        //   16 blocks x 30 ms    base 55   presented 138
        //
        // Stretches of 200 ms at the low count read the same as changing every
        // frame. The pinning does not depend on settling time, so no
        // arrangement of the same two counts moves it.
        //
        // What it is, stated as a law that fits the data rather than as a
        // suspicion: the producer is throttled to refresh/(ceiling + 1)
        // rendered frames per second even on the frames that generate fewer, so
        //
        //     presented = refresh * ratio / (ceiling + 1)
        //
        // That holds to within 1 fps on 2.00, 2.25, 2.50, 2.75, 3.00, 1.50,
        // 1.75 and 1.90, and it breaks at exactly the two points where the
        // mechanism does not apply -- 1.10x and 1.25x, whose low state is count
        // 0, where the real frame presents down the ordinary path with no pacer
        // in it. The loss is display slots left empty: at 2.25x, 2.25 of every
        // 3 are used and the missing 41 fps are the other quarter.
        //
        // So the fix has an exact target: make the throttle follow the count
        // that frame actually generates instead of the cadence ceiling. Then a
        // ratio at or above 2.0 saturates the display the way both neighbouring
        // integers already do. It lives in the pacer inside sl.dlss_g.dll, and
        // it is not attempted here.
        const int kBlocks = g_blocks > 0 ? g_blocks : 32;
        const double cycle_secs = kBlockSecs * (double)kBlocks;
        bool cycle_wrapped = false;
        if (sa_clock >= cycle_secs) { sa_clock -= cycle_secs; cycle_wrapped = true; }
        const int sa_pos_block = (int)(sa_clock / kBlockSecs);
        static int sa_pos = 0;

        // How many blocks take the high count, corrected by what came out.
        //
        // The open-loop split -- frac x kBlocks -- is right only if a block at
        // the low count delivers exactly its share, and below 2.0x it does not:
        // the low count there is zero, generation is off for those blocks, and
        // the app renders faster without it, so the low blocks contribute more
        // frames than the arithmetic assumed and the average lands high. 1.50x
        // asked, 1.90 delivered.
        //
        // So the split is nudged by the error between the ratio asked for and
        // the one the last full cycle actually produced. Presented frames per
        // rendered frame is (1 + generated per rendered frame), and both are
        // counted here, on this thread, over whole cycles.
        static double sa_bias = 0.0;
        static double cyc_gen = 0.0;
        static int cyc_frames = 0;
        // El umbral era g_last_dt porque el reloj avanzaba de a un dt por
        // vuelta. Ahora avanza tiempo real, asi que el paso tipico es mucho
        // menor: se usa un valor fijo y chico, holgado contra el ciclo de
        // 0.768 s y suficiente para detectar la vuelta.
        if (sa_clock < 0.05 && g_lo_time + g_hi_time > 0.5) {
            // Weighted by time, for the same reason the blocks are: presented
            // frames per second over rendered frames per second is what the
            // player experiences.
            // Frame-weighted, which is what the delivered ratio is. This is
            // still the figure the audit called circular -- it decrees what each
            // frame presented -- and it is used ONLY to nudge the split, never
            // reported as a result. The reported number is the counted one.
            const double pres = (double)g_lo_frames_seen * (double)(lo + 1)
                              + (double)g_hi_frames_seen * (double)(lo + 2);
            const double ren = (double)(g_lo_frames_seen + g_hi_frames_seen);
            const double produced = ren > 0.0 ? pres / ren : 1.0;
            const double want = per_frame + 1.0;
            // Gently: a cycle is a second or two, and a gain that corrects in
            // one step would hunt between the two counts instead of settling.
            // Reported once per cycle: the ratio the dll actually delivered,
            // counted on this thread over whole cycles. Below 2.0x the
            // saturated `cap / rendered` estimate is invalid -- generation is
            // off for part of the cycle, so the base rate itself moves -- and
            // this is the only figure that stays meaningful there.
            // The delivered ratio, from the clock rather than from our own
            // choices. Accumulating the api value just picked and calling its
            // average "delivered" is circular -- it can only report what was
            // asked for, which is exactly what it did: 150 on every cycle from
            // the first, while an independent estimate said 1.81. Same mistake
            // as `ratio produced x100`.
            //
            // These counters are measured: how many frames the app rendered
            // while each count was in force, and how long that took.
            // Also with only one block type in play -- an integer ratio never
            // alternates, so without this the controls report nothing and the
            // instrument goes unchecked on the two cases whose answer is known.
            if (g_lo_frames_seen + g_hi_frames_seen > 0) {
                const double tot_ren = (double)(g_lo_frames_seen + g_hi_frames_seen);
                const double tot_pres = (double)g_lo_frames_seen * (double)(lo + 1)
                                      + (double)g_hi_frames_seen * (double)(lo + 2);
                // CIRCULAR -- kept only because the block rates beside it are
                // real. This figure decrees that each low-block frame presented
                // (lo+1) and each high-block frame (lo+2), which is the very
                // thing under test: it reduces to the fraction of samples taken
                // while the high count was selected, and returned the request
                // to within 0.3% even at 2.10x and 2.90x, fractions the
                // scheduler cannot represent. Use "counted multiplier x100",
                // which counts presents at the swap chain.
                log_num("slowalt: CIRCULAR ratio x100 ",
                        (unsigned)(int)(tot_pres / tot_ren * 100.0 + 0.5));
                log_num("  low block fps x10 ",
                        (unsigned)(int)(g_lo_time > 0.0 ?
                            (double)g_lo_frames_seen / g_lo_time * 10.0 : 0.0));
                // Presented frames per second, and the ratio against rendered,
                // both from the clock. This does not assume the display is the
                // limit -- and it is not: in a 2.50x run the low blocks render
                // 61 fps where saturation against a 165 Hz panel would put them
                // at 82, so the "cap / rendered" estimate is invalid there and
                // read 2.76 against a true 2.47.
                {
                    const double t = g_lo_time + g_hi_time;
                    const double pres_n = (double)g_lo_frames_seen * (double)(lo + 1)
                                        + (double)g_hi_frames_seen * (double)(lo + 2);
                    if (t > 0.0) {
                        log_num("  presented fps x10 ", (unsigned)(int)(pres_n / t * 10.0));
                        log_num("  rendered fps x10 ",
                                (unsigned)(int)((double)(g_lo_frames_seen + g_hi_frames_seen) / t * 10.0));
                    }
                }
                log_num("  high block fps x10 ",
                        (unsigned)(int)(g_hi_time > 0.0 ?
                            (double)g_hi_frames_seen / g_hi_time * 10.0 : 0.0));
                // Snapshot first: the conversion below runs in this same call
                // and needs these.
                g_rate_lo = g_lo_time > 0.05 ? (double)g_lo_frames_seen / g_lo_time : 0.0;
                g_rate_hi = g_hi_time > 0.05 ? (double)g_hi_frames_seen / g_hi_time : 0.0;
                g_lo_frames_seen = 0;
                g_hi_frames_seen = 0;
                g_lo_time = 0.0;
                g_hi_time = 0.0;
            }
            // Full gain, no dead band. Both were tried on the theory that the
            // loop was hunting -- 26 count changes at 1.25x where the pattern
            // calls for 4 -- and both made the whole range worse: 1.50x fell
            // from 1.48 to 1.36 and every point settled about 9% low. The
            // changes are the loop tracking a base rate that really does move,
            // not noise, and damping it just leaves the error uncorrected.
            // Removed, not retuned. An independent audit reduced this loop to
            // its setpoint: `produced` is presents per rendered frame under the
            // assumption that each count-c frame presents c+1 times, which is
            // the same quantity `presents / 45` reports. So the integrator ran
            // until its estimate of the reported metric equalled the request,
            // and the sweep could only ever return the request. It did: the
            // loop's own `CIRCULAR ratio x100` read 109/113/125/150/174/188/190
            // against 110/113/125/150/175/187/190 asked.
            //
            // What the open-loop split actually delivers is recorded a few
            // lines up, from before this loop existed: 1.50x asked, 1.90
            // delivered; 1.10x delivered 1.00. Those are the numbers to beat,
            // and beating them has to come from the schedule.
            //
            // The tuning history above is void for the same reason -- "full
            // gain beat a dead band" compares two ways of reverse-fitting.
            (void)produced;
            sa_bias = 0.0;
            cyc_gen = 0.0;
            cyc_frames = 0;
        }
        // Blocks split by frames, not by time.
        //
        // `frac` is the fraction of *rendered frames* that must take the high
        // count, because the delivered ratio is the frame-weighted mean of
        // (count + 1). Handing that straight to a time-based schedule is only
        // correct when both states render at the same rate, and below 2.0x they
        // do not: the low count there is zero, generation is off, and the app
        // renders nearly twice as fast (167 fps against 89.8 measured). So an
        // equal-time split gives the low state far more frames than intended
        // and the ratio lands low -- 1.10x delivered 1.00, 1.50x delivered 1.27.
        //
        // Converting frame fraction to time fraction:
        //
        //   t_hi / (t_hi + t_lo) = (frac / r_hi) / (frac / r_hi + (1-frac) / r_lo)
        //
        // where r is each state's measured rendered rate. Above 2.0x both rates
        // are close and this is nearly a no-op, which is why it was not needed
        // there; below 2.0x it is the whole correction.
        // The correction is NOT added here. sa_bias is computed from the error
        // in frames -- want minus produced, both frame-weighted -- and adding it
        // to `frac` sends it through the frame-to-time conversion below, so it
        // gets applied twice. At 1.10x, where the two rates differ most, a bias
        // of 0.35 turned a requested 10 percent of frames into 59 percent of the
        // time: 19 blocks of 32 measured, against the 5 the ratio calls for. The
        // delivered ratio came out 0.98, with windows as low as 0.88 -- fewer
        // presents than rendered frames, which is loss, not a low multiplier.
        double t_frac = frac;
        if (t_frac < 0.0) t_frac = 0.0;
        if (t_frac > 1.0) t_frac = 1.0;
        {
            const double r_lo = g_rate_lo;
            const double r_hi = g_rate_hi;
            if (r_lo > 1.0 && r_hi > 1.0) {
                const double a = t_frac / r_hi;
                const double b = (1.0 - t_frac) / r_lo;
                if (a + b > 0.0) t_frac = a / (a + b);
            }
        }
        // Latched once per cycle, not recomputed every frame.
        //
        // This runs per frame-token call, so hi_blocks was being recalculated
        // thousands of times inside a single cycle, and every recalculation
        // could move the boundary the schedule was already walking past. A
        // contiguous run of high blocks should change the count twice per
        // cycle; it was changing 46 to 86 times, and the ratio came out low
        // because blocks kept being reclassified underneath the cursor.
        //
        // It also invalidated every comparison built on top of it: two attempts
        // to improve this range -- spreading the blocks, damping the loop --
        // were judged against a schedule that was not holding still, and the
        // same configuration measured 1.48 once and 1.34 three times running.
        t_frac += sa_bias;
        if (t_frac < 0.0) t_frac = 0.0;
        if (t_frac > 1.0) t_frac = 1.0;
        // A new request is not churn, so it does not wait for the wrap.
        //
        // The latch below exists because this runs per frame and a boundary
        // that moves under the walking cursor reclassifies blocks the schedule
        // has already passed. But it was also swallowing the one recalculation
        // that is not churn: the user picking a different multiplier. A cycle
        // is 32 blocks of 1.1 s, so a change made in the panel could sit unused
        // for half a minute -- which is exactly what "the multiplier takes a
        // while to apply" looks like from inside the game.
        //
        // Restarting the cycle as well, rather than only the split: entering a
        // new schedule two thirds of the way through a cycle would spend the
        // remainder walking blocks laid out for the previous request.
        static int hi_blocks = -1;
        bool request_changed = false;
        {
            static double last_req = -1.0;
            if (frac != last_req) {
                last_req = frac;
                request_changed = hi_blocks >= 0;
            }
        }
        if (request_changed) {
            sa_clock = 0.0;
            sa_bias = 0.0;   // the old correction was for the old request
        }
        if (cycle_wrapped || request_changed || hi_blocks < 0) {
            hi_blocks = (int)(t_frac * (double)kBlocks + 0.5);
            if (hi_blocks < 0) hi_blocks = 0;
            if (hi_blocks > kBlocks) hi_blocks = kBlocks;
        }
        // A null control: with mfg-nullalt.txt the scheduler runs exactly as it
        // does for a fractional ratio -- same blocks, same clock, same
        // slDLSSGSetOptions replay every boundary -- but both values are the
        // same, so nothing about the generated count changes. If the cost
        // survives that, it is the machinery of alternating; if it vanishes,
        // it is the count itself changing. Nothing else separates the two.
        // High blocks contiguous, and the reason is measured rather than
        // aesthetic.
        //
        // Spreading them evenly (Bresenham) is the obvious way to make the
        // fraction look like a fraction rather than two long stretches, and it
        // is worse on every count. Measured against the contiguous run, same
        // block length, same everything:
        //
        //   1.25x  contiguous 1.16, 26 changes, 5% off-refresh
        //          spread     1.05, 321 changes, 36% off-refresh
        //   1.50x  contiguous 1.48, 5% off-refresh
        //          spread     1.39, 199 changes, 43% off-refresh
        //
        // Each count change costs, so multiplying the changes by ten multiplies
        // the cost. The evenness is not worth what it takes.
        //
        // Both of those numbers were taken against a schedule that was not
        // holding still -- hi_blocks was being recomputed thousands of times
        // per cycle -- with a rendered-frame counter 3.7% high and a block
        // clock running 7x fast, so every block length was mislabelled by that
        // factor. They are not evidence any more.
        //
        // And contiguous blocks cannot satisfy the criterion at all. The
        // delivered ratio over N frames is 1 + mean(api), so a window only
        // reads 2.75 if the count varies inside that window. With blocks of
        // 1.1 s a window of 0.5 s always sits inside one block and can only
        // ever read a whole integer. Measured in GTA V: 2.75 asked, 3.00 read
        // flat across every window, because 24 of the 32 blocks are high and
        // contiguous -- 26 s of 3x before the first 2x block.
        //
        // mfg-peralt.txt selects error diffusion on the frame instead. Note
        // what it removes: mean(api) is a per-frame average, so scheduling per
        // frame needs no weighting between the two states' render rates. That
        // weighting is what the whole sub-2.0x failure came down to, and here
        // the question does not arise.
        //
        // This is not the per-frame scheme the note above rejects. That one
        // held a decision back and let the held value fight the demand; the
        // accumulator could not settle the difference without going negative,
        // and 1.5x came out as 2.0x. Nothing is held here: each frame takes
        // the whole part of the accumulator and leaves the remainder.
        // Which scheduler, decided by whether the low state generates.
        //
        // Above 2.0x the two states are lo and lo+1 with both generating, and
        // diffusing per frame is free: 2.75x reads 2.76 with an inter-quartile
        // range of 0.02, and the rendered rate is 55 either way -- the same 55
        // that whole-second blocks give while delivering 3.00 instead of 2.75.
        //
        // Below 2.0x the low state is count 0, generation off, and entering and
        // leaving it costs a present. Held, it is harmless: whole blocks at
        // count 0 read a clean 1.00. Toggled every frame it loses half of them
        // -- 1.50x measured 0.51. Blocks of 16 ms survive it because they are
        // contiguous: 16 high blocks in a row is 256 ms of settled state, and
        // the whole cycle still fits in 0.512 s, just inside the window the
        // ratio has to hold over. That reads 1.49 with an IQR of 0.07.
        //
        // It is not free there. The rendered rate falls from 134 to 83, which
        // whole-second blocks do not cost. The toggling itself is the price and
        // no arrangement of the same two states avoids it.
        // mfg-peralt.txt forces diffusion below 2.0x as well, which is how
        // the destructive case stays reproducible rather than becoming a
        // number in a comment.
        // Bloques en todo el rango, no difusion por frame.
        //
        // La difusion cambia la cuenta casi cada frame, y cada cambio dispara
        // una llamada a slDLSSGSetOptions -- 8985 en una sesion de GTA V --
        // que hace que el plugin libere recursos y arranque 100 ms de
        // enfriamiento (0x1800497fd escribe 100.0 en [ctx+0x4488]). Con una
        // llamada por frame el enfriamiento no termina nunca: 290 de 389
        // ventanas a 2.25x quedaron con la generacion apagada, ratio 1.34
        // contra 2.25 pedido. La guia de NVIDIA lo dice sin rodeos: llamarla
        // en interacciones de UI, no por frame.
        //
        // Los bloques cambian la cuenta dos veces por ciclo: con 32 bloques de
        // 16 ms son unas 4 veces por segundo en vez de cien, y la cuenta de la
        // API sigue siendo la del byte, que es el invariante que rompio el
        // intento de mandar el techo constante.
        //
        // mfg-peralt.txt sigue eligiendo la difusion, para poder comparar.
        const bool diffuse = g_peralt && !g_nullalt && !g_blockalt;
        LONG want;
        if (diffuse) {
            static double acc = 0.0;
            static double last_pf = -1.0;
            if (per_frame != last_pf) { last_pf = per_frame; acc = 0.0; }
            acc += per_frame;
            want = (LONG)acc;               // whole part
            acc -= (double)want;            // remainder carries to the next
            if (want < 0) want = 0;
            if (want > lo + 1) want = lo + 1;
        } else {
            want = g_nullalt ? lo + 1
                 : ((sa_pos_block < hi_blocks) ? lo + 1 : lo);
        }
        sa_pos = (sa_pos + 1) % (kBlocks * g_slowalt_len);
        // Zero is kept as zero here, not clamped to one. Below 2.0x the API
        // cannot express the ratio at all -- its smallest generating value is
        // one, which is 2.0x -- so the only way down is whole blocks with
        // generation off alternating with blocks at 2.0x. force_into already
        // writes eOff when the count is zero, so nothing else is needed; what
        // this cannot do is make the transition free, and at a couple of
        // seconds per block the question is whether it reads as pulsing.
        const LONG api = want < 0 ? 0 : (want > 5 ? 5 : want);
        if (api != g_force_generated) {
            g_force_generated = api;
            g_opt_pending = 1;
            // Twice per cycle at most, so cheap -- and the only direct evidence
            // that the count moved. The plugin logs a count only when the
            // enabled/disabled state changes, so its log cannot answer this.
            log_num("slowalt: API count now ", (unsigned)api);
            g_last_change_pres = g_present_count;
        }
        // Rendered frames and elapsed time, split by which count was in force.
        // Their ratio is a fact about the pipeline; the average of the counts we
        // chose is not.
        {
            // Once per rendered frame, on the same edge the schedule clock uses.
            // These ran once per token call -- about seven times a frame -- so
            // the ratio they feed the correction loop was right only if the
            // burst length is identical with generation on and off, which is
            // exactly what turning generation on changes.
            if (api > lo) { ++g_hi_frames_seen; g_hi_time += g_last_dt; }
            else          { ++g_lo_frames_seen; g_lo_time += g_last_dt; }
        }
        cyc_gen += (double)api;
        ++cyc_frames;
        set_count_now(api);
        return;
    }

    g_frac_acc += frac * (double)kFracPeriod;
        hi_frames = (int)g_frac_acc;
        if (hi_frames > kFracPeriod) hi_frames = kFracPeriod;
        g_frac_acc -= (double)hi_frames;
    }
    LONG n = pos < hi_frames ? lo + 1 : lo;
    pos = (pos + 1) % kFracPeriod;

    // Zero is fine now: the entry guard is ours too, so a frame with nothing
    // generated simply skips the loop. That is what every ratio under 2.0x is
    // made of.
    g_produced_avg = g_produced_avg * 0.995 + (double)n * 0.005;
    set_count_now(n);
    // The API still has to be told to turn generation ON, and with a count it
    // will accept. Our byte decides how many frames each batch really makes,
    // but the plugin never reaches that loop unless it has been enabled first
    // -- and `eOn` with a count of zero is refused outright. Asking for zero
    // through the API is exactly what left sl.log without a single
    // "interpolation state changed" line while the byte sat there unread.
    // The API is told the *floor* of the cadence, never more.
    //
    // A batch stalls when the loop delivers fewer frames than the presentation
    // side was promised -- that is what turned 75 fps into 35, and what froze
    // the game outright at 1.00x where every batch was promised one and given
    // none. Promising the smallest number the cadence ever produces means the
    // loop can only ever match it or exceed it, and nothing waits.
    //
    // It also draws the line honestly: at 2.5x the cadence is 1 and 2, so the
    // floor is 1 and every batch is safe. At 1.5x it is 0 and 1, the floor is
    // 0, and zero is refused by the API -- so ratios under 2.0x still cannot
    // work this way and are held at 2.0x rather than allowed to stall.
    // Exactly what this frame will generate, including none of them. Zero
    // goes through as eOff rather than as eOn with a count of zero, which the
    // plugin refuses -- and that refusal is what held every ratio under 2.0x
    // at 2.0x. force_into already writes eOff when the count is zero.
    // No API call per frame any more. The three bytes carry the count -- loop
    // bound, entry guard and metering -- so slDLSSGSetOptions is left to the
    // game, and the race against Present goes with it.
    // The API is told the ceiling of the ratio, not this frame's count.
    //
    // The plugin sizes its per-sub-frame resources from the count it is given,
    // and the loop bound writing past that is an access violation: 2.50x, whose
    // cadence alternates one and two generated frames, crashed the sample with
    // 0xC0000005 reproducibly while the API count alternated with it. Asking
    // for the maximum the cadence will ever reach means the allocation always
    // covers the loop, and the loop is then free to produce fewer.
    // The API is told the ceiling of the ratio, not this frame's count.
    //
    // The plugin sizes its per-sub-frame resources from the count it is given,
    // and a loop bound past that allocation is an access violation: 2.50x
    // crashed the sample with 0xC0000005, reproducibly, while the API count
    // alternated with the cadence. The ceiling covers the largest bound the
    // cadence will use. (Pinning it at the declared maximum of 5 also avoids
    // the crash, but asks the plugin to do five frames of work for a ratio that
    // needs one or two.)
    const LONG ceil_n = (LONG)(per_frame + 0.999);
    // Varying the API count at runtime crashes, in every arrangement tried:
    // per frame, in blocks of eight, and in blocks with the loop bound clamped
    // to what the plugin had actually applied so the two could never be out of
    // step. All three end in 0xC0000005. The count is fixed for the life of the
    // run at the ceiling of the ratio.
    g_force_generated = ceil_n < 1 ? 1 : (ceil_n > 5 ? 5 : ceil_n);

    // Reported rarely: this runs on the render thread and log_line opens the
    // file per line.
    // The cadence, not a single frame of it. Sampling one frame in 240 and
    // printing its count says almost nothing when the interesting ratios are
    // made of mostly-zeros with an occasional one: every sample landed on a
    // zero and the log looked like nothing was happening. Counting how many
    // frames took each value over the window shows the ratio directly, and it
    // is the thing being claimed.
    static int hist[7] = { 0, 0, 0, 0, 0, 0, 0 };
    // How evenly the frames are spaced, not just how many there are. The
    // average rate can be exactly right while delivery is not: at 1.25x three
    // frames in four go out at the base interval and the fourth carries two,
    // which is a stutter the frame counter cannot show. That would separate
    // 2.5x -- cadence 1,2,1,2 -- from 1.25x -- cadence 0,0,0,1 -- and it is a
    // different problem from cost.
    static int spread[5] = { 0, 0, 0, 0, 0 };
    if (g_token_dt > 0.0 && g_last_dt > 0.0) {
        const double r = g_last_dt / g_token_dt;
        const int b = r < 0.6 ? 0 : r < 0.85 ? 1 : r < 1.15 ? 2 : r < 1.4 ? 3 : 4;
        ++spread[b];
    }
    static int beat = 0;
    if (n >= 0 && n <= 6) ++hist[n];
    if (++beat >= 240) {
        beat = 0;
        int total = 0, frames = 0;
        for (int i = 0; i <= 6; ++i) { total += i * hist[i]; frames += hist[i]; }
        log_num("fractional: over the last N rendered frames ", (unsigned)frames);
        log_num("  ratio asked x100 ", (unsigned)(int)((per_frame + 1.0) * 100.0));
        log_num("  ratio produced x100 ",
                (unsigned)(frames > 0 ? (unsigned)((total + frames) * 100 / frames) : 0));
        for (int i = 0; i <= 6; ++i)
            if (hist[i] > 0) {
                log_num("    frames generating this many: ", (unsigned)i);
                log_num("      how many such frames ", (unsigned)hist[i]);
            }
        {
            static const char *kName[5] = {
                "    much shorter than average ", "    shorter ",
                "    about right ", "    longer ", "    much longer " };
            int tot = 0;
            for (int i = 0; i < 5; ++i) tot += spread[i];
            log_num("  frame-time spread, out of ", (unsigned)tot);
            for (int i = 0; i < 5; ++i)
                if (spread[i] > 0) log_num(kName[i], (unsigned)spread[i]);
            for (int i = 0; i < 5; ++i) spread[i] = 0;
        }
        log_num("  token fps (raw) ", (unsigned)(int)g_token_fps);
        log_num("  presented fps (what you should see) ",
                (unsigned)(int)(g_token_fps * (1.0 + g_produced_avg)));
        log_num("  multiplier x100 ", (unsigned)g_dyn_target);
        for (int i = 0; i <= 6; ++i) hist[i] = 0;
    }
}

// Re-reading the selection while the game runs, so a target change can be
// measured rather than deduced.
//
// The goal asks that a new target show up in the next window, and the only way
// to see that was for a person to move the panel slider mid-session. The file
// is the same one the panel writes, so re-reading it makes the bench able to
// change the target during a run and time how long the counted ratio takes to
// follow. Behind mfg-watch.txt: it is a file open per interval, which nothing
// shipped should be doing.
static bool g_watch_settings = false;
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

static unsigned hk_slGetNewFrameToken(void *&tok, const unsigned *idx) {
    // Time inside the original frame-token call, same method as the Present
    // measurement. Present accounts for 2% of the producer's time and Reflex
    // is ruled out, so the ~74 ms per frame is somewhere else; this is the
    // other place the producer passes through us.
    unsigned r;
    {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        r = g_orig_frametoken(tok, idx);
        QueryPerformanceCounter(&b);
        if (g_qpc_freq > 0)
            g_token_block_us += (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)g_qpc_freq;
    }
    // One rendered frame, exactly once.
    //
    // The game asks for the frame token about seven times per frame -- 305.7
    // calls against 45 counted frames -- and everything below used to run on
    // every one of those calls: the frame counter, the present index, the
    // block clock, the override. Every attempt to regroup the burst was a
    // timing threshold, and the gap histogram shows no threshold can work:
    // 38.7 gaps land in 4-8 ms and 7.3 in 2-4 ms, and a frame boundary is not
    // separable from a hiccup inside a burst. That is why the generation-off
    // control read 0.96 instead of 1.00, and every sub-2x number was scaled by
    // it.
    //
    // Streamline states the frame's identity: repeat calls for one frame carry
    // the same index and hand back the same token. Comparing that is exact and
    // needs no threshold.
    {
        // Se acepta un frame cuando su indice SUPERA al maximo aceptado, no
        // cuando difiere del de la llamada anterior.
        //
        // Con una sola fuente las dos reglas dan lo mismo: 1,1,2,2,3 acepta
        // 1,2,3. Con dos fuentes intercaladas no. Cyberpunk mapea dos copias de
        // sl.dlss_g y cada una lleva su propio indice, asi que comparar con la
        // llamada anterior daba distinto casi siempre. Medido por ventana de
        // juego: 45 pasaban el gate -- 30 con salto +1 y 15 con salto ATRAS --
        // contra una base real de Reflex de 41/s. La cuenta salia 128.9/s, 3.1
        // veces la real, y eso rompio el multiplicador contado, el aprendizaje
        // del sesgo de DYNAMIC y probablemente la dispersion.
        //
        // La prueba de que quedo bien es que "salto atras" caiga a cero en juego.
        //
        // Cuarta cosa que rompia, y la mas traicionera porque parecia fisica:
        // pedir 2.50 en Cyberpunk entregaba 2.63x y 2.55x, un +2 a +5% que no
        // aparecia sobre la pila 2.13 en el banco. Con el gate arreglado da
        // 2.48x y 2.50x. No era el esquema fraccionario: la ventana son 45
        // frames del gate, y cuando el gate cuenta triple esos 45 cubren unos
        // 15 frames reales, con el sesgo colandose en la mediana.
        static unsigned max_idx = 0;
        static void *last_tok = nullptr;
        static bool have = false;
        static unsigned last_idx = 0;   // solo diagnostico
        bool same;
        if (idx != nullptr) {
            const unsigned v = *idx;
            // Un indice muy por debajo del maximo no es una revisita sino un
            // reinicio -- swapchain nuevo, o vuelta de cero -- y sin esto el
            // gate quedaria trabado para siempre.
            if (have && v + 1000u < max_idx) { have = false; }
            same = have && v <= max_idx;
            if (!same) max_idx = v;
        } else {
            same = have && tok == last_tok;
        }
        // Diagnostico: last_idx ya no decide nada, solo mide la forma del
        // salto entre llamadas consecutivas para poder ver el intercalado.
        // Antes el gate suponia que las llamadas de un frame llegan
        // seguidas. Cyberpunk emite desde siete hilos a la vez, y si dos frames
        // se intercalan la comparacion contra la llamada ANTERIOR da distinto
        // siempre y pasan todas. Ahi el gate contaria 113/s con una base real
        // de 39. Se cuenta la forma del salto para saber si es eso.
        if (idx != nullptr && have) {
            const unsigned d = *idx - last_idx;      // sin signo, a proposito
            if (*idx == last_idx)      ++g_paso_igual;
            else if (d == 1u)          ++g_paso_uno;
            else if (d < 0x80000000u)  ++g_paso_salta;
            else                       ++g_paso_atras;
        }
        last_idx = (idx != nullptr) ? *idx : last_idx;
        last_tok = tok;
        have = true;
        ++g_token_calls;
        if (same) return r;
        ++g_frames_gated;
    }
    note_rendered_frame();
    g_game_set_this_frame = 0;      // a new frame; the last one is settled
    reflex_poll();
    // The present index advances here, with the frame, and not in
    // set_count_now: that only runs in fractional mode, so the counter feeding
    // *every* present sat at zero whenever the cadence was constant. A control
    // run at 2.00x then reported 1414 out-of-order drops where the unpatched
    // plugin had none -- the patch was manufacturing the very failure it was
    // written to remove. The stride matches the block the original arithmetic
    // reserved per frame.
    // Muerto en la configuracion por defecto, y conviene saberlo.
    //
    // g_pidx solo se asigna dentro de patch_monotonic_index, que vive en la
    // rama sin wic de patch_subframe_count -- despues del return de la rama
    // con wic, que es la que corre siempre. Con wic activo el puntero es nulo
    // y esta linea no hace nada.
    //
    // Importa porque se "arreglo" en esta sesion para que avanzara una vez por
    // frame en vez de 41, y ese arreglo se reporto como hallazgo. No cambio
    // nada: la compuerta por indice de frame si era real, esta linea no.
    //
    // Y patch_monotonic_index esta apagado por una razon medida, escrita sobre
    // su llamada: con el puesto, un 2.00x normal renderiza 30 y presenta 30
    // -- cero frames generados -- con 538 "Out of order frame" en sl.log. No
    // es el arreglo del desorden; lo empeora dos ordenes de magnitud.
    // Se evaluo sacarla en la auditoria y se decidio dejarla. Es codigo muerto
    // en la configuracion por defecto, si, pero el costo es UNA comparacion por
    // frame contra un puntero que siempre es nulo: el predictor la acierta
    // siempre y no aparece en ninguna medicion. Y sacarla romperia en silencio
    // el camino sin wic, donde patch_monotonic_index si puede asignar el
    // puntero si alguien pone mfg-monoidx.txt. Basura, no costo.
    if (g_pidx != nullptr) *g_pidx += 6;
    // Make the frame cost something, from mfg-slowframe.txt (milliseconds).
    //
    // The bench renders 1280x720 of almost nothing, so it is refresh-bound in
    // every configuration and the pacer divides that ceiling -- which is the
    // entire "base pinned at refresh/(ceiling + 1)" result, and it does not
    // happen in a real game. GTA V holds base 60/55/57 across 2.00x/2.50x/3.00x
    // and 2.50x presents more than 2.00x.
    //
    // What has to be reproduced is not GPU load specifically: it is a base rate
    // set by the application's own work rather than by the present ceiling. A
    // spin does that, costs ten lines, and needs nothing from the sample --
    // which is just as well, because -GpuLoad is not one of its options and
    // 400, 8000 and 32000 all render 164.
    if (g_slow_frame_us > 0 && g_qpc_freq > 0) {
        int us = g_slow_frame_us;
        if (g_slow_frame_us2 > 0 && g_slow_step_ms > 0) {
            static ULONGLONG t0 = 0;
            if (t0 == 0) t0 = GetTickCount64();
            const ULONGLONG el = GetTickCount64() - t0;
            if (((el / (ULONGLONG)g_slow_step_ms) & 1ull) != 0ull)
                us = g_slow_frame_us2;
        }
        // Jitter, from mfg-jitter.txt as a percentage.
        //
        // 57 archived bench runs -- 21 of them at 2.50x -- record zero
        // "Out of order frame", while a single GTA V session records three.
        // The bench renders on a fixed delta and the game does not, so the
        // first thing to try is a frame time that moves.
        if (g_jitter_pct > 0) {
            static unsigned seed = 22695477u;
            seed = seed * 1103515245u + 12345u;
            const int span = us * g_jitter_pct / 100;
            us += (int)(seed >> 16) % (2 * span + 1) - span;
            if (us < 100) us = 100;
        }
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        const long long want = (long long)us * g_qpc_freq / 1000000;
        do { QueryPerformanceCounter(&b); } while (b.QuadPart - a.QuadPart < want);
    }
    settings_watch();
    query_state();
    fractional_tick();
    apply_override_now();
    return r;
}

// Put in place the first time the player picks something, and never before.
// A game nobody opens the panel in runs with nothing of ours in its per-frame
// path, which is how it was before the override existed.
static void arm_dxgi_recorder();

static void arm_frametoken_hook(void) {
    // The swap chain hook comes with it. Counting presents is the only
    // non-circular way to measure the multiplier, and the DXGI chain was armed
    // only from the key-polling loop -- which never runs under the test bench,
    // so every measurement there had to be inferred instead of counted.
    arm_dxgi_recorder();
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

// Remembered across runs: the mode picked in the panel and, for DYNAMIC, the
// frame-rate target. Nothing else -- the flag files are a separate thing and
// are not rewritten from here, so a file the player created by hand is never
// silently replaced by one of ours.
static volatile LONG g_settings_dirty = 0;

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
    // Two keys, each followed by a number. Anything unrecognised is ignored
    // rather than treated as zero: a truncated write must not silently turn
    // into "AUTO, target nothing".
    for (DWORD i = 0; i < n; ++i) {
        const bool is_mode = (i + 5 < n) && buf[i] == 'm' && buf[i+1] == 'o' &&
                             buf[i+2] == 'd' && buf[i+3] == 'e';
        const bool is_dfps = (i + 7 < n) && buf[i] == 'd' && buf[i+1] == 'y' &&
                             buf[i+2] == 'n' && buf[i+3] == 'f' &&
                             buf[i+4] == 'p' && buf[i+5] == 's' && buf[i+6] == ' ';
        const bool is_hud  = (i + 4 < n) && buf[i] == 'h' &&
                             buf[i+1] == 'u' && buf[i+2] == 'd' &&
                             buf[i+3] == ' ';
        const bool is_tgt  = (i + 7 < n) && buf[i] == 't' && buf[i+1] == 'a' &&
                             buf[i+2] == 'r' && buf[i+3] == 'g' && buf[i+4] == 'e' &&
                             buf[i+5] == 't';
        if (is_dfps) {
            int v = 0; size_t j = i + 7;
            while (j < n && buf[j] >= '0' && buf[j] <= '9')
                v = v * 10 + (buf[j++] - '0');
            if (v >= 0 && v <= 1000) g_dyn_fps = v;
            i = j;
            continue;
        }
        if (is_hud) {
            g_hud_on = buf[i+4] == '1';
            i += 4;
            continue;
        }
        if (!is_mode && !is_tgt) continue;
        DWORD j = i + (is_mode ? 4 : 6);
        while (j < n && (buf[j] == ' ' || buf[j] == '=')) ++j;
        if (j >= n || buf[j] < '0' || buf[j] > '9') { i = j; continue; }
        int v = 0;
        while (j < n && buf[j] >= '0' && buf[j] <= '9' && v < 100000)
            v = v * 10 + (buf[j++] - '0');
        if (is_mode) {
            if (v >= 0 && v < kPanRows) {
                g_force_sel = v;
                g_force_generated = (v >= 2 && v <= kSelMaxFixed) ? v - 1 : 0;
                // DYNAMIC has to start somewhere. Restored from disk it landed
                // on zero generated frames, which asks the plugin to turn
                // generation on and produce none -- so DLSS-G never started,
                // and the controller cannot measure a base rate without it
                // running. It waited for itself. One generated frame is the
                // seed; the controller moves off it on the first measurement.
                if (v == kSelDynamic || v == kSelDynFuture)
                    g_force_generated = 1;
            }
        } else if (is_tgt && v >= 0 && v <= kMaxCustom) {
            // Un ajuste guardado por una version anterior puede traer 150. Se
            // sube al piso en vez de aceptarlo: el panel ya no ofrece ese valor.
            //
            // Con mfg-sub2.txt el piso baja a 110, que es lo mas chico que el
            // scheduler puede expresar sin que el estado bajo sea todo el ciclo.
            // Es para medir: el banco escribe el multiplicador por este mismo
            // archivo, asi que sin esto una corrida a --fractional 150 mide un
            // 2.00x plano y parece que anduvo. Paso exactamente eso una vez.
            const int piso = g_sub2 ? 110 : kMinCustom;
            g_dyn_target = v < piso ? piso : v;
        }
        i = j;
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


static void arm_slinit_temprano(void) {
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

    arm_slinit_temprano();
    if (false) {
        void *fi = (void *)GetProcAddress((HMODULE)base, "slInit");
        if (fi == nullptr) {
            log_line("  ! slInit no esta exportado aca");
        } else if (MH_CreateHook(fi, (void *)&hk_slInit,
                                 (void **)&g_orig_slinit) != MH_OK) {
            log_line("  ! no se pudo enganchar slInit");
            g_orig_slinit = nullptr;
        } else if (MH_EnableHook(fi) != MH_OK) {
            log_line("  ! no se pudo activar el enganche de slInit");
            g_orig_slinit = nullptr;
        } else {
            log_line("  slInit enganchado");
        }
    }

    // After MH_Initialize, not before it. Putting this above the init made
    // MH_CreateHook fail with "not initialised" and the else branch swallowed
    // it -- the same silent-failure shape fixed for slGetFeatureFunction
    // earlier today, reintroduced in the same function hours later. Every
    // branch says something now.
    g_frametoken_addr = (void *)GetProcAddress((HMODULE)base, "slGetNewFrameToken");
    if (g_frametoken_addr == nullptr) {
        log_line("  ! slGetNewFrameToken not exported");
    } else if (g_force_sel != 0 || g_frac_enabled) {
        // A selection restored from mfg-settings.txt is a selection: it needs
        // the same per-frame hook that picking one in the panel installs.
        // Without this the override was armed only by hand, so a saved DYNAMIC
        // came back looking chosen and did nothing at all.
        arm_frametoken_hook();
    } else {
        log_line("  slGetNewFrameToken found (hooked only if an override is picked)");
    }
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
    // Presented frames, counted. Nothing else in this file has ever counted
    // one, and that is why three separate multiplier instruments in one
    // session were all circular -- the last of them reduced to "what fraction
    // of my samples did I take while the high count was selected" and returned
    // the requested ratio to within 0.3% even at 2.10x and 2.90x, fractions its
    // own scheduler cannot represent. An instrument that cannot fail measures
    // nothing.
    InterlockedIncrement(&g_present_count);
    // The runtime's own tally, so our hook can be checked against something we
    // do not maintain. If these two agree, a shortfall of presents against
    // rendered frames is the frame counter's fault and not the hook's.
    {
        DXGI_FRAME_STATISTICS a;
        if (self != nullptr && SUCCEEDED(self->GetFrameStatistics(&a)))
            g_rt_present_count = (LONG)a.PresentCount;
    }
    // Queue latency: how long a present waits before the panel shows it.
    //
    // This is the half of "latency" the swap chain can answer. PresentCount is
    // how many presents the runtime took and PresentRefreshCount which refresh
    // the last one was scanned at, so their gap is how many frames are waiting.
    // What this cannot answer is input to photon -- that needs the sample to
    // timestamp input, which it does not, and PresentMon, which is starved on
    // this machine.
    //
    // The comparison that matters: a fractional ratio spends whole blocks at
    // each integer count, and generated frames queue differently from real
    // ones. If the alternation adds queue depth, it shows up here.
    if (self != nullptr) {
        DXGI_FRAME_STATISTICS fs;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (SUCCEEDED(self->GetFrameStatistics(&fs)) && g_qpc_freq > 0) {
            // Age of the last scan-out at the moment this present is made.
            //
            // PresentCount and PresentRefreshCount are unrelated running totals
            // -- one counts presents, the other refreshes -- so subtracting them
            // is meaningless and read 0.00 on every configuration, which is how
            // a broken metric announces itself. SyncQPCTime is a real clock: the
            // instant the panel last scanned out. How far behind it we are when
            // handing over a frame is the queue this can see.
            const double age = (double)(now.QuadPart - fs.SyncQPCTime.QuadPart)
                             / (double)g_qpc_freq * 1000.0;
            if (age >= 0.0 && age < 200.0) {
                g_lat_sum += age;
                ++g_lat_n;
                if ((int)age > g_lat_max) g_lat_max = (int)age;
            }
        }
    }
    // Present pacing, from the same hook. The multiplier says how many frames
    // reach the screen; this says whether they arrive evenly, which is the half
    // of the question a player actually feels. Histogram rather than a mean:
    // block alternation is expected to produce two populations, and an average
    // would hide exactly that.
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (g_pres_qpc != 0 && g_qpc_freq > 0) {
            const double dt = (double)(t.QuadPart - g_pres_qpc) / (double)g_qpc_freq;
            if (dt > 0.0 && dt < 0.500) {
                const double ms = dt * 1000.0;
                g_pres_ms_sum += ms;
                ++g_pres_n;
                if (ms > g_pres_ms_max) g_pres_ms_max = ms;
                // Buckets in milliseconds: under 4, 4-8, 8-12, 12-20, 20-33,
                // over 33. A 165 Hz panel refreshes every 6.06 ms.
                const int b = ms < 4.0 ? 0 : ms < 8.0 ? 1 : ms < 12.0 ? 2
                            : ms < 20.0 ? 3 : ms < 33.0 ? 4 : 5;
                ++g_pres_bucket[b];
                // Where the throughput actually goes, conditioned on distance
                // from a block boundary.
                //
                // The cost is not the six intervals over 33 ms; counting those
                // is a thousand times less sensitive than the intervals
                // themselves, which is how a ten percent throughput loss was
                // read as "no measurable cost". At 2.50x, 7.7 percent of
                // presents arrive under 4 ms after the previous one -- inside a
                // 6.06 ms refresh, so overwritten -- and 29.7 percent arrive
                // late, against 1.0 and 2.1 for an integer count. That is
                // burst-and-wait, thousands of intervals, and it costs 149.7
                // presented fps against 166.7.
                //
                // If the count change causes it, the bad intervals cluster in
                // the presents just after one. If they are spread evenly, the
                // cost is in running a varying count at all and there is
                // nothing at the boundary to fix.
                {
                    ++g_all_n;
                    if (b == 0 || b >= 2) ++g_all_bad;
                    g_all_ms += ms;
                    const LONG since = g_present_count - g_last_change_pres;
                    const bool bad = (b == 0 || b >= 2);
                    if (since < 8) { ++g_near_n; if (bad) ++g_near_bad; }
                    else           { ++g_far_n;  if (bad) ++g_far_bad; }
                }
                if (ms > 33.0) {
                    ++g_pres_hitch;
                    // How many presents ago the count last changed. Zero means
                    // this hitch is the change itself.
                    const LONG since = g_present_count - g_last_change_pres;
                    if (since < 4) ++g_hitch_near;
                    else ++g_hitch_far;
                }
            }
        }
        g_pres_qpc = t.QuadPart;
    }
    note_present(nullptr, 2);
    if (g_recording != 0) note_display(self);
    // Diagnostic only, behind mfg-novsync.txt.
    //
    // Every throughput comparison in this file was taken against a 165 Hz
    // display that 2.00x already saturates, so nothing above it could show a
    // gain and the numbers could not answer the question. Worse, the pacer
    // settles the producer at refresh/(count+1), so a cadence whose ceiling is
    // 3 renders at 165/3 whatever its average is: 2.25x pays 3x's base and
    // returns 2.25x of it, 124 presented against 2.00x's 166. That reads as
    // "fractional is strictly worse" and it may be nothing but vsync.
    //
    // Forcing the interval to zero takes the refresh out of the loop and lets
    // the comparison come out either way.
    //
    // interval 0 on its own changed nothing: base 165/83/55 and 165 presented,
    // identical to vsync on, because a flip-model swap chain still waits for
    // vblank unless the present asks to tear. So ask, and fall back if the
    // chain was not created allowing it -- Present returns DXGI_ERROR_INVALID_CALL
    // rather than tearing, and a failed present would break the run.
    if (g_novsync) {
        static int tearing = 1;          // 1 = try, 0 = chain refused it
        if (tearing) {
            const HRESULT hr = g_orig_dxgi_present(self, 0, flags | 0x00000200 /*ALLOW_TEARING*/);
            if (SUCCEEDED(hr)) return hr;
            tearing = 0;
            log_line("novsync: the swap chain refuses tearing; the refresh cap stays");
        }
        interval = 0;
    }
    // How long the producer is blocked inside Present, per window.
    //
    // One number that separates the two remaining candidates for the throttle.
    // If the app spends essentially the whole window inside this call, the gate
    // is presentation. If it comes back quickly, whatever holds the producer is
    // upstream of here and Present is not it.
    {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        const HRESULT hr = g_orig_dxgi_present(self, interval, flags);
        QueryPerformanceCounter(&b);
        if (g_qpc_freq > 0)
            g_present_block_us += (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)g_qpc_freq;
        return hr;
    }
}

// IDXGIDevice1::SetMaximumFrameLatency, clamped from the DXGI side.
//
// The throughput law says the producer runs at refresh/(ceiling + 1), and the
// frame latency is the only thing measured to track the count (0 -> 1, 1 -> 2,
// 2 -> 3). Patching the plugin's own register load to pin it kills the run
// outright -- no measurement window at all, three of three. Hooking the DXGI
// method instead leaves every instruction in the plugin running and changes
// only the number that reaches the runtime, which is a much smaller thing to
// be wrong about.
//
// Slot 12: IUnknown 0-2, IDXGIObject 3-6, IDXGIDevice 7-11, then
// SetMaximumFrameLatency. 12 * 8 = 0x60, matching the `call qword ptr [rax+0x60]`
// the pin patch was written against.
typedef HRESULT (STDMETHODCALLTYPE *PFN_SMFL)(IUnknown *, UINT);
static PFN_SMFL g_orig_smfl = nullptr;
static HRESULT STDMETHODCALLTYPE hk_smfl(IUnknown *self, UINT n) {
    InterlockedIncrement(&g_smfl_calls);
    if (g_clamp_latency > 0 && (int)n > g_clamp_latency) n = (UINT)g_clamp_latency;
    return g_orig_smfl(self, n);
}
static void hook_frame_latency(void *sc) {
    if (g_clamp_latency <= 0 || g_orig_smfl != nullptr || sc == nullptr) return;
    // IDXGISwapChain2, not IDXGIDevice1. The first attempt asked the swap chain
    // for IDXGIDevice1 and got nothing: that interface is a D3D11 device
    // concept, and this is D3D12. On D3D12 SetMaximumFrameLatency lives on the
    // swap chain itself.
    //
    // Slot 31: IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7,
    // IDXGISwapChain 8-17, IDXGISwapChain1 18-28, then SetSourceSize 29,
    // GetSourceSize 30, SetMaximumFrameLatency 31.
    IUnknown *chain = reinterpret_cast<IUnknown *>(sc);
    IUnknown *sc2 = nullptr;
    // {a8be2ac4-199f-4946-b331-79599fb98de7} IDXGISwapChain2
    GUID iid = { 0xa8be2ac4, 0x199f, 0x4946,
                 { 0xb3, 0x31, 0x79, 0x59, 0x9f, 0xb9, 0x8d, 0xe7 } };
    if (FAILED(chain->QueryInterface(iid, reinterpret_cast<void **>(&sc2))) || sc2 == nullptr) {
        log_line("  clamp: no IDXGISwapChain2 either -- frame latency is not reachable here");
        return;
    }
    void **vt = *reinterpret_cast<void ***>(sc2);
    DWORD prot = 0;
    if (VirtualProtect(&vt[31], sizeof(void *), PAGE_READWRITE, &prot)) {
        g_orig_smfl = reinterpret_cast<PFN_SMFL>(vt[31]);
        vt[31] = reinterpret_cast<void *>(&hk_smfl);
        VirtualProtect(&vt[31], sizeof(void *), prot, &prot);
        log_num("  frame latency clamped to ", (unsigned)g_clamp_latency);
    }
    sc2->Release();
}

static void hook_swapchain_present(void *sc) {
    if (sc == nullptr || g_orig_dxgi_present != nullptr) return;
    void **vt = *reinterpret_cast<void ***>(sc);
    // The slot, not the bytes. This used to call MH_CreateHook on vt[8], which
    // detours the Present implementation itself -- the one thing this project
    // has a standing rule against, because Steam's overlay detours the same
    // bytes and whoever installs second wins.
    //
    // It cost a measurement. In one 1.50x run our counter read exactly 45
    // presents in 86 of 86 windows while the runtime's own PresentCount
    // advanced 66 to 69 in each, so the run reported 1.00 where the runtime
    // said 1.49. The failure is silent and reads as a clean integer, which at
    // 1.10x would be indistinguishable from a correct answer.
    DWORD prot = 0;
    if (!VirtualProtect(&vt[8], sizeof(void *), PAGE_READWRITE, &prot)) return;
    g_orig_dxgi_present = reinterpret_cast<PFN_DXGIPresent>(vt[8]);
    g_swapchain = reinterpret_cast<IDXGISwapChain *>(sc);
    hook_frame_latency(sc);
    // What the swap chain was created with, and whether the app takes the
    // waitable handle.
    //
    // 98% of the producer's time is spent outside both of our hooks -- 2% in
    // Present, 0% in the frame-token call -- so whatever holds it is in the
    // application's own loop. A swap chain created with
    // DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT (0x800) is waited on
    // with WaitForSingleObject before the frame starts, which is exactly the
    // shape of a gate that never touches us.
    {
        DXGI_SWAP_CHAIN_DESC sd;
        if (SUCCEEDED(g_swapchain->GetDesc(&sd))) {
            log_num("  swap chain flags ", (unsigned)sd.Flags);
            log_num("  buffers ", (unsigned)sd.BufferCount);
            log_line((sd.Flags & 0x40u)
                     ? "  this chain IS frame-latency waitable"
                     : "  this chain is not waitable");
        }
    }
    vt[8] = reinterpret_cast<void *>(&hk_dxgi_present);
    VirtualProtect(&vt[8], sizeof(void *), prot, &prot);
    log_line("recorder: present slot swapped (vt[8], not detoured)");
}

// Strip FRAME_LATENCY_WAITABLE_OBJECT at creation, behind mfg-nowaitable.txt.
//
// The producer spends 98% of its time outside both hooks -- 2% in Present, 0%
// in the frame-token call -- and the swap chain turns out to be created with
// flag 0x800 and 3 buffers. An app with a waitable swap chain blocks on that
// handle before starting a frame, which is a gate that never passes through
// us. Taking the flag away is the measurement that says whether it is the one
// holding the base at limit/(ceiling + 1).
//
// Diagnostic only. An app that asks for the waitable handle on a chain created
// without the flag gets a failure, so this can break the target outright --
// which is itself an answer, and the integer controls will say so.
static bool g_no_waitable = false;
static UINT strip_waitable(UINT flags) {
    if (!g_no_waitable) return flags;
    return flags & ~0x40u;
}

typedef HRESULT(STDMETHODCALLTYPE *PFN_CSC)(IDXGIFactory *, IUnknown *,
        DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CSCFH)(void *, IUnknown *, HWND, const void *,
        const void *, void *, IDXGISwapChain **);
static PFN_CSC g_orig_csc = nullptr;
static PFN_CSCFH g_orig_cscfh = nullptr;

static HRESULT STDMETHODCALLTYPE hk_csc(IDXGIFactory *self, IUnknown *dev,
        DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **out) {
    if (desc != nullptr) {
        log_num("  CreateSwapChain asked for flags ", (unsigned)desc->Flags);
        log_line((desc->Flags & 0x40u) ? "    WAITABLE requested" : "    not waitable");
        if (desc->Flags & 0x800u) log_line("    ALLOW_TEARING requested");
    }
    if (g_no_waitable && desc != nullptr && (desc->Flags & 0x40u)) {
        desc->Flags = strip_waitable(desc->Flags);
        log_line("  waitable flag stripped at creation (mfg-nowaitable.txt)");
    }
    HRESULT hr = g_orig_csc(self, dev, desc, out);
    if (SUCCEEDED(hr) && out != nullptr) hook_swapchain_present(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_cscfh(void *self, IUnknown *dev, HWND hwnd,
        const void *d1, const void *fs, void *restrict_to, IDXGISwapChain **out) {
    // La ultima gana, no la primera. Cyberpunk recrea la ventana al cambiar de
    // modo de video, y engancharse a la primera dejaba un HWND viejo: el
    // indicador de foco daba 0 con el juego al frente y descarto 196 ventanas
    // buenas de una medicion. Solo se usa para loguear, asi que no cambia
    // ningun comportamiento.
    if (g_game_hwnd != hwnd) {
        g_game_hwnd = hwnd;
        log_line("swapchain con ventana nueva");
    }
    // DXGI_SWAP_CHAIN_DESC1 keeps Flags as the last UINT of the struct, after
    // Width, Height, Format, Stereo, SampleDesc(2), BufferUsage, BufferCount,
    // Scaling, SwapEffect, AlphaMode -- offset 0x2C.
    // DXGI_SWAP_CHAIN_DESC1 is not declared in this translation unit, and it
    // does not need to be: Flags is the trailing UINT at offset 0x2C, after
    // Width, Height, Format, Stereo, SampleDesc(2), BufferUsage, BufferCount,
    // Scaling, SwapEffect and AlphaMode.
    // What the caller asked for, before anyone else touches it.
    //
    // Upstream Donut -- which this sample is built on -- does not set
    // FRAME_LATENCY_WAITABLE_OBJECT at all; it sets ALLOW_TEARING and waits on
    // its own per-buffer fences. Yet the swap chain we end up with reports flag
    // 0x800. So either the sample modified Donut, or Streamline's interposer
    // adds it. That distinction decides whether the gate belongs to the bench
    // or to Streamline -- and if it is Streamline's, it applies to every game.
    if (d1 != nullptr) {
        const UINT f = *reinterpret_cast<const UINT *>(
                            reinterpret_cast<const unsigned char *>(d1) + 0x2C);
        // 0x40 is FRAME_LATENCY_WAITABLE_OBJECT. 0x800 is ALLOW_TEARING, and
        // reading one for the other is what made the first pass conclude the
        // app used a waitable chain: it reported flags 2048, which is tearing
        // alone -- exactly what upstream Donut sets. Stripping "0x800" then
        // took ALLOW_TEARING away from a fullscreen chain and killed the app,
        // which was read as evidence for the wrong thing.
        log_num("  CreateSwapChainForHwnd asked for flags ", (unsigned)f);
        log_line((f & 0x40u) ? "    WAITABLE requested"
                             : "    not waitable");
        if (f & 0x800u) log_line("    ALLOW_TEARING requested");
        {
            static int n = 0;
            log_num("    swap chain number ", (unsigned)(++n));
        }
    }
    unsigned char copy[0x30];
    if (g_no_waitable && d1 != nullptr) {
        for (int i = 0; i < 0x30; ++i)
            copy[i] = reinterpret_cast<const unsigned char *>(d1)[i];
        UINT *pf = reinterpret_cast<UINT *>(copy + 0x2C);
        if (*pf & 0x40u) {
            *pf = strip_waitable(*pf);
            log_line("  waitable flag stripped at creation (mfg-nowaitable.txt)");
            d1 = copy;
        }
    }
    HRESULT hr = g_orig_cscfh(self, dev, hwnd, d1, fs, restrict_to, out);
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

static bool g_debug = false;      // mfg-debug.txt: developer diagnostics

static void arm_dxgi_recorder() {
    if (!g_debug) return;
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

// One line per run, cheap enough to leave on in quiet mode.
static void log_pacing_summary() {
    if (g_all_n < 200) return;
    log_num("SUMMARY presents ", (unsigned)g_all_n);
    log_num("  present ms avg x100 ", (unsigned)(int)(g_all_ms / (double)g_all_n * 100.0));
    if (g_lat_n > 0) {
        log_num("  present-to-scanout ms x100 ",
                (unsigned)(int)(g_lat_sum / (double)g_lat_n * 100.0));
        log_num("  worst ms ", (unsigned)g_lat_max);
        g_lat_sum = 0.0;
        g_lat_n = 0;
        g_lat_max = 0;
    }
    log_num("  off-refresh x1000 ",
            (unsigned)((unsigned long long)g_all_bad * 1000ULL / (unsigned)g_all_n));
    g_all_n = 0; g_all_bad = 0; g_all_ms = 0.0;
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
    int pacing_ticks = 0;
    for (;;) {
        // Faster while the panel is up: this thread owns the panel window, so
        // the pointer only moves as often as it comes round.
        Sleep(g_ov_visible ? 8 : 50);

        // Streamline may already be mapped before this dll attaches -- a game
        // that imports sl.interposer statically gives the loader nothing to
        // notify us about -- so the arming is retried here until it takes.
        //
        // It doubles as the test for whether this process is the one running
        // the game. Games ship helper executables beside themselves, and they
        // load a version.dll sitting next to them exactly as the game does:
        // GTA V's error reporter did, and because every key here is read with
        // GetAsyncKeyState, which is system-wide, it opened a second panel of
        // its own on top of the real one. Every panel line in the log appeared
        // twice, and the window the player was clicking belonged to a process
        // with no DLSS-G in it -- so DYNAMIC was greyed and the modes changed
        // nothing. A process without sl.interposer is not the renderer.
        if (g_orig_getfeaturefn == nullptr) {
            HMODULE si = GetModuleHandleW(L"sl.interposer.dll");
            if (si != nullptr) arm_multiplier_override((unsigned char *)si);
        }
        const bool is_renderer = GetModuleHandleW(L"sl.interposer.dll") != nullptr;

        // The panel. Every input here is polled -- GetAsyncKeyState reads
        // system key state directly, so it does not depend on message routing,
        // on focus, or on the game delivering anything. That is the one input
        // path in this project that has never failed.
        if (g_ov_enabled && is_renderer) {
            static bool tilde_was = false, lmb_was = false, dragging = false;
            const bool t   = (GetAsyncKeyState(VK_OEM_3) & 0x8000) != 0;   // `
            const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;

            if (esc && g_ov_visible) {
                // Escape backs out one step: an edit in progress first, the
                // panel only once there is nothing else to leave.
                if (g_ov_editing) ov_edit_cancel();
                else { g_ov_visible = false; ov_show(false); }
            }
            if (t && !tilde_was) {
                g_ov_visible = !g_ov_visible;
                ov_show(g_ov_visible);
            }
            tilde_was = t;

            // Fuera del bloque de abajo a proposito: el HUD no depende de que
            // el panel este abierto.
            hud_tick();
            if (InterlockedExchange(&g_twocopies_pending, 0) != 0) {
                wchar_t ruta[MAX_PATH];
                beside_dll(ruta, L"mfg-twocopies.txt");
                HANDLE h2 = CreateFileW(ruta, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h2 != INVALID_HANDLE_VALUE) {
                    char b2[MAX_PATH * 2];
                    DWORD g2 = 0;
                    if (ReadFile(h2, b2, sizeof(b2) - 1, &g2, nullptr) && g2 > 4) {
                        b2[g2] = 0;
                        for (DWORD k2 = 0; k2 < g2; ++k2)
                            if (b2[k2] == 13 || b2[k2] == 10) { b2[k2] = 0; break; }
                        wchar_t w2[MAX_PATH * 2];
                        if (MultiByteToWideChar(CP_UTF8, 0, b2, -1, w2, MAX_PATH * 2) > 0) {
                            log_line("banco: cargando una SEGUNDA copia a proposito");
                            log_line(b2);
                            if (LoadLibraryW(w2) == nullptr)
                                log_num("  no se pudo cargar, error ",
                                        (unsigned)GetLastError());
                        }
                    }
                    CloseHandle(h2);
                }
            }

            if (g_ov_visible) {
                const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                // El slider vuelve solo para DYNAMIC, y mueve fps, no ratio.
                const bool onslider = g_ov_hot == kHotSlider &&
                                     g_force_sel == kSelDynFuture;
                const bool onvalue  = g_ov_hot == kHotValue;
                const bool onhud    = g_ov_hot == kHotHud;
                if (onhud && lmb && !lmb_was) {
                    g_hud_on = !g_hud_on;
                    log_num("panel: HUD ", (unsigned)(g_hud_on ? 1 : 0));
                    g_settings_dirty = 1;
                }
                const bool row_ok =
                    (g_ov_hot == kSelDynFuture) ? true
                  : (g_ov_hot == kSelDynamic) ? true
                  : (g_ov_hot < 2 || g_frames_max == 0 ||
                     (LONG)(g_ov_hot - 1) <= g_frames_max);

                if (lmb && !lmb_was) {
                    if (onvalue) {
                        ov_edit_begin();
                    } else {
                        // Any other click ends an edit rather than abandoning
                        // it half-finished: what was typed is what was meant.
                        const int typed = ov_edit_commit();
                        if (typed >= 0 && typed != g_dyn_target) {
                            g_dyn_target = typed;
                            g_dyn_said = 0;
                            arm_frametoken_hook();
                            g_opt_pending = 1;
                            log_num("panel: target typed ", (unsigned)typed);
                            g_settings_dirty = 1;
                        }
                        if (onslider) {
                            dragging = true;
                        } else if (g_ov_hot >= 0 && g_ov_hot < kPanRows && row_ok) {
                            g_force_sel = g_ov_hot;
                            g_force_generated = g_ov_hot >= 2 ? g_ov_hot - 1 : 0;
                            arm_frametoken_hook();
                            g_opt_pending = 1;
                            g_override_said = false;
                            log_num("panel: mode now ", (unsigned)g_ov_hot);
                            g_settings_dirty = 1;
                        }
                    }
                }
                if (!lmb) dragging = false;
                if (dragging) {
                    const LONG v = (LONG)kFpsStops[ov_fps_at(g_ov_mx)];
                    if (v != g_dyn_fps) {
                        g_dyn_fps = v;
                        g_dyn_said = 0;
                        arm_frametoken_hook();
                        g_opt_pending = 1;
                        log_num("panel: target now ", (unsigned)v);
                        g_settings_dirty = 1;
                    }
                }
                lmb_was = lmb;

                if (g_ov_editing) {
                    // Punto y coma, del teclado y del bloque numerico: la
                    // coma porque en este teclado es lo que cae al escribir
                    // un decimal, y ov_edit_digit la normaliza a punto.
                    static bool dwas[15] = { false };
                    static const int vks[15] = { '0','1','2','3','4','5','6','7','8','9',
                                                 VK_BACK, VK_RETURN,
                                                 VK_DECIMAL, VK_OEM_PERIOD, VK_OEM_COMMA };
                    for (int k = 0; k < 15; ++k) {
                        const bool dn = (GetAsyncKeyState(vks[k]) & 0x8000) != 0 ||
                                        (k < 10 && (GetAsyncKeyState(VK_NUMPAD0 + k) & 0x8000) != 0);
                        if (dn && !dwas[k]) {
                            if (k < 10)                 ov_edit_digit((char)('0' + k));
                            else if (k >= 12)           ov_edit_digit('.');
                            else if (vks[k] == VK_BACK) ov_edit_back();
                            else {
                                const int typed = ov_edit_commit();
                                if (typed >= 0 && typed != g_dyn_target) {
                                    g_dyn_target = typed;
                                    g_dyn_said = 0;
                                    arm_frametoken_hook();
                                    g_opt_pending = 1;
                                    log_num("panel: target typed ", (unsigned)typed);
                            g_settings_dirty = 1;
                                }
                            }
                        }
                        dwas[k] = dn;
                    }
                }
                ov_tick();
            }

            // A moment after the last change, not on every one: dragging the
            // slider walks through a dozen stops and each would be a write.
            if (g_settings_dirty) {
                static int settle = 0;
                if (++settle > (g_ov_visible ? 60 : 10)) {
                    settle = 0;
                    g_settings_dirty = 0;
                    settings_save();
                }
            }
        }

        // The panel window lives on this thread, so WM_INPUT -- and with it
        // the pointer -- only arrives while this pumps.
        {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (g_debug && g_orig_present == nullptr) {   // Vulkan side, same rule
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
            continue;
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

        const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
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
        if (++ticks >= 20) { ticks = 0; if (g_recording) write_samples(); }
        // Every ~10 s from the polling thread, never from the render thread.
        if (++pacing_ticks >= 200) { pacing_ticks = 0; log_pacing_summary(); }
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
// 2.13.0 writes a zeroed register instead. Both forms are handled: the
// immediate loop below returns if it hits, and the register loop after it
// redirects the displacement. Verified statically with tools/verify_sites.py --
// 2.12.0 has exactly one immediate store at field 0x44f8 and no register one,
// 2.13.0 exactly one register store at 0x44f0 and no immediate.
//
// This comment used to say the 2.13.0 form was deliberately left unpatched,
// which the code right below it contradicted. A comment that lies about its
// own code is worse than none.
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

// Sacar los sitios de un modulo que se descarga.
//
// Con un puntero unico esto no hacia falta: la siguiente copia parcheada lo
// pisaba. Con una lista, un modulo descargado deja punteros colgados y la
// escritura por frame se convierte en una violacion de acceso. Los juegos SI
// descargan sl.dlss_g -- se vio en los logs de Cyberpunk -- asi que sin esto la
// lista es mas peligrosa que el puntero que reemplazo.
static void sitio_drop(volatile unsigned char **lista, int *n,
                       const unsigned char *base, size_t largo) {
    int w = 0;
    for (int i = 0; i < *n; ++i) {
        const unsigned char *q = (const unsigned char *)lista[i];
        const bool dentro = q >= base && q < base + largo;
        if (!dentro) lista[w++] = lista[i];
    }
    for (int i = w; i < *n; ++i) lista[i] = nullptr;
    *n = w;
}

static VOID CALLBACK on_dll_load(ULONG reason, const DllNotifyData *d, PVOID) {
    if (reason == 2 && d != nullptr) {                       // 2 = UNLOADED
        const unsigned char *b = (const unsigned char *)d->DllBase;
        const size_t largo = (size_t)d->SizeOfImage;
        const int antes = g_wic_n + g_imm_n + g_imm2_n + g_imm3_n;
        sitio_drop(g_wic_sitios,  &g_wic_n,  b, largo);
        sitio_drop(g_imm_sitios,  &g_imm_n,  b, largo);
        sitio_drop(g_imm2_sitios, &g_imm2_n, b, largo);
        sitio_drop(g_imm3_sitios, &g_imm3_n, b, largo);
        const int ahora = g_wic_n + g_imm_n + g_imm2_n + g_imm3_n;
        if (ahora != antes) {
            log_line("modulo descargado: se retiran sus sitios parcheados");
            log_num("  sitios que quedan ", (unsigned)ahora);
        }
        return;
    }
    if (reason != 1 || d == nullptr) return;                 // 1 = LDR_DLL_NOTIFICATION_REASON_LOADED

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
        if (!g_dynamic_known &&
            image_has((unsigned char *)d->DllBase, "sl::DLSSGMode::eDynamic")) {
            g_dynamic_known = true;
            log_line("  this copy knows DLSSGMode::eDynamic");
        }
        log_line("sl.dlss_g mapped");
        log_wide("  in ", d->FullDllName);
        // Marcar nada mas: esto corre bajo el loader lock y LoadLibrary desde
        // aca falla por diseno. La carga se hace en el camino de present.
        if (g_twocopies) g_twocopies_pending = 1;
        {
            const int sc = patch_subframe_count(reinterpret_cast<unsigned char *>(d->DllBase));
            log_num("  sub-frame count made writable, sites: ", (unsigned)sc);
        }
        const int n = patch_enable_cpu_pacer(reinterpret_cast<unsigned char *>(d->DllBase));
        g_outputs_patched += n;
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
            wchar_t pb[MAX_PATH];
            int j = 0;
            while (g_frames[j] != 0 && j < MAX_PATH - 1) { pb[j] = g_frames[j]; ++j; }
            while (j > 0 && pb[j - 1] != 0x5C) --j;
            g_debug = flag_file(L"mfg-debug.txt");
            // On unless refused, because the person who uses this has the dll
            // and nothing else. Everything measured tonight was measured with
            // mfg-frac.txt, mfg-slowalt.txt and mfg-wic.txt beside the dll --
            // three files that the shipped copy does not come with, so picking
            // DYNAMIC in the panel did nothing on its own.
            //
            // Nothing here starts generating by itself: the scheduler returns
            // immediately unless the panel is on DYNAMIC (see the guard on
            // g_force_sel), and the patch only makes its site writable. The
            // value that changes behaviour is written by the scheduler.
            //
            // The old names still work as opt-outs under mfg-nofrac.txt and
            // mfg-noslowalt.txt.
            // Opt-in, and the attempt to default it on is recorded here so it
            // is not tried again the same way.
            //
            // The person who uses this has the dll and nothing else, so picking
            // DYNAMIC in the panel ought to be enough, and everything measured
            // in this file was measured with mfg-frac.txt beside the dll. But
            // patch_subframe_count replaces the comparison against the count
            // field with one against an immediate byte, and only the fractional
            // scheduler keeps that byte equal to the API count. On an integer
            // selection the scheduler never runs, the byte goes stale, and a
            // bound below (API count + 1) stops presentation outright: 3.00x
            // read 1.000 with the override armed and applied zero times, three
            // runs out of three, rendering 165 as if nothing were enabled.
            //
            // Breaking 3x and 4x for someone who never asked for a fractional
            // ratio is worse than making them opt in. The real fix is to defer
            // the patch until DYNAMIC is first selected, so the site is found
            // at map time but only rewritten once something maintains it.
            // On unless refused, because the person who uses this has the dll
            // and nothing else: picking DYNAMIC in the panel has to be enough.
            //
            // The first attempt at this broke every integer selection. The
            // patch replaces the plugin's write of the count with an immediate,
            // and nothing filled that byte in unless the fractional scheduler
            // was running, so 3.00x read 1.000 with the override applied zero
            // times. Arming it by default was not the mistake; leaving the byte
            // without an owner was. fractional_tick now writes it for the fixed
            // rows too, and with the patch armed the integers read 1.000, 2.001
            // and 3.000 while 2.50x reads 2.501.
            //
            // Nothing generates by itself: the scheduler still returns before
            // any cadence work unless the panel is on DYNAMIC.
            g_watch_settings = flag_file(L"mfg-watch.txt");
            g_novsync = flag_file(L"mfg-novsync.txt");
            g_pin_latency = flag_file(L"mfg-pinlatency.txt");
            g_pace_follow = flag_file(L"mfg-pacefollow.txt");
            g_frac_enabled = !flag_file(L"mfg-nofrac.txt");
            g_sub2 = flag_file(L"mfg-sub2.txt");
            g_twocopies = flag_file(L"mfg-twocopies.txt");
            g_ceilfirst = flag_file(L"mfg-ceilfirst.txt");
            g_ota = flag_file(L"mfg-ota.txt");
            // Con la bandera puesta hay que llegar antes que la llamada del
            // juego, y el armado del hilo del panel llega tarde en los juegos
            // que importan el interposer estaticamente.
            if (g_ota) arm_slinit_temprano();
            g_slowalt = !flag_file(L"mfg-noslowalt.txt");
            g_quiet = flag_file(L"mfg-quiet.txt");
            g_nullalt = flag_file(L"mfg-nullalt.txt");
            {
                // A one-line integer beside the dll; the sweep needs to move
                // this without a rebuild.
                wchar_t bp[MAX_PATH];
                int bj = 0;
                while (g_log[bj] != 0 && bj < MAX_PATH - 1) { bp[bj] = g_log[bj]; ++bj; }
                while (bj > 0 && bp[bj - 1] != 0x5C) --bj;
                const wchar_t *bn = L"mfg-blockms.txt";
                for (int i = 0; bn[i] != 0; ++i) bp[bj + i] = bn[i];
                bp[bj + 15] = 0;
                HANDLE bh = CreateFileW(bp, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, 0, nullptr);
                if (bh != INVALID_HANDLE_VALUE) {
                    char buf[16] = { 0 };
                    DWORD got = 0;
                    if (ReadFile(bh, buf, 15, &got, nullptr) && got > 0) {
                        int v = 0;
                        for (DWORD i = 0; i < got && buf[i] >= '0' && buf[i] <= '9'; ++i)
                            v = v * 10 + (buf[i] - '0');
                        if (v > 0 && v < 100000) {
                            g_block_ms = v;
                            log_num("slowalt: block length from file, ms ", (unsigned)v);
                        }
                    }
                    CloseHandle(bh);
                }
            }
            g_peralt = flag_file(L"mfg-peralt.txt");
            g_optsv3 = flag_file(L"mfg-optsv3.txt");
            { wchar_t mp[MAX_PATH]; beside_dll(mp, L"mfg-markergap.txt");
              HANDLE mh = CreateFileW(mp, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
              if (mh != INVALID_HANDLE_VALUE) {
                  char b4[32]; DWORD r6 = 0;
                  if (ReadFile(mh, b4, sizeof(b4)-1, &r6, nullptr) && r6 > 0) {
                      b4[r6] = 0;
                      int v[4] = {0, 0, 0, 0}; int k = 0; DWORD i = 0;
                      while (i < r6 && k < 4) {
                          while (i < r6 && (b4[i] < '0' || b4[i] > '9')) ++i;
                          if (i >= r6) break;
                          int n2 = 0;
                          while (i < r6 && b4[i] >= '0' && b4[i] <= '9')
                              n2 = n2 * 10 + (b4[i++] - '0');
                          v[k++] = n2;
                      }
                      if (v[0] > 0 && v[1] > 0 && k >= 4) {
                          g_marker_every = (double)v[0] / 1000.0;
                          g_marker_for = (double)v[1] / 1000.0;
                          g_marker_long_every = (double)v[2] / 1000.0;
                          g_marker_long_for = (double)v[3] / 1000.0;
                          log_num("bench: marker bursts, ms on ", (unsigned)v[0]);
                          log_num("  ms off ", (unsigned)v[1]);
                          log_num("  long blackout every ms ", (unsigned)v[2]);
                          log_num("  lasting ms ", (unsigned)v[3]);
                      } else if (v[0] > 0 && v[1] > 0) {
                          g_marker_every = (double)v[0];
                          g_marker_for = (double)v[1];
                          log_num("bench: dropping Reflex/PCL markers every N s, N = ",
                                  (unsigned)v[0]);
                          log_num("  for this many seconds ", (unsigned)v[1]);
                      }
                  }
                  CloseHandle(mh);
              } }
            g_blockalt = flag_file(L"mfg-blockalt.txt");
            g_no_waitable = flag_file(L"mfg-nowaitable.txt");
            { wchar_t sp[MAX_PATH]; beside_dll(sp, L"mfg-slowframe.txt");
              HANDLE sh = CreateFileW(sp, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
              if (sh != INVALID_HANDLE_VALUE) {
                char b3[48]; DWORD r5 = 0;
                if (ReadFile(sh, b3, sizeof(b3)-1, &r5, nullptr) && r5 > 0) {
                    b3[r5] = 0;
                    int v[3] = { 0, 0, 0 }; int k = 0; DWORD q = 0;
                    while (q < r5 && k < 3) {
                            while (q < r5 && (b3[q] < '0' || b3[q] > '9')) ++q;
                        if (q >= r5) break;
                        int n2 = 0;
                            while (q < r5 && b3[q] >= '0' && b3[q] <= '9')
                                n2 = n2 * 10 + (b3[q++] - '0');
                        v[k++] = n2;
                    }
                    if (v[0] > 0 && v[0] <= 100000) {
                        g_slow_frame_us = v[0];
                        log_num("bench: frame slowed by us ", (unsigned)v[0]);
                    }
                    if (k >= 3 && v[1] > 0 && v[1] <= 100000 && v[2] > 0) {
                        g_slow_frame_us2 = v[1];
                        g_slow_step_ms = v[2];
                        log_num("bench: base steps to us ", (unsigned)v[1]);
                        log_num("  every ms ", (unsigned)v[2]);
                    }
                }
                CloseHandle(sh);
              } }
            { wchar_t jp[MAX_PATH]; beside_dll(jp, L"mfg-jitter.txt");
              HANDLE jh = CreateFileW(jp, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
              if (jh != INVALID_HANDLE_VALUE) {
                  char b4[16]; DWORD r6 = 0;
                  if (ReadFile(jh, b4, sizeof(b4)-1, &r6, nullptr) && r6 > 0) {
                      b4[r6] = 0; int v = 0;
                      for (DWORD k = 0; k < r6 && b4[k] >= '0' && b4[k] <= '9'; ++k)
                          v = v * 10 + (b4[k] - '0');
                      if (v > 0 && v <= 90) { g_jitter_pct = v;
                          log_num("bench: frame jitter pct ", (unsigned)v); }
                  }
                  CloseHandle(jh);
              } }
            { wchar_t cp[MAX_PATH]; beside_dll(cp, L"mfg-clamplatency.txt");
              HANDLE ch = CreateFileW(cp, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
              if (ch != INVALID_HANDLE_VALUE) {
                  char c2[16]; DWORD r4 = 0;
                  if (ReadFile(ch, c2, sizeof(c2) - 1, &r4, nullptr) && r4 > 0) {
                      c2[r4] = 0;
                      if (c2[0] >= '1' && c2[0] <= '9') g_clamp_latency = c2[0] - '0';
                  }
                  CloseHandle(ch);
              } }
            // Queue parallelism mode, from mfg-queue.txt. patch_queue_mode has
            // been in the file with no way to reach it -- g_queue_mode was left
            // at -1 -- and it is the one knob that touches the pacing subsystem
            // the throughput law lives in, so it gets a flag before anything is
            // disassembled.
            { wchar_t qp[MAX_PATH]; beside_dll(qp, L"mfg-queue.txt");
              HANDLE qh = CreateFileW(qp, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
              if (qh != INVALID_HANDLE_VALUE) {
                  char q2[16]; DWORD r3 = 0;
                  if (ReadFile(qh, q2, sizeof(q2) - 1, &r3, nullptr) && r3 > 0) {
                      q2[r3] = 0;
                      if (q2[0] >= '0' && q2[0] <= '3') g_queue_mode = q2[0] - '0';
                  }
                  CloseHandle(qh);
              } }
            { wchar_t bp[MAX_PATH]; beside_dll(bp, L"mfg-blocks.txt");
              HANDLE bh = CreateFileW(bp, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
              if (bh != INVALID_HANDLE_VALUE) {
                  char b2[32]; DWORD r2 = 0;
                  if (ReadFile(bh, b2, sizeof(b2) - 1, &r2, nullptr) && r2 > 0) {
                      b2[r2] = 0; int v = 0;
                      for (DWORD k = 0; k < r2 && b2[k] >= '0' && b2[k] <= '9'; ++k)
                          v = v * 10 + (b2[k] - '0');
                      if (v >= 2 && v <= 64) { g_blocks = v;
                          log_num("slowalt: blocks per cycle from file ", (unsigned)v); }
                  }
                  CloseHandle(bh);
              } }
            if (g_frac_enabled)
                log_line("fractional multiplier ON (experimental: can stall the game)");
            if (g_peralt)
                log_line("slowalt: per-frame error diffusion (mfg-peralt.txt)");
            if (g_debug) log_line("debug: F9 recorder armed (hooks Present)");
            g_ov_enabled = !flag_file(L"mfg-nopanel.txt");
            settings_load();
            log_line(g_ov_enabled ? "panel on (` opens it)" : "panel off");
            const wchar_t *pn = L"mfg-presetb.txt";
            for (int i = 0; pn[i] != 0; ++i) pb[j + i] = pn[i];
            pb[j + 15] = 0;
            g_preset_b = GetFileAttributesW(pb) != INVALID_FILE_ATTRIBUTES;
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
            const wchar_t *cn = L"mfg-nocubins.txt";
            for (int i = 0; cn[i] != 0; ++i) pb[j + i] = cn[i];
            pb[j + 16] = 0;
            g_cubins = GetFileAttributesW(pb) == INVALID_FILE_ATTRIBUTES;
            // g_meter_off was declared and read but never assigned, so
            // mfg-nometer.txt did nothing at all.
            const wchar_t *mn = L"mfg-nometer.txt";
            for (int i = 0; mn[i] != 0; ++i) pb[j + i] = mn[i];
            pb[j + 15] = 0;
            g_meter_off = GetFileAttributesW(pb) != INVALID_FILE_ATTRIBUTES;
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
    {
        wchar_t p[MAX_PATH];
        int j = 0;
        while (g_log[j] != 0 && j < MAX_PATH - 1) { p[j] = g_log[j]; ++j; }
        while (j > 0 && p[j - 1] != 0x5C) --j;
        const wchar_t *fn = L"mfg-indicator.txt";
        for (int i = 0; fn[i] != 0; ++i) p[j + i] = fn[i];
        p[j + 17] = 0;
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
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

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto reg = reinterpret_cast<PFN_LdrRegister>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    if (reg == nullptr) {
        log_line("LdrRegisterDllNotification is unavailable; nothing will be patched");
        return TRUE;
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
    return TRUE;
}
