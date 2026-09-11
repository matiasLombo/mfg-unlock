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
// Estado del set, compartido entre el hook de carga, el observador y el panel.
// -1 sin dato, 0 verde, 1 amarillo, 2 rojo.
static int g_veredicto_previo = -1;
static bool g_veredicto_leido = false;
// -1 sin preguntar todavia, 0 dijo que no, 1 dijo que si.
//
// M4. Sustituir el Streamline de un juego es una decision del usuario, no
// nuestra: cambia que binarios corre su juego. Con ROJO solo NO alcanza; hace
// falta un si explicito, y hasta que lo haya el dll no toca nada y lo dice.
static int g_consentimiento = -1;
volatile LONG g_pide_permiso = 0;   // lo lee el panel (overlay.h)
static void read_previous_verdict(void);
static void save_consent(int si);
static bool version_supported(const wchar_t *path, unsigned *major, unsigned *minor_v);
static bool g_ya_sustituimos = false;                         // M3
static void emit_verdict_if_due(void);   // M1, definida mas abajo
static bool state_path(wchar_t *out, int max);            // M1, idem
static void dyn_apply(double base_fps);                          // definida mas abajo
bool g_dynamic_known = false;
bool g_ov_enabled = true;
static void log_line(const char *text);
static void log_num(const char *label, unsigned long long v);
// Capa 0: cual copia EJECUTA, atada por el puntero que devuelve el interposer.
static void executing_copy(const void *fn, const char *name);

// La fase de la sesion. Monotona: nunca vuelve para atras.
//
//   ARMADO      todavia no se pudieron evaluar los invariantes
//   VERIFICADO  se evaluaron y dieron bien (paso inmediato a ACTIVO)
//   ACTIVO      se permite parchear y reescribir opciones
//   PASIVO      la topologia no cumple: NO se toca nada y el log dice por que
//
// Existe para invertir el modo de falla. Hasta ahora un juego con una topologia
// que no habiamos visto producia un crash y una regla reactiva nueva; con esto
// produce un bloque de diagnostico y nada mas. Es la pieza que hace que "romperse
// por juego" deje de ser una opcion del codigo.
enum class Phase { ARMED, VERIFIED, ACTIVE, PASSIVE };
static volatile LONG g_phase = (LONG)Phase::ARMED;
static inline bool phase_active(void) { return g_phase == (LONG)Phase::ACTIVE; }
static inline bool phase_passive(void) { return g_phase == (LONG)Phase::PASSIVE; }
static void evaluate_invariants(void);
// Alimenta g_present_count desde el runtime cuando no hay hook de Present.
static void runtime_presents(void);
extern int g_executing_copy;
// Presentaciones vistas en el swapchain. Declarada aca porque el latch de
// apply_override_now la necesita y vive antes que su definicion.
extern volatile LONG g_present_count;
extern bool g_executing_resolved;

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
static bool g_saw_any_copy = false;
// Sitios del parche de la cuenta de la ULTIMA copia parcheada. La cuenta se
// parchea adentro de patch_subframe_count, y el observador de M1 la necesita
// por copia, no acumulada.
static int g_wic_sites_last = -1;
static bool g_quiet = false;          // mfg-quiet.txt: no per-window logging
static bool g_nullalt = false;        // mfg-nullalt.txt: alternate between equals
static bool g_slowalt = false;        // mfg-slowalt.txt
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
static volatile LONG g_cycle_ceiling = 0;   // lo+1 del ciclo en curso
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

// Lo que hay al lado de la dll, leido una vez en DllMain (src/config.h).
static config::Settings g_cfg;

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
static LONG  g_burst_hilo = 0;
static void  log_flush(void);

static void log_burst_begin(void) {
    g_burst_hilo = (LONG)GetCurrentThreadId();
    g_burst_on = true;
}
static void log_burst_end(void) {
    log_flush();
    g_burst_on = false;
    g_burst_hilo = 0;
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
    if (g_burst_on && (LONG)GetCurrentThreadId() == g_burst_hilo) {
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
// La base propia va ENCENDIDA. mfg-sinbase.txt la apaga.
//
// Estaba al reves -- detras de mfg-snippet.txt -- y eso es justo lo que rompio
// GTA V: sin el archivo, el juego caia a sus propios binarios y a la semantica
// vieja, en silencio. Un usuario que solo tiene el dll no puede depender de
// acordarse de poner un txt en cada juego.
//
// Si la carpeta no tiene los archivos, no se sustituye nada igual: el respaldo
// es la ausencia de la base, no la ausencia de un flag.
static bool g_snippet_on = true;
// El interposer entra en la base como todos. mfg-sininterposer.txt lo saca.
//
// Estuvo excluido porque el banco fallaba 5 de 5 con el sustituido, y lo exclui
// sin leer POR QUE fallaba -- que es para lo que existe el banco. El motivo real
// es que se mapean DOS interposers, no que el modulo este mal: el del banco y el
// nuestro son byte a byte el mismo archivo.
// El interposer del JUEGO se respeta. mfg-coninterposer.txt lo vuelve a pisar.
//
// Estaba al reves, y el motivo por el que se pisaba murio medido. Se hacia por
// Halo: su 2.7.30 no tiene un dlss_g parcheable -- el sitio de la cuenta existe
// desde 2.11 -- asi que "habia que subir todo el set junto". Falso: los plugins
// suben solos. Halo corriendo SU interposer 2.7.30 con nuestros nueve plugins
// 2.12 entrega 4.02 pidiendo 4X y 5.98 pidiendo 6X, sobre 81 ventanas.
//
// Y forzarlo costaba: un juego compilado contra el host SDK 2.7.30 pierde las
// constantes de sl.common bajo un interposer 2.12, y el plugin apaga la
// generacion CUADRO A CUADRO. Medido, 108 warnings por segundo contra 31, y
// 2.007x contra 4.02x.
//
// Ademas es la unica pieza que un juego puede traer como import estatico
// -- Cyberpunk lo tiene en el puesto #5 y nosotros en el #26 -- asi que en la
// mitad de los casos no habia nada que sustituir de todos modos, y de ahi
// salieron los dos peores crashes del 2026-09-10.
//
// Solo lo usamos por tres exports -- slGetFeatureFunction, slInit y
// slGetNewFrameToken -- que cualquier version exporta. No le parcheamos un byte.
static bool g_interposer_out = true;
// mfg-sllog.txt esta presente: ademas del log, se sube el nivel en Preferences.
static bool g_sllog_on = false;
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

// Experimento de la fase D: apuntar pathsToPlugins a nuestra carpeta.
// Ver el bloque de hk_slInit, mas abajo.
static bool g_pathsplugins = false;

static unsigned hk_slInit(void *pref, unsigned long long sdk) {
    if (pref != nullptr) {
        unsigned char *p = (unsigned char *)pref;
        log_num("slInit: structVersion ", (unsigned)*(unsigned long long *)(p + 24));
        const unsigned long long f = *(unsigned long long *)(p + kPrefFlags);
        log_num("  banderas en +88 ", (unsigned)f);
        // EXPERIMENTO (mfg-pathsplugins.txt): apagar OTA y nombrar NUESTRA
        // carpeta, para que el plugin manager del interposer DEL JUEGO enumere
        // ahi y no mire la cache de NGX.
        //
        // La disposicion no es una suposicion: sale del header oficial del SDK
        // 2.12, include/sl_core_types.h, contando desde BaseStructure
        // (next +0, GUID +8, structVersion +24 = 32):
        //
        //   +32 bool showConsole          +40 const wchar_t** pathsToPlugins
        //   +36 LogLevel logLevel         +48 uint32_t numPathsToPlugins
        //   +56 pathToLogsAndData         +88 PreferenceFlags flags
        //
        // Los dos ceros que se veian en el volcado eran justamente
        // pathsToPlugins nulo y numPathsToPlugins en 0. Y el +88 de las
        // banderas, verificado hace tiempo por otro camino, cae en el mismo
        // lugar: el conteo cierra entero.
        //
        // Detras de un flag porque cambia la TOPOLOGIA de carga, que es la
        // unica parte del diseno que puede romper un juego que hoy anda. Y
        // porque falta medir algo que el header no dice: el plugin manager,
        // ante el mismo plugin en dos rutas, se queda con el mas nuevo ("A
        // duplicate was found, but a newer plugin version was available"). Si
        // el juego trae 2.13 y nosotros 2.12, puede preferir el suyo aunque
        // apuntemos aca.
        if (g_pathsplugins) {
            static wchar_t buf[MAX_PATH];
            static const wchar_t *list[1];
            static bool armado = false;
            if (!armado) {
                const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH - 40);
                if (n > 0 && n < MAX_PATH - 40) {
                    int k = (int)n;
                    const wchar_t *tail = L"\\mfg-unlock\\sdk\\2.12";
                    for (int i = 0; tail[i] != 0; ++i) buf[k++] = tail[i];
                    buf[k] = 0;
                    list[0] = buf;
                    armado = true;
                }
            }
            if (armado) {
                *(const wchar_t ***)(p + 40) = list;
                *(unsigned *)(p + 48) = 1;
                *(unsigned long long *)(p + kPrefFlags) =
                    f & ~((1ull << 3) | (1ull << 6));   // eAllowOTA, eLoadDownloadedPlugins
                log_line("  pathsToPlugins apuntado a nuestra carpeta (mfg-pathsplugins.txt)");
                log_num("    banderas ahora ",
                        (unsigned)*(unsigned long long *)(p + kPrefFlags));
            }
        }
        // logLevel, a verbose, para que el runtime diga por que corta.
        //
        // beginCommandList (sl.common 0x77AF0) devuelve false por una de dos
        // razones y las dos escriben al log de Streamline:
        //
        //     'Command list not closed'
        //     "Couldn't reset the allocator %S for %.2f seconds."
        //
        // Ninguna aparece en el sl.log de Halo, pero las dos estan gateadas por
        // una bandera global:
        //
        //     0x77b08  cmp byte ptr [rip+0x59e2a], 0
        //     0x77b0f  je  0x77b98        ; sin log, igual devuelve false
        //
        // SL_LOG_LEVEL=2 ya se pone por entorno y no alcanzo: lo que la
        // aplicacion pasa en Preferences gana. Aca se pisa el campo.
        //
        // Offset 36: BaseStructure son 32 (next 8 + GUID 16 + structVersion 8),
        // showConsole ocupa 4 con relleno, y logLevel viene despues. El mismo
        // conteo que da 88 para las banderas, que ya esta verificado.
        //
        // Solo si el valor que hay tiene forma de enum chico -- si no, el
        // offset no es el que creemos y escribir seria corromper.
        {
            LONG *nivel = (LONG *)(p + 36);
            const LONG before = *nivel;
            log_num("  logLevel en +36 ", (unsigned)before);
            if (before >= 0 && before <= 3 && g_sllog_on) {
                DWORD viejo = 0;
                if (VirtualProtect(nivel, 4, PAGE_READWRITE, &viejo)) {
                    *nivel = 2;                 // eVerbose
                    VirtualProtect(nivel, 4, viejo, &viejo);
                    log_line("  logLevel forzado a verbose (mfg-sllog.txt)");
                }
            }
        }
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
// Lo que el JUEGO pide, que no es lo mismo que lo que pedimos nosotros.
//
// g_juego_quiere: su ultimo modo, 0 = eOff, 1 = eOn, -1 = todavia no lo vimos.
// g_juego_pidio_on: si alguna vez lo vimos pedir eOn en esta corrida.
static volatile LONG g_game_wants = -1;
static volatile LONG g_game_asked_on = 0;
// La cuenta que el plugin tiene REALMENTE aplicada, no la que queremos.
//
// bound y reserva tienen que moverse juntos: bound = API + 1 anda, bound mayor
// escribe fuera de la reserva y crashea con 0xC0000005, bound menor detiene la
// presentacion. No hay margen.
//
// El panel cambiaba g_force_generated y dejaba el envio a la API pendiente
// mientras fractional_tick escribia el byte en el frame siguiente: subir de
// modo ponia el bound en 5 con reserva para 4. Halo cambio a 6X a los 59 s y
// murio a los 68 -- la escritura fuera de rango corrompe y el crash llega
// despues. No aparecio antes porque el banco y Cyberpunk fijan el multiplicador
// al arrancar y no lo cambian en caliente.
// 6X queda fuera hasta que el bound deje de necesitar +1.
//
// El byte del bound lleva cuenta+1, porque nuestro guard de entrada consume una
// iteracion (sin eso cada ratio salia un frame corto y el pacing se derrumbaba).
// El array de sub-frames tiene 6 ranuras inline -- 5 generados y el real -- y
// los campos vivos del contexto empiezan justo donde termina.
//
// A 5X el bound es 5: un desborde de una posicion cae DENTRO del array y no
// hace nada. A 6X el bound es 6 y cae sobre los campos vivos. Por eso es 6X en
// particular el que se rompe, y no los de abajo: no tiene holgura.
//
// Medido jugando Halo: 2X y 4X andan y sostienen la sesion; 6X mata el juego,
// tres veces, sin evento WER ni dump -- corrompe y muere despues.
//
// Se limita en las DOS mitades a la vez, la cuenta de la API y el byte, porque
// frenar una sola ya salio mal antes: "freno: la cuenta se limita a 1" con el
// byte siguiendo al 3, y crash a los 36 s.
//
// mfg-x6.txt lo vuelve a habilitar para investigarlo. El arreglo de fondo es
// encontrar por que el bound necesita el +1 y sacarlo.
static bool g_permitir_x6 = false;
// 5 es el maximo del plugin (6X). El tope en 4 era una mitigacion del crash
// que ademas limitaba 5X, y la causa resulto ser otra.
// Las direcciones que se parchearon para subir el tope, para releerlas despues.
//
// Hace falta porque el parche engancho (2 sitios) y el campo siguio en 5. Dos
// causas posibles y no se adivinan: o el parche no quedo vivo en la copia que
// realmente corre, o NGX contesta 5 y el clamp min(NGX,6) lo deja en 5.
// Releyendo el byte en memoria se distingue.
// El tope 6 va ENCENDIDO. mfg-sinseis.txt lo baja a 5.
//
// Estaba detras de mfg-seis.txt "hasta que este medido", y ya esta medido: con
// nuestro snippet entrega 6.00 con cero fallos NGX y sin artefactos. Dejarlo
// como habilitador es el error que la regla de polaridad prohibe -- el usuario
// recibe un dll y nada mas, y un juego sin el archivo cae al 5 en silencio.
//
// Los dos parches que lo implementan son seguros por contenido, no por fe:
// patch_tope_seis busca dos inmediatos exactos y no hace nada si no estan, y
// patch_snippet_max solo corre si la semantica dice que el snippet es el nuestro
// -- forzar 6 sobre la build de julio, que topa en 3 por arquitectura, es
// exactamente lo que hizo crashear a Halo.
static bool g_six = true;             // mfg-sinseis.txt lo apaga

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
static volatile LONG g_snippet_cargado = 0;

static bool count_is_multiplier(void) {
    return g_snippet_cargado != 0;
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
static void detectar_semantica(unsigned char *base) {
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
    const LONG before = g_snippet_cargado;
    g_snippet_cargado = julio ? 0 : 1;
    if (before != g_snippet_cargado)
        log_line(julio ? "semantica: la cuenta son los GENERADOS (snippet con tope 3)"
                       : "semantica: la cuenta es el MULTIPLICADOR");
}

static volatile unsigned char *g_six_sites[4] = { nullptr, nullptr, nullptr, nullptr };
static int g_six_n = 0;
static volatile LONG g_max_declared = 0;   // 0 = todavia no se leyo
// mfg-topefijo.txt: vuelve al tope de 5 de antes, para poder MEDIR la linea
// base sin recompilar. Diagnostico local, nunca el arreglo: sin el archivo el
// dll se comporta como se envia.
static bool g_tope_fijo = false;

static LONG count_cap(void) {
    (void)g_permitir_x6;
    if (g_tope_fijo) return 5;
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
    const LONG techo = (g_six || count_is_multiplier()) ? 6 : 5;
    return (d >= 1 && d <= techo) ? d : 5;
}
// Base del sl.dlss_g que estamos parcheando, para poder leerle campos.
static const unsigned char *g_dlssg_base = nullptr;
static volatile LONG g_api_applied = -1;
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
static bool leer_ok(const void *src, void *dst, unsigned n) {
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
static void bajar_4168(LONG v) {
    if (g_dlssg_base == nullptr || v < 1) return;
    unsigned char *pp = nullptr;
    if (!leer_ok(g_dlssg_base + 0x8f1e8, &pp, sizeof(pp)) || pp == nullptr) return;
    LONG *field = (LONG *)(pp + 0x4168);
    LONG actual = 0;
    if (!leer_ok(field, &actual, 4)) return;
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
static void decir_modulo(const char *tag, const void *fn) {
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
static void sonda_vtable(void) {
    if (g_dlssg_base == nullptr) return;
    static bool said = false;
    if (said) return;
    unsigned char *ctx = nullptr;
    if (!leer_ok(g_dlssg_base + 0x8f1e8, &ctx, sizeof(ctx)) || ctx == nullptr) return;
    said = true;
    log_line("vtable: resolviendo el corte del bucle de generacion");
    LONG sel1 = -1; unsigned char sel2 = 0xFF;
    leer_ok(ctx + 0x45a8, &sel1, 4);
    leer_ok(ctx + 0x45e1, &sel2, 1);
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
        if (!leer_ok(ctx + cands[c].off, &obj, sizeof(obj)) || obj == nullptr) {
            log_line(cands[c].name_w); log_line("    nulo"); continue;
        }
        if (cands[c].doble) {
            unsigned char *o2 = nullptr;
            if (!leer_ok(obj, &o2, sizeof(o2)) || o2 == nullptr) {
                log_line(cands[c].name_w); log_line("    segundo deref nulo"); continue;
            }
            obj = o2;
        }
        unsigned char *vt = nullptr, *fn = nullptr;
        if (!leer_ok(obj, &vt, sizeof(vt)) || vt == nullptr) {
            log_line(cands[c].name_w); log_line("    vtable ilegible"); continue;
        }
        if (!leer_ok(vt + 0x40, &fn, sizeof(fn)) || fn == nullptr) {
            log_line(cands[c].name_w); log_line("    slot +0x40 ilegible"); continue;
        }
        decir_modulo(cands[c].name_w, fn);
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
    if (!leer_ok(g_dlssg_base + 0x8f1e8, &ctx, sizeof(ctx)) || ctx == nullptr) return;
    LONG v = 0;
    if (!leer_ok(ctx + 0x45e4, &v, 4)) return;
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

static LONG sonda_4168(void) {
    if (g_dlssg_base == nullptr) return -1;
    const unsigned char *pp = nullptr;
    if (!leer_ok(g_dlssg_base + 0x8f1e8, &pp, sizeof(pp)) || pp == nullptr)
        return -1;
    LONG v = -1;
    if (!leer_ok(pp + 0x4168, &v, 4)) return -1;
    return v;
}
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

// The game asks for a frame token once per frame, from its own render thread
// -- which is the thread that called slDLSSGSetOptions and the only one it is
// safe to call it from again. With DLSS-G on, Present is Streamline's thread,
// so the replay cannot happen there; the log said so outright.
typedef unsigned (*PFN_slGetNewFrameToken)(void *&, const unsigned *);
static PFN_slGetNewFrameToken g_orig_frametoken = nullptr;
static void *g_frametoken_addr = nullptr;      // found at startup, hooked later
static void apply_override_now(void);

static void query_state(void);


#include "patches.h"

// N generated frames on the next batch. Zero is not expressible here -- the
// loop is a do-while, so its body has already run once by the time the bound
// is tested, and one generated frame is the floor.
// What is actually in force, which is not the same as g_force_generated once
// the byte is being written directly: a mode picked by hand leaves that
// variable behind, and dividing the measured rate by a stale multiplier makes
// the base look far lower than it is -- which asks for more generation, which
// makes it look lower still.
static volatile LONG g_count_live = 1;



#include "measurement.h"

// How many generated frames it would take to reach the target from the rate
// the game is actually rendering at, with the ceiling the plugin gave us.
#include "writer.h"

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

#include "frametoken.h"

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
        const int piso = g_sub2 ? 110 : kMinCustom;
        g_dyn_target = s.target < piso ? piso : s.target;
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


#include "exceptions.h"

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
            // Toda la configuracion se lee de una, en src/config.h, y de ahi
            // se reparte a los globales que cada subsistema ya usaba. Lo que
            // sigue conserva el orden, los avisos y los efectos (arm_slinit
            // temprano, la cuenta recalculada) que DllMain tenia a mano.
            config::read_flags(g_cfg, [](const wchar_t *f) { return flag_file(f); });
            config::read_numerics(g_cfg, [](const wchar_t *f, char *b, unsigned cap) -> unsigned {
                wchar_t p[MAX_PATH];
                beside_dll(p, f);
                HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE) return 0u;
                DWORD got = 0;
                const BOOL ok = ReadFile(h, b, cap, &got, nullptr);
                CloseHandle(h);
                if (!ok) return 0u;
                b[got] = 0;
                return (unsigned)got;
            });
            // Y el archivo unico, ultimo para que sus claves ganen. Ver
            // docs/configuracion.md.
            {
                wchar_t cp[MAX_PATH];
                beside_dll(cp, L"mfg-config.txt");
                HANDLE ch = CreateFileW(cp, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (ch != INVALID_HANDLE_VALUE) {
                    static char cbuf[4096];
                    DWORD got = 0;
                    const BOOL ok = ReadFile(ch, cbuf, sizeof(cbuf) - 1, &got, nullptr);
                    CloseHandle(ch);
                    if (ok && got > 0) {
                        cbuf[got] = 0;
                        const int vistas = config::parse_config(cbuf, (unsigned)got, g_cfg);
                        log_num("config: claves leidas de mfg-config.txt ", (unsigned)vistas);
                    }
                }
            }
            const config::Settings &a = g_cfg;
            g_debug = a.debug;
            g_watch_settings = a.watch;
            g_novsync = a.novsync;
            g_pin_latency = a.pinlatency;
            g_pace_follow = a.pacefollow;
            g_frac_enabled = a.frac;
            g_sub2 = a.sub2;
            g_twocopies = a.twocopies;
            g_ceilfirst = a.ceilfirst;
            g_ota = a.ota;
            // Con la bandera puesta hay que llegar antes que la llamada del
            // juego, y el armado del hilo del panel llega tarde en los juegos
            // que importan el interposer estaticamente.
            if (g_ota) arm_slinit_temprano();
            g_slowalt = a.slowalt;
            g_quiet = a.quiet;
            g_nullalt = a.nullalt;
            if (a.blockms > 0) {
                g_block_ms = a.blockms;
                log_num("slowalt: block length from file, ms ", (unsigned)a.blockms);
            }
            if (!a.sat) {
                g_sat_on = false;
                log_line("sat: deteccion de techo DESACTIVADA (mfg-sinsat.txt)");
            }
            g_pathsplugins = a.pathsplugins;
            if (g_pathsplugins)
                log_line("slInit: se apuntara pathsToPlugins a nuestra carpeta (experimento)");
            g_peralt = a.peralt;
            if (!a.seis) {
                g_six = false;
                log_line("tope: 6X DESACTIVADO a mano (mfg-sinseis.txt)");
            }
            if (g_six) {
                log_line("tope: 6X activo");
                // El archivo de settings se lee ANTES que este flag, asi que un
                // modo fijo restaurado de disco ya aplico el mapeo viejo (v-1) y
                // la cuenta quedaba una abajo: con mode 6 se pedia 5 y se
                // entregaba 5X. Se recalcula aca, que es cuando el flag existe.
                if (g_force_sel >= 2 && g_force_sel <= kSelMaxFixed) {
                    g_force_generated = g_force_sel;
                    g_opt_pending = 1;
                    log_num("  cuenta recalculada para el modo fijo ",
                            (unsigned)g_force_generated);
                }
            }
            if (a.coninterposer) {
                g_interposer_out = false;
                log_line("base: el interposer TAMBIEN se sustituye (mfg-coninterposer.txt)");
            }
            if (a.sinbase) {
                g_snippet_on = false;
                log_line("base: DESACTIVADA a mano (mfg-sinbase.txt)");
            }
            // mfg-mfcmax.txt: sube la constante del snippet. Experimental.
            if (a.mfcmax) {
                g_mfcmax = 5;
                log_line("MultiFrameCountMax: se intentara subir a 5 (mfg-mfcmax.txt)");
            }
            g_tope_fijo = a.topefijo;
            if (g_tope_fijo) log_line("tope fijo en 5 (mfg-topefijo.txt): es la LINEA BASE, crashea");
            g_permitir_x6 = a.x6;
            if (g_permitir_x6) log_line("6X habilitado a mano (mfg-x6.txt): crashea en Halo");
            g_dyn_diag = a.dyndiag;
            if (g_dyn_diag) log_line("dynamic: diagnostico por cambio de ratio ENCENDIDO (mfg-dyndiag.txt)");
            if (!a.latch) { g_latch_schedule = false; log_line("fractional: reparto NO latcheado (mfg-nolatch.txt)"); }
            if (!a.deuda) { g_use_debt = false; log_line("dynamic: integrador de deuda APAGADO (mfg-sin-deuda.txt)"); }
            g_optsv3 = a.optsv3;
            if (a.marker[0] > 0 && a.marker[1] > 0 && a.marker_n >= 4) {
                g_marker_every = (double)a.marker[0] / 1000.0;
                g_marker_for = (double)a.marker[1] / 1000.0;
                g_marker_long_every = (double)a.marker[2] / 1000.0;
                g_marker_long_for = (double)a.marker[3] / 1000.0;
                log_num("bench: marker bursts, ms on ", (unsigned)a.marker[0]);
                log_num("  ms off ", (unsigned)a.marker[1]);
                log_num("  long blackout every ms ", (unsigned)a.marker[2]);
                log_num("  lasting ms ", (unsigned)a.marker[3]);
            } else if (a.marker[0] > 0 && a.marker[1] > 0) {
                g_marker_every = (double)a.marker[0];
                g_marker_for = (double)a.marker[1];
                log_num("bench: dropping Reflex/PCL markers every N s, N = ",
                        (unsigned)a.marker[0]);
                log_num("  for this many seconds ", (unsigned)a.marker[1]);
            }
            g_blockalt = a.blockalt;
            g_no_waitable = a.nowaitable;
            if (a.slowframe[0] > 0 && a.slowframe[0] <= 100000) {
                g_slow_frame_us = a.slowframe[0];
                log_num("bench: frame slowed by us ", (unsigned)a.slowframe[0]);
            }
            if (a.slowframe_n >= 3 && a.slowframe[1] > 0 && a.slowframe[1] <= 100000 &&
                a.slowframe[2] > 0) {
                g_slow_frame_us2 = a.slowframe[1];
                g_slow_step_ms = a.slowframe[2];
                log_num("bench: base steps to us ", (unsigned)a.slowframe[1]);
                log_num("  every ms ", (unsigned)a.slowframe[2]);
            }
            if (a.jitter > 0) {
                g_jitter_pct = a.jitter;
                log_num("bench: frame jitter pct ", (unsigned)a.jitter);
            }
            if (a.clamplatency > 0) g_clamp_latency = a.clamplatency;
            // Queue parallelism mode, from mfg-queue.txt: the one knob that
            // touches the pacing subsystem the throughput law lives in.
            if (a.queue >= 0) g_queue_mode = a.queue;
            if (a.blocks > 0) {
                g_blocks = a.blocks;
                log_num("slowalt: blocks per cycle from file ", (unsigned)a.blocks);
            }
            if (g_frac_enabled)
                log_line("fractional multiplier ON (experimental: can stall the game)");
            if (g_peralt)
                log_line("slowalt: per-frame error diffusion (mfg-peralt.txt)");
            if (g_debug) log_line("debug: F9 recorder armed (hooks Present)");
            g_ov_enabled = a.panel;
            settings_load();
            log_line(g_ov_enabled ? "panel on (` opens it)" : "panel off");
            g_preset_b = a.presetb;
            // Los cubins van encendidos por defecto: son lo que hace fluido el
            // 4x ([[cubins-are-the-fluidity-fix]]), y quien instala esto tiene
            // la dll y nada mas. mfg-nocubins.txt los apaga.
            g_cubins = a.cubins;
            g_meter_off = a.meter_off;
        }
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        g_qpc_freq = f.QuadPart ? f.QuadPart : 1;
        g_samples = static_cast<Sample *>(VirtualAlloc(nullptr, sizeof(Sample) * kMaxSamples,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        HANDLE th = CreateThread(nullptr, 0, &recorder, nullptr, 0, nullptr);
        if (th != nullptr) CloseHandle(th);
    }
    AddVectoredExceptionHandler(1, exception_witness);
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

    // Antes de que nadie cargue plugins: si el sl.dlss_g del juego no tiene el
    // sitio de la cuenta, se carga uno de la cache que si lo tenga.
    arm_plugin_redirect();

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
