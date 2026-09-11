// state.h -- las globales que usa mas de un modulo, en un solo lugar, cada
// una con quien la escribe y quien la lee.
//
// Las que usa un solo modulo viven en ese modulo. Lo que hay aca es el
// acoplamiento real entre modulos, visible: si una variable aparece con tres
// escritores, eso es lo que hay que mirar cuando algo cambia solo.
//
// El comentario "escribe / lee" de cada una lo genera tools/globals_report.py
// contando el codigo (sin cadenas ni comentarios); "escribe" es asignacion,
// ++/--, op= o &g_x (Interlocked*). config_apply.h escribe casi todas las de
// configuracion una vez, al cargar. Se incluye al principio de proxy.cpp,
// despues de los headers puros y antes de cualquier modulo.
//
// Movidas de donde estaban (2026-09-11) sin cambiar la declaracion.
#pragma once

// La fase de la sesion: ARMADO -> VERIFICADO|PASIVO -> ACTIVO. La decide
// evaluate_invariants (loader.h) una vez, tarde, con el grafo entero cargado.
enum class Phase { ARMED, VERIFIED, ACTIVE, PASSIVE };

// Tipos que las declaraciones de abajo necesitan.
typedef unsigned (*PFN_slSetMarker)(unsigned, void *);
typedef unsigned (*PFN_slDLSSGGetState)(const void *, void *, const void *);
typedef unsigned (*PFN_slDLSSGSetOptions)(const void *, const void *);
typedef unsigned (*PFN_slGetFeatureFunction)(unsigned, const char *, void *&);
typedef unsigned (*PFN_slInit)(void *, unsigned long long);
typedef unsigned (*PFN_slGetNewFrameToken)(void *&, const unsigned *);
typedef unsigned (*PFN_slReflexGetState)(void *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_DXGIPresent)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SMFL)(IUnknown *, UINT);
typedef int(__stdcall *PFN_Present)(void *, const void *);
static const int kMaxSites = 4;
struct Sample { long long qpc; unsigned img; int meter; unsigned char src; };

// escribe: present.h, recorder.h; lee: -
static double g_all_ms = 0.0;

// escribe: present.h, recorder.h; lee: -
static int g_all_n = 0, g_all_bad = 0;       // every present, for the summary

// escribe: writer.h; lee: exceptions.h, measurement.h, present.h
static volatile LONG g_api_applied = -1;

// escribe: measurement.h; lee: writer.h
static double g_base_fps = 0.0;       // that, divided by the multiplier in force

// escribe: config_apply.h; lee: writer.h
static int  g_block_ms = 0;           // mfg-blockms.txt, 0 = default

// escribe: config_apply.h; lee: writer.h
static bool g_blockalt = false;       // mfg-blockalt.txt: blocks above 2.0x too

// escribe: config_apply.h; lee: writer.h
static int  g_blocks = 0;             // mfg-blocks.txt, 0 = 32

// escribe: writer.h; lee: measurement.h
// What the last capture was taken from. A game may call slDLSSGSetOptions
// once per frame, and re-capturing on every call meant two VirtualQuery
// syscalls per frame on the game's own render thread -- to copy bytes that had
// not changed since the frame before. The capture itself is still page-bounded
// every time it runs; what is skipped is running it when there is nothing new.
static LONG g_cap_mode = -1, g_cap_cnt = -1;

// escribe: config_apply.h; lee: writer.h
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

// escribe: -; lee: config_apply.h, patches.h, proxy.cpp
// Lo que hay al lado de la dll, leido una vez en DllMain (src/config.h).
static config::Settings g_cfg;

// escribe: config_apply.h, measurement.h; lee: present.h
static int g_clamp_latency = 0;        // mfg-clamplatency.txt, 0 = off

// escribe: loader.h, recorder.h; lee: -
// -1 sin preguntar todavia, 0 dijo que no, 1 dijo que si.
//
// M4. Sustituir el Streamline de un juego es una decision del usuario, no
// nuestra: cambia que binarios corre su juego. Con ROJO solo NO alcanza; hace
// falta un si explicito, y hasta que lo haya el dll no toca nada y lo dice.
static int g_consentimiento = -1;

// escribe: writer.h; lee: exceptions.h, measurement.h, present.h
// N generated frames on the next batch. Zero is not expressible here -- the
// loop is a do-while, so its body has already run once by the time the bound
// is tested, and one generated frame is the floor.
// What is actually in force, which is not the same as g_force_generated once
// the byte is being written directly: a mode picked by hand leaves that
// variable behind, and dividing the measured rate by a stale multiplier makes
// the base look far lower than it is -- which asks for more generation, which
// makes it look lower still.
static volatile LONG g_count_live = 1;

// escribe: reflex.h; lee: measurement.h, writer.h
static double g_ctrl_fps = 0.0;       // frames/tiempo, insesgada, para el controlador

// escribe: reflex.h; lee: writer.h
// Cuantas muestras tiene el anillo del estimador ahora mismo.
//
// Existe porque el detector de escalon vacia el anillo y deja la base valiendo
// 1/dt de UN solo frame. Al armarse DYNAMIC los primeros frames son lentos --
// la reconfiguracion del swapchain esta medida en [[fractional-swapchain-churn]]
// -- asi que un frame de ~62 ms dejaba la base en 16 fps con la real en 156.
// Con eso el controlador calculaba 165/16 = 10.3 y topaba en 6.00, que es
// justo el ratio que revienta.
//
// Del log de Halo, sin interpretacion:
//
//     [37891ms] dynamic: refresh is 165            <- DYNAMIC recien armado
//     [38000ms] target needs more than 6x at this base, fps 165
//     [38000ms]   base is 16
//     [38172ms] measured: rendered fps 156         <- 172 ms despues
//     [38172ms]   base por Reflex 0                <- y Reflex no alimentaba
//
// Con la base real el ratio pedido es 165/33 = 5.0 exacto: alcanzable y sin
// tocar la ranura que el plugin no llena.
static int g_ctrl_n = 0;

// escribe: config_apply.h; lee: loader.h
static bool g_cubins = false;

// escribe: measurement.h, present.h; lee: -
static int    g_cv_n = 0;

// escribe: measurement.h, present.h; lee: -
static double g_cv_sq = 0.0;     // suma de cuadrados

// escribe: measurement.h, present.h; lee: -
// Cuando cambio la cuenta, en reloj real, y la curva de recuperacion.
//
// El corte viejo era "menos de 8 presentaciones", que a 161 fps son ~50 ms --
// la MITAD del enfriamiento de 100 ms que se quiere detectar. Con la ventana
// mas corta que el efecto, cerca y lejos dan igual (0.94 y 0.92 medidos) y eso
// NO prueba que cambiar la cuenta salga gratis.
//
// Aca se mide en milisegundos y por tramos, asi la DURACION del efecto sale del
// dato en vez de asumirse. Si el enfriamiento es real y dura 100 ms, los tramos
// de abajo de 100 tienen que estar peor que los de arriba, y la recuperacion
// tiene que verse.
// Cadencia SIN ESCALA, porque la de arriba tiene un defecto.
//
// "Fuera de cadencia" usa una banda ABSOLUTA de 4-8 ms, o sea que mide cuanto
// se aleja el frame rate de ~165 fps mezclado con la irregularidad real. 4X
// fijo sale 23.8 % en parte porque su intervalo medio (7.5 ms) roza el borde de
// la banda, no porque sea irregular; 5X fijo sale 1.2 % porque 5.2 ms cae comodo
// en el medio. Comparar configuraciones con distinto fps por esa banda no es
// valido.
//
// Esta compara cada intervalo contra la media de SU PROPIA ventana: cuenta los
// que se van mas de un cuarto. Es adimensional, asi que 133 y 192 present/s se
// pueden comparar. Necesita dos pasadas por ventana, y como no se guardan los
// intervalos se hace con la desviacion acumulada: suma y suma de cuadrados dan
// el desvio relativo (coeficiente de variacion), que sirve igual y cuesta dos
// sumas por presentacion.
static double g_cv_sum = 0.0;    // suma de intervalos, ms

// escribe: config_apply.h; lee: recorder.h
static bool g_debug = false;      // mfg-debug.txt: developer diagnostics

// escribe: present.h; lee: measurement.h
static int    g_disp_bucket[6] = { 0, 0, 0, 0, 0, 0 };

// escribe: measurement.h, present.h; lee: -
static int    g_disp_changes = 0;     // cambios de imagen en la ventana

// escribe: measurement.h, present.h; lee: -
static int    g_disp_hitch = 0;       // cambios separados por mas de 33 ms

// escribe: measurement.h, present.h; lee: -
static double g_disp_ms_max = 0.0;

// escribe: measurement.h, present.h; lee: -
static double g_disp_ms_sum = 0.0;

// escribe: measurement.h, present.h; lee: -
static int    g_disp_presents = 0;    // presentaciones en la misma ventana

// escribe: measurement.h, present.h; lee: -
static int    g_disp_skip = 0;        // cambios que saltaron mas de un refresh

// escribe: loader.h, proxy.cpp; lee: -
// Base del sl.dlss_g que estamos parcheando, para poder leerle campos.
static const unsigned char *g_dlssg_base = nullptr;

// escribe: config_apply.h, writer.h; lee: -
static bool g_dyn_diag = false;        // mfg-dyndiag.txt: diagnostico por cambio de ratio

// escribe: proxy.cpp, recorder.h; lee: overlay.h, writer.h
// DYNAMIC: el objetivo esta en fps presentados, no en ratio. Cero significa
// el refresh del monitor. El controlador escribe g_dyn_target -- el ratio --
// y de ahi para abajo todo es el planificador fraccionario que ya andaba.
volatile LONG g_dyn_fps = 0;

// escribe: recorder.h, writer.h; lee: -
// 0 nothing said yet, 1 written, 2 declined. Reset when the target changes so
// a new value gets its own line, and guarded so this never logs per frame:
// log_line opens and closes the file on every call.
static int g_dyn_said = 0;

// escribe: proxy.cpp, recorder.h, writer.h; lee: overlay.h
volatile LONG g_dyn_target = 0;

// escribe: loader.h, proxy.cpp; lee: overlay.h, writer.h
bool g_dynamic_known = false;

// escribe: loader.h; lee: proxy.cpp
// Cual de las copias registradas contiene este puntero.
//
// Se dice UNA vez por corrida, con el cuadro completo: la que ejecuta y las que
// no, cada una con los sitios que recibio. Si la que ejecuta tiene cero sitios y
// otra los tiene, ahi esta el defecto de fondo que este proyecto viene pagando
// juego por juego -- parchear todas las copias y confiar, en vez de verificar el
// efecto en la que corre.
int g_executing_copy = -1;   // indice en g_copias, -1 = todavia no se sabe

// escribe: loader.h; lee: proxy.cpp
// Distinto de lo anterior: si el interposer llego a resolvernos una funcion de
// DLSS-G. Sin esto, "no se sabe cual ejecuta" y "el gancho nunca disparo" serian
// el mismo estado, y el segundo NO es una topologia rota -- es falta de dato.
// Apagar el mod por falta de dato seria la misma clase de regla no verificada
// que este trabajo viene a sacar.
bool g_executing_resolved = false;

// escribe: measurement.h, present.h; lee: -
static int g_far_n = 0, g_far_bad = 0;       // and the rest

// escribe: config_apply.h, frametoken.h, proxy.cpp, recorder.h, writer.h; lee: exceptions.h, measurement.h, overlay.h, present.h
volatile LONG g_force_generated = 0;

// escribe: proxy.cpp, recorder.h; lee: config_apply.h, exceptions.h, frametoken.h, measurement.h, overlay.h, patches.h, present.h, writer.h
// The panel's own state, defined here and shared with overlay.h. It is a
// window of ours now, not something drawn into the game's frame -- see the
// note at the top of that file for what the three rendering attempts before
// it cost.
volatile LONG g_force_sel = 0;

// escribe: config_apply.h; lee: patches.h
static bool g_frac_enabled = false;   // mfg-frac.txt

// escribe: -; lee: proxy.cpp, recorder.h
static wchar_t g_frames[MAX_PATH];

// escribe: -; lee: proxy.cpp, recorder.h
static wchar_t g_frames_base[MAX_PATH];   // unnumbered name, per-run suffix added at F9

// escribe: frametoken.h, measurement.h; lee: -
static volatile LONG g_frames_gated = 0;

// escribe: proxy.cpp; lee: overlay.h, recorder.h, writer.h
// What the plugin says it will accept. Zero means "not asked yet"; the panel
// offers nothing above this once it is known.
volatile LONG g_frames_max = 0;      // shared with overlay.h

// escribe: patches.h; lee: frametoken.h
static void *g_frametoken_addr = nullptr;      // found at startup, hooked later

// escribe: present.h; lee: measurement.h
// Para clasificar los apagones sin pedirle al usuario que juegue distinto:
// si la ventana no tiene el foco, o el juego dejo de estar en primer plano,
// el apagon es del menu/pausa/alt-tab y no un defecto nuestro.
static HWND g_game_hwnd = nullptr;

// escribe: frametoken.h, writer.h; lee: -
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

// escribe: patches.h, writer.h; lee: -
static volatile unsigned char *g_gen_flag = nullptr;    // 1 generating, 0 not

// escribe: measurement.h, present.h; lee: -
static int g_hitch_far = 0;                  // and the rest

// escribe: measurement.h, present.h; lee: -
static int g_hitch_near = 0;                 // hitches within 4 presents of one

// escribe: measurement.h, overlay.h; lee: -
// La base, para poder mostrar "base/presentadas" en vez de un numero solo.
//
// Un "342 FPS" no dice nada por si mismo: no se sabe si son 57 x6 o 171 x2, que
// se ven y se sienten distinto. Los dos numeros juntos son el multiplicador a
// simple vista, sin tener que abrir el panel.
static volatile LONG g_hud_base_x10 = 0;

// escribe: measurement.h, overlay.h; lee: -
// Los alimenta el hook de present y la sonda de Reflex.
static volatile LONG g_hud_fps_x10 = 0;

// escribe: measurement.h, overlay.h; lee: -
static volatile LONG g_hud_lat_us = 0;

// escribe: overlay.h, proxy.cpp, recorder.h; lee: -
static bool g_hud_on = false;            // lo enciende el casillero del panel

// escribe: loader.h, patches.h; lee: writer.h
static int g_imm2_n = 0;

// escribe: -; lee: loader.h, patches.h, writer.h
static volatile unsigned char *g_imm2_sites[kMaxSites] = { nullptr, nullptr, nullptr, nullptr };

// escribe: loader.h, patches.h; lee: writer.h
static int g_imm3_n = 0;

// escribe: -; lee: loader.h, patches.h, writer.h
static volatile unsigned char *g_imm3_sites[kMaxSites] = { nullptr, nullptr, nullptr, nullptr };

// escribe: loader.h, patches.h; lee: writer.h
static int g_imm_n = 0;

// escribe: -; lee: loader.h, patches.h, writer.h
static volatile unsigned char *g_imm_sites[kMaxSites] = { nullptr, nullptr, nullptr, nullptr };

// escribe: measurement.h; lee: present.h, writer.h
// Encendida o no. No hay consulta directa que sirva: slDLSSGGetState avisa que
// hay que sincronizarla con el hilo de present. Se deduce de que lo presentado
// supere a la base de Reflex, que es la medida honesta que ya tenemos.
static volatile LONG g_interp_on = 0;

// escribe: config_apply.h; lee: loader.h
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

// escribe: config_apply.h; lee: frametoken.h
static int  g_jitter_pct = 0;         // mfg-jitter.txt, porcentaje

// escribe: writer.h; lee: present.h
static volatile LONG g_last_change_pres = 0; // present index at the last count change

// escribe: measurement.h; lee: writer.h
static double g_last_dt = 0.0;        // and the most recent one, for the spread

// escribe: writer.h; lee: overlay.h, patches.h, present.h
volatile LONG g_last_seen_generated = 0;

// escribe: patches.h, writer.h; lee: -
static volatile unsigned char *g_lat_allow = nullptr;   // 1 reconfigure, 0 leave alone

// escribe: present.h, recorder.h; lee: -
static int g_lat_n = 0, g_lat_max = 0;

// escribe: present.h, recorder.h; lee: -
static double g_lat_sum = 0.0;               // queued presents, summed

// escribe: config_apply.h, writer.h; lee: -
static bool g_latch_schedule = true;    // mfg-nolatch.txt lo apaga, ver fractional_tick

// escribe: config_apply.h, proxy.cpp; lee: reflex.h
static double g_marker_every = 0.0;

// escribe: config_apply.h; lee: reflex.h
static double g_marker_for = 0.0;

// escribe: config_apply.h; lee: reflex.h
static double g_marker_long_every = 0.0;

// escribe: config_apply.h; lee: reflex.h
static double g_marker_long_for = 0.0;

// escribe: proxy.cpp; lee: writer.h
static volatile LONG g_max_declared = 0;   // 0 = todavia no se leyo

// escribe: config_apply.h; lee: loader.h
static bool g_meter_off = false;

// escribe: config_apply.h, patches.h; lee: loader.h
// Both gates are `cmp <r32>, 0x1B0` against the NGX architecture id.  Rewriting
// the immediate to 0 makes the comparison read "arch >= 0", true everywhere, so
// whichever way the compiler phrased the predicate -- jl, setae, cmovl, all of
// which appear across snippet builds -- the Blackwell branch is the one taken.
// El techo de MFG, en su origen: la constante que el snippet le contesta a NGX.
//
// El plugin no inventa el maximo, se lo pregunta al snippet:
//
//   sl.dlss_g 0x57c97  lea   rdx, 'DLSSG.MultiFrameCountMax'
//             0x57c9e  call  [GetParameterInt]
//             0x57cca  mov   edx, 5
//             0x57cd1  cmovb edx, ecx        ; [ctx+0x45e4] = min(NGX, 5)
//
// Y el snippet lo decide por arquitectura:
//
//   nvngx_dlssg 0x26572  mov   ebx, 1
//               0x26577  mov   r8d, 3        ; <- ESTE
//               0x2657d  cmp   edi, 0x1b0    ; 0x1b0 = Blackwell
//               0x26583  cmovl r8d, ebx      ; arch < 0x1b0 -> 1
//               0x26587  lea   rdx, 'DLSSG.MultiFrameCountMax'
//               0x26595  call  [SetParameterInt]
//
// O sea `max = (arch >= 0x1B0) ? 3 : 1`. En Ada (0x190) de fabrica seria 1 --
// el 2X nativo -- y llega a 3 porque patch_gates ya reescribe ese `cmp` a cero
// y Ada toma el camino de Blackwell.
//
// **El 3 no es una capacidad medida de la GPU: es una constante por
// arquitectura**, y ya la estamos moviendo de 1 a 3. Subirla a 5 es el unico
// punto de la cadena que puede dar mas: forzar el campo del PLUGIN ya se probo
// y NGX rechaza igual (hipotesis 9 en docs/objetivo-halo-6x.md), porque el que
// valida despues es el snippet.
//
// No se sabe si el snippet aguanta 5. Puede que 3 refleje recursos que si
// existen. Por eso va detras de mfg-mfcmax.txt hasta que este medido: si sale
// mal, el dll que se envia no cambia.
//
// El patron evita el inmediato del `cmp`, que patch_gates ya puede haber puesto
// en cero: se ancla en `mov r8d, 3` + `81 FF` (cmp edi, imm32, sea cual sea) +
// `cmovl r8d, ebx`.
static int g_mfcmax = 0;          // 0 = no tocar; si no, el valor a escribir

// escribe: measurement.h, present.h; lee: -
static int g_near_n = 0, g_near_bad = 0;     // presents within 8 of a change

// escribe: config_apply.h, present.h; lee: -
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

// escribe: config_apply.h; lee: present.h
static bool g_novsync = false;        // mfg-novsync.txt: diagnostic

// escribe: recorder.h; lee: present.h
static volatile LONG g_nsamples = 0;

// escribe: config_apply.h; lee: writer.h
static bool g_nullalt = false;        // mfg-nullalt.txt: alternate between equals

// escribe: -; lee: proxy.cpp, writer.h
// Enough of the last call to make it again ourselves. The header says
// slDLSSGSetOptions is not thread safe, so the thread the game used is
// recorded with it and the replay only happens on that same thread -- from
// the present hook, which the game enters every frame.
static unsigned char g_opt_copy[256];

// escribe: proxy.cpp, writer.h; lee: -
static volatile LONG g_opt_have = 0;

// escribe: config_apply.h, frametoken.h, recorder.h, writer.h; lee: -
static volatile LONG g_opt_pending = 0;

// escribe: proxy.cpp, writer.h; lee: -
static volatile LONG g_opt_thread = 0;

// escribe: config_apply.h; lee: writer.h
// mfg-optsv3.txt: SOLO para el banco. GTA V llena DLSSGOptions con
// structVersion 3 y el sample con 5, y eso se leyo de los logs de los dos.
// En v3 no existe numFramesToGenerate, asi que la cuenta que force_into
// escribe en p+36 la ignora el plugin: el sl.log del juego reporta
// numFramesToGenerate=1 en las seis transiciones mientras el planificador
// escribia 2. Con esto el sample manda v3 y el banco recorre el mismo camino.
// Apagada por defecto; no cambia nada de lo que la dll hace en un juego.
static bool g_optsv3 = false;

// escribe: patches.h, proxy.cpp; lee: recorder.h
static PFN_slGetFeatureFunction g_orig_getfeaturefn = nullptr;

// escribe: proxy.cpp; lee: reflex.h
// mfg-markergap.txt: SOLO para el banco. Contiene dos numeros, "cada" y
// "cuanto", en segundos: cada N segundos deja de reenviar los marcadores de
// Reflex/PCL durante M segundos. Es lo que GTA V hace solo -- su sl.log
// muestra el id de frame de Reflex congelado en 13307 mientras el actual
// llegaba a 23110, y DLSS-G se niega con
// eDLSSGStatusFailReflexNotDetectedAtRuntime durante 72 s seguidos.
// El sample nunca corta ese flujo, y por eso el banco no podia apagarse.
// Apagada por defecto; en un juego la dll no hace nada de esto.
static PFN_slSetMarker g_orig_pclmarker = nullptr;

// escribe: proxy.cpp; lee: reflex.h
static PFN_slSetMarker g_orig_reflexmarker = nullptr;

// escribe: proxy.cpp, reflex.h; lee: -
// Latencia: sl.reflex la calcula solo. El sample llama a slReflexGetState
// todos los frames (StreamlineSample.cpp:901, sin condicion), asi que envolver
// esa llamada da el struct ya armado por el, con su GUID y su version -- no
// hay que adivinar nada. La cuenta que importa la escribe NVIDIA en su propio
// sample: totalGameToRenderLatency = gpuRenderEndTime - inputSampleTime.
// Paso 1, y por ahora lo unico: volcar los bytes para leer el layout. Nada de
// calcular latencias sobre offsets supuestos.
static PFN_slReflexGetState g_orig_reflexstate = nullptr;

// escribe: proxy.cpp; lee: writer.h
static PFN_slDLSSGSetOptions g_orig_setoptions = nullptr;

// escribe: patches.h, proxy.cpp; lee: slinit.h
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
static PFN_slInit g_orig_slinit = nullptr;

// escribe: config_apply.h; lee: slinit.h
static bool g_ota = false;              // mfg-ota.txt

// escribe: -; lee: loader.h, proxy.cpp
// The id of the newest build in NVIDIA's OTA cache, or empty if there is none.
// NGX loads that copy and leaves the one in the game folder unused, so a patch
// failing against the game's own copy is not a failure worth reporting -- and
// reporting it anyway is exactly how a log cries wolf: DOOM maps both, and the
// verdict shouted "CUBINS NOT APPLIED" about the image that never executes
// while the one that does was patched correctly.
static wchar_t g_ota_newest[64] = {0};

// escribe: overlay.h; lee: recorder.h
// Typing into the value box, polled like every other key here so a game that
// never delivers WM_CHAR cannot stop it.
static bool g_ov_editing = false;

// escribe: config_apply.h; lee: overlay.h, recorder.h
bool g_ov_enabled = true;

// escribe: overlay.h; lee: recorder.h
static int   g_ov_hot = -1;

// escribe: overlay.h; lee: recorder.h
static float g_ov_mx = 0.0f, g_ov_my = 0.0f;

// escribe: overlay.h, recorder.h; lee: -
static volatile bool g_ov_visible = false;

// escribe: recorder.h, writer.h; lee: -
static bool g_override_said = false;

// escribe: patches.h, writer.h; lee: -
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

// escribe: config_apply.h, patches.h; lee: -
static bool g_pace_follow = false;    // mfg-pacefollow.txt

// escribe: config_apply.h, slinit.h; lee: -
// Experimento de la fase D: apuntar pathsToPlugins a nuestra carpeta.
// Ver el bloque de hk_slInit, mas abajo.
static bool g_pathsplugins = false;

// escribe: config_apply.h; lee: writer.h
static bool g_peralt = false;         // mfg-peralt.txt: diffuse per frame

// escribe: config_apply.h, proxy.cpp; lee: -
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

// escribe: loader.h, proxy.cpp; lee: -
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
static volatile LONG g_phase = (LONG)Phase::ARMED;

// escribe: recorder.h; lee: overlay.h
volatile LONG g_pide_permiso = 0;   // lo lee el panel (overlay.h)

// escribe: frametoken.h, patches.h; lee: -
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

// escribe: config_apply.h, patches.h; lee: -
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

// escribe: present.h; lee: measurement.h
static int    g_pres_bucket[6] = { 0, 0, 0, 0, 0, 0 };

// escribe: measurement.h, present.h; lee: -
static int    g_pres_hitch = 0;             // intervals over 33 ms

// escribe: measurement.h, present.h; lee: -
static double g_pres_ms_max = 0.0;

// escribe: measurement.h, present.h; lee: -
static double g_pres_ms_sum = 0.0;

// escribe: measurement.h, present.h; lee: -
static int    g_pres_n = 0;

// escribe: measurement.h, present.h; lee: -
static double g_present_block_us = 0.0;   // blocked inside Present, per window

// escribe: measurement.h, present.h; lee: proxy.cpp, writer.h
volatile LONG g_present_count = 0;   // presents seen at the swap chain

// escribe: config_apply.h; lee: loader.h
static bool g_preset_b = false;

// escribe: measurement.h; lee: writer.h
static bool g_probe_done = false;

// escribe: measurement.h, writer.h; lee: -
static LONG g_probe_i = 0;

// escribe: writer.h; lee: measurement.h
// Sonda de una sola vez: cuantos frames pasan entre escribir un ratio nuevo y
// que las presentaciones lo reflejen. El objetivo pedia medir esto y no
// suponerlo, y es el unico candidato que queda para el sobrepaso -- todo el
// error residual esta del lado alto, justo despues de que la base sube.
static LONG g_probe_left = 0;

// escribe: measurement.h, writer.h; lee: -
static LONG g_probe_pc0 = 0;

// escribe: proxy.cpp, reflex.h; lee: frametoken.h, measurement.h, present.h, recorder.h, writer.h
static long long g_qpc_freq = 1;   // set in DllMain

// escribe: config_apply.h; lee: loader.h
static int g_queue_mode = -1;

// escribe: config_apply.h; lee: measurement.h
static bool g_quiet = false;          // mfg-quiet.txt: no per-window logging

// escribe: recorder.h; lee: present.h
// QPC at the start of a recording, so display times can be stored as a small
// offset rather than a 64-bit absolute.
static long long g_rec_qpc0 = 0;

// escribe: recorder.h; lee: present.h
static volatile LONG g_recording = 0;

// escribe: measurement.h, present.h; lee: -
// PresentRefreshCount, que es el unico dato de pantalla que resulto confiable.
//
// SyncQPCTime NO sirve para MsBetweenDisplayChange: viene repetido. Se midio y
// el control lo mata -- en ventanas SIN generacion, donde cada presentacion es
// un frame real, daba 6.42 presentaciones por "cambio de imagen" y 49
// imagenes/s contra 312 presentaciones/s. Un artefacto que aparece igual de
// fuerte sin un solo frame generado no esta midiendo frames generados.
//
// PresentRefreshCount cuenta REFRESHES del panel. Si avanza a ~165/s el panel
// va normal y lo grueso era SyncQPCTime; presentaciones por refresh dice si
// cada presentacion se gano su refresh o si se pisan entre ellas.
static unsigned  g_ref_first = 0;

// escribe: measurement.h, present.h; lee: -
static unsigned  g_ref_last = 0;

// escribe: measurement.h, present.h; lee: -
static long long g_ref_qpc0 = 0;

// escribe: reflex.h; lee: measurement.h
static double g_rfx_base = 0.0;        // base en fps, del contador de frames de Reflex

// escribe: measurement.h, reflex.h; lee: -
static double g_rfx_drv = 0.0;     // sim start -> fin de driver

// escribe: measurement.h, reflex.h; lee: -
static double g_rfx_ft = 0.0;          // gpuFrameTimeUs, para validar contra la base

// escribe: measurement.h, reflex.h; lee: -
// Offsets confirmados con el volcado y con dos chequeos internos: el
// gpuActiveRenderTimeUs que escribe NVIDIA coincide con gpuRenderEnd-Start
// (3802 vs 3883 us) y su gpuFrameTimeUs de 16854 us es exactamente la base de
// 59 fps que medimos aparte. El cuerpo del informe 63, el mas reciente, cae en
// 72 + 152*63 = 9648.
static double g_rfx_gpu = 0.0;     // sim start -> fin de render en GPU

// escribe: measurement.h, reflex.h; lee: -
static unsigned g_rfx_max = 0;

// escribe: measurement.h, reflex.h; lee: -
// Minimo y maximo ademas del promedio: si la cadencia fraccionaria alterna
// entre comportarse como el entero de abajo y el de arriba, la latencia seria
// dos poblaciones y no un valor intermedio. El promedio de la ventana eso lo
// tapa, que es exactamente como este proyecto ya se equivoco una vez.
static unsigned g_rfx_min = 0xFFFFFFFFu;

// escribe: measurement.h, reflex.h; lee: -
static LONG g_rfx_n = 0;

// escribe: measurement.h, present.h; lee: writer.h
static volatile LONG g_rt_present_count = 0;

// escribe: proxy.cpp, recorder.h; lee: present.h
// Two layers matter and they are not the same. The game calls
// sl.interposer!vkQueuePresentKHR once per *rendered* frame; Streamline then
// issues the generated frames further down, through the Vulkan loader. Hooking
// only the top layer measures the input rate, not the output -- which is why an
// earlier capture showed two images at 71 fps and no metering at all. Hook both
// and tag which one produced each row.
static Sample *g_samples = nullptr;

// escribe: config_apply.h, writer.h; lee: -
// Saturacion: el ratio mas barato que sostiene las presentadas que ya se logran.
//
// Arriba de cierto punto, subir el multiplicador NO compra frames. La base se
// derrumba en la misma proporcion y el producto queda quieto. Medido en GTA V,
// agrupando 355 ventanas de juego real por el ratio que entregaron:
//
//   ratio  ventanas   base   presentado   latencia
//    3.5      50      45.5      161.1      47.4 ms
//    4.0      61      41.9      165.1      50.3 ms
//    4.5      76      35.6      162.6      61.8 ms
//    5.0     147      32.6      161.2      66.7 ms
//
// De 3.5x a 5.0x las presentadas no se mueven -- 161 contra 161 -- y la latencia
// sube 19 ms. El controlador paso 147 de esas 355 ventanas en 5.0x, o sea en el
// peor punto de la tabla, cobrando 19 ms por cero frames.
//
// Y no era un defecto de la aritmetica: hacia lo que se le pidio. Ve 161 contra
// un objetivo de 165, concluye que le falta, y empuja. Lo que no sabia es que
// por esa via ya no hay nada que ganar.
//
// Esto es lo que un entero no puede hacer y un fraccional si: el punto optimo es
// el ratio mas bajo que llega al techo, y ese punto se mueve con la base -- 3.94
// con base 41.9, 3.63 con base 45.5. Un entero obliga a redondear para arriba y
// pagar la latencia entera.
//
// mfg-sinsat.txt lo apaga, para poder comparar.
static bool   g_sat_on = true;

// escribe: config_apply.h, proxy.cpp; lee: loader.h, patches.h, writer.h
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

// escribe: patches.h, proxy.cpp; lee: -
static int g_six_n = 0;

// escribe: -; lee: patches.h, proxy.cpp
static volatile unsigned char *g_six_sites[4] = { nullptr, nullptr, nullptr, nullptr };

// escribe: proxy.cpp; lee: slinit.h
// mfg-sllog.txt esta presente: ademas del log, se sube el nivel en Preferences.
static bool g_sllog_on = false;

// escribe: config_apply.h; lee: frametoken.h
static int  g_slow_frame_us = 0;      // mfg-slowframe.txt, en microsegundos

// escribe: config_apply.h; lee: frametoken.h
// Con tres numeros en mfg-slowframe.txt la carga alterna entre el primero y
// el segundo cada N milisegundos, y la base del banco se mueve de verdad
// durante la corrida. Sin esto no hay forma de probar un controlador: una
// escena de carga constante no distingue a uno que ajusta de uno que no
// hace nada. Escalon y no rampa a proposito -- el que aguanta un escalon
// aguanta una rampa, y en los datos se ve donde empieza.
static int  g_slow_frame_us2 = 0;

// escribe: config_apply.h; lee: frametoken.h
static int  g_slow_step_ms = 0;

// escribe: config_apply.h; lee: writer.h
static bool g_slowalt = false;        // mfg-slowalt.txt

// escribe: measurement.h, present.h; lee: -
static volatile LONG g_smfl_calls = 0;

// escribe: config_apply.h; lee: loader.h
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

// escribe: frametoken.h, measurement.h; lee: -
// Forma del salto de indice entre llamadas consecutivas al token, para saber si
// el gate se rompe por intercalado de hilos.
static volatile LONG g_step_same = 0, g_step_plus_one = 0, g_step_forward = 0, g_step_back = 0;

// escribe: config_apply.h, proxy.cpp; lee: -
// mfg-sub2.txt: SOLO para medir. Baja el piso del target por debajo de 200
// en el camino de mfg-settings.txt, para poder medir 1.25x/1.50x/1.75x sin
// bajar el piso del panel, que es justo lo que hay que decidir con esos
// numeros. Sin el archivo, el comportamiento es identico al de antes.
static bool g_sub2 = false;           // mfg-sub2.txt

// escribe: frametoken.h, measurement.h; lee: -
static double g_token_block_us = 0.0;     // blocked inside slGetNewFrameToken

// escribe: frametoken.h, measurement.h; lee: -
static volatile LONG g_token_calls = 0;

// escribe: measurement.h, writer.h; lee: -
// Smoothed over a window rather than taken frame to frame. A single frame
// time is noisy enough that the multiplier would change on a passing hitch,
// and every change makes Streamline restart interpolation -- the native
// controller did that 42 times in one run, twice within seven milliseconds,
// and that churn is what a player feels as stutter.
static double g_token_dt = 0.0;       // the averaged frame time, in seconds

// escribe: measurement.h, writer.h; lee: -
static double g_token_fps = 0.0;      // and the rate that follows from it

// escribe: config_apply.h, proxy.cpp; lee: -
// mfg-topefijo.txt: vuelve al tope de 5 de antes, para poder MEDIR la linea
// base sin recompilar. Diagnostico local, nunca el arreglo: sin el archivo el
// dll se comporta como se envia.
static bool g_tope_fijo = false;

// escribe: config_apply.h; lee: loader.h
// mfg-twocopies.txt: SOLO para el banco. Contiene la ruta de una segunda copia
// de sl.dlss_g; se carga a proposito para reproducir lo que hace Cyberpunk, que
// mapea la del juego Y la del cache OTA. g_wic y g_count_imm son punteros unicos
// y la segunda copia parcheada los pisa, asi que las escrituras por frame se van
// a un modulo que no genera. El sample no lo hace solo: Streamline avisa
// "eLoadDownloadedPlugins flag not passed to preferences", y esa bandera la pasa
// la aplicacion en slInit, no un archivo de configuracion.
static bool g_twocopies = false;      // mfg-twocopies.txt

// escribe: loader.h, recorder.h; lee: -
static volatile LONG g_twocopies_pending = 0;

// escribe: config_apply.h, writer.h; lee: -
// El integrador de deuda del controlador. Redundante con el sesgo por
// tramo y el que rompia (dos corridas de Cyberpunk con el mismo binario:
// buena 153-160, mala 274 con deuda y sesgo clavados en el riel). Se
// deja detras de mfg-sin-deuda.txt para el A/B; la regla esta en
// controlador.h.
static bool g_use_debt = true;

// escribe: loader.h; lee: recorder.h
// Estado del set, compartido entre el hook de carga, el observador y el panel.
// -1 sin dato, 0 verde, 1 amarillo, 2 rojo.
static int g_veredicto_previo = -1;

// escribe: -; lee: proxy.cpp, writer.h
static unsigned char g_vp_copy[64];

// escribe: config_apply.h, proxy.cpp; lee: -
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

// escribe: patches.h; lee: writer.h
static bool g_wic_mode = false;                         // mfg-wic.txt

// escribe: loader.h, patches.h; lee: writer.h
static int g_wic_n = 0;

// escribe: patches.h; lee: writer.h
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

// escribe: patches.h; lee: writer.h
// Poner y sacar el parche del byte segun el modo.
//
// El parche reemplaza `mov eax,[rdx+4]` (8B 42 04) por `push imm8 / pop rax`,
// y ese inmediato es el limite del bucle de sub-frames. Mientras esta puesto
// hay DOS canales fijando la misma cantidad -- ese byte y la cuenta que el
// plugin recibe por slDLSSGSetOptions, con la que dimensiona sus ranuras -- y
// cuando se desfasan las dos direcciones rompen: el byte por encima recorre una
// ranura sin inicializar y crashea ([nulo+0x40] en +0x3ED6F, del dump de
// UE4SS); por debajo detiene la presentacion.
//
// Medido en el banco con el parche DESACTIVADO de entrada:
//
//     6X entero      6.00  genera
//     CUSTOM 2.50x   1.00  no genera
//
// O sea: los enteros no lo necesitan y los fraccionarios no pueden sin el. Con
// el parche fuera en modos enteros queda un solo canal y el desfasaje no puede
// existir.
//
// Se escribe sobre codigo del plugin con el juego corriendo. Los tres bytes se
// escriben de atras hacia adelante para que el opcode quede ultimo: asi ningun
// hilo puede leer una instruccion a medio formar.
static volatile LONG g_wic_set = 1;   // el parche arranca aplicado

// escribe: -; lee: loader.h, patches.h, writer.h
static volatile unsigned char *g_wic_sites[kMaxSites] = { nullptr, nullptr, nullptr, nullptr };

// escribe: loader.h, patches.h; lee: -
// Sitios del parche de la cuenta de la ULTIMA copia parcheada. La cuenta se
// parchea adentro de patch_subframe_count, y el observador de M1 la necesita
// por copia, no acumulada.
static int g_wic_sites_last = -1;
