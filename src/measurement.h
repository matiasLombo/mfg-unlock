// measurement.h -- la ventana de medicion: que se renderizo, que se presento,
// a que ritmo y con que latencia, cada 45 frames renderizados.
//
// ENTRA: por frame renderizado, note_rendered_frame() desde el hook del frame
//   token; el contador del runtime en g_rt_present_count (lo alimenta present.h
//   con GetLastPresentCount, o el hook de Present cuando lo hay); la base y la
//   latencia de Reflex en g_rfx_* (reflex.h); la cuenta en vigor
//   (g_force_generated, g_dyn_target, la cadencia).
// SALE: g_hud_fps_x10 / g_hud_base_x10 / g_hud_lat_us para el HUD
//   (overlay.h); g_base_fps y g_token_fps para el controlador y el reparto;
//   el volcado "measured: ..." al log al cerrar cada ventana; dyn_control()
//   y dyn_apply() se llaman desde aca con la ventana cerrada.
// DEPENDE DE: log_line/log_num, QueryPerformanceCounter, el controlador
//   (controller.h) y los globales compartidos que todavia viven en proxy.cpp.
//
// Movido de proxy.cpp tal cual (2026-09-11): las lineas son las mismas, el
// orden de declaracion tambien, porque el #include quedo donde estaba el
// bloque. Separar "contar" de "volcar" y renombrar es el paso siguiente.
#pragma once

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
volatile LONG g_present_count = 0;   // presents seen at the swap chain
static long long g_pres_qpc = 0;            // when the last present went out
static volatile LONG g_last_change_pres = 0; // present index at the last count change
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
static double g_cv_sq = 0.0;     // suma de cuadrados
static int    g_cv_n = 0;
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
static int    g_freeze_dichos = 0;          // tope de lineas del diagnostico
static int    g_pres_bucket[6] = { 0, 0, 0, 0, 0, 0 };
// El reloj de la PANTALLA, que es otro que el de Present.
//
// Todo lo de arriba mide intervalos entre LLAMADAS a Present. NVIDIA dice
// explicitamente que eso no sirve para juzgar fluidez con DLSS-G: el plugin
// retrasa la imagen por hardware DESPUES de Present(), y la guia de
// ProgrammingGuideDLSS_G manda medir MsBetweenDisplayChange y no
// MsBetweenPresents. Explica de paso por que el pacer daba vuelta toda la
// distribucion de intervalos y no cambiaba nada visible ([[present-timing-is-
// not-fluidity]]): movia el reloj que no se ve.
//
// SyncQPCTime es el instante en que el panel escaneo la ultima imagen. Cuando
// cambia, hubo un cambio de imagen de verdad; si no cambia entre dos presents,
// esa presentacion no llego a la pantalla como imagen propia. Esto tambien
// responde la pregunta que quedo abierta en note_display -- si de cada lote de
// 4 llega una sola o si el driver devuelve la estructura vieja -- porque
// presentaciones por cambio de imagen lo dice directo.
static long long g_disp_qpc = 0;      // SyncQPCTime del ultimo cambio visto
static unsigned  g_disp_refresh = 0;  // PresentRefreshCount en ese cambio
static int    g_disp_changes = 0;     // cambios de imagen en la ventana
static int    g_disp_presents = 0;    // presentaciones en la misma ventana
static double g_disp_ms_sum = 0.0;
static double g_disp_ms_max = 0.0;
static int    g_disp_hitch = 0;       // cambios separados por mas de 33 ms
static int    g_disp_bucket[6] = { 0, 0, 0, 0, 0, 0 };
static int    g_disp_skip = 0;        // cambios que saltaron mas de un refresh
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
static unsigned  g_ref_last = 0;
static long long g_ref_qpc0 = 0;
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
                // Todo el volcado de la ventana en una sola apertura.
                log_burst_begin();
                // Silenced by mfg-quiet.txt. log_line opens, seeks, writes and
                // closes the file for every line, on the render thread, and the
                // alternating arm emits more lines than the integer one -- so
                // part of the 10% throughput gap could be this rather than the
                // cadence. Running both arms silent settles which.
                if (!g_quiet)
                log_num("measured: rendered fps ", (unsigned)(int)(g_rendered_fps + 0.5));
            {
                sonda_vtable();
                const LONG p4168 = sonda_4168();
                log_num("  [global+0x4168] ", (unsigned)p4168);
                log_num("  techo declarado a la API ", (unsigned)g_api_aplicada);
                // Las tres variables juntas, para no descartar de a una.
                log_num("  g_force_sel ", (unsigned)g_force_sel);
                log_num("  g_force_generated ", (unsigned)g_force_generated);
                log_num("  cuenta_es_multiplicador ", (unsigned)(cuenta_es_multiplicador() ? 1 : 0));
                log_num("  byte vivo (la cadencia) ", (unsigned)g_count_live);
            }
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
                        // La base del HUD sale de Reflex, no de nuestro gate.
                        //
                        // g_rendered_fps cuenta el gate de frames, que dispara
                        // varias veces por frame renderizado -- en Cyberpunk da
                        // 113 donde la base real es 39. Ese numero en pantalla
                        // seria una mentira prolija. g_rfx_base es la que el
                        // propio Reflex reporta, y es la que este archivo ya
                        // trata como honesta unas lineas mas abajo.
                        if (g_rfx_base > 1.0)
                            g_hud_base_x10 = (LONG)(g_rfx_base * 10.0 + 0.5);
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
                        // Solo las tres lineas de Present dependen de NUESTRO
                        // hook. Todo lo demas de este bloque -- la latencia
                        // por Reflex, el foco, lo que pidio el juego, la
                        // cadencia -- se media igual sin el, y estaba adentro
                        // del mismo `if`: con el overlay de Steam (sin hook)
                        // la latencia desaparecio del log y del HUD en los tres
                        // juegos, desde 2f0d758.
                        {
                            if (g_pres_n > 0) {
                                log_num("  present ms avg x10 ",
                                        (unsigned)(g_pres_ms_sum / (double)g_pres_n * 10.0));
                                log_num("  present ms max x10 ", (unsigned)(g_pres_ms_max * 10.0));
                                log_num("  hitches over 33ms ", (unsigned)g_pres_hitch);
                            }
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
                            // Cadencia sin escala: desvio relativo de los
                            // intervalos contra la media de ESTA ventana.
                            if (g_cv_n > 2) {
                                const double media = g_cv_sum / (double)g_cv_n;
                                const double var = g_cv_sq / (double)g_cv_n - media * media;
                                if (media > 0.0 && var > 0.0) {
                                    double sd = var;
                                    for (int it = 0; it < 20; ++it) sd = 0.5 * (sd + var / sd);
                                    log_num("  cadencia: desvio relativo x1000 ",
                                            (unsigned)(sd / media * 1000.0));
                                }
                                g_cv_sum = 0.0; g_cv_sq = 0.0; g_cv_n = 0;
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
                        // PANTALLA: el mismo analisis sobre el reloj que se ve.
                        // Se emite aparte y con los mismos baldes a proposito,
                        // para poder comparar los dos relojes lado a lado.
                        if (g_disp_changes > 0) {
                            log_num("  PANTALLA cambios de imagen ", (unsigned)g_disp_changes);
                            log_num("    presentaciones ", (unsigned)g_disp_presents);
                            log_num("    presentaciones por cambio x100 ",
                                    (unsigned)((unsigned long long)g_disp_presents * 100ULL
                                               / (unsigned)g_disp_changes));
                            log_num("    display ms avg x10 ",
                                    (unsigned)(g_disp_ms_sum / (double)g_disp_changes * 10.0));
                            log_num("    display ms max x10 ", (unsigned)(g_disp_ms_max * 10.0));
                            log_num("    hitches de pantalla over 33ms ", (unsigned)g_disp_hitch);
                            log_num("    refreshes salteados ", (unsigned)g_disp_skip);
                            if (g_ref_qpc0 != 0 && g_qpc_freq > 0 && g_ref_last > g_ref_first) {
                                LARGE_INTEGER ahora;
                                QueryPerformanceCounter(&ahora);
                                const double seg = (double)(ahora.QuadPart - g_ref_qpc0)
                                                 / (double)g_qpc_freq;
                                if (seg > 0.05) {
                                    log_num("    refreshes del panel por seg ",
                                            (unsigned)((double)(g_ref_last - g_ref_first) / seg));
                                    log_num("    presentaciones por refresh x100 ",
                                            (unsigned)((double)g_disp_presents * 100.0
                                                       / (double)(g_ref_last - g_ref_first)));
                                }
                            }
                            g_ref_first = 0; g_ref_last = 0; g_ref_qpc0 = 0;
                            for (int b = 0; b < 6; ++b)
                                if (g_disp_bucket[b] > 0) {
                                    log_num("    dbucket ", (unsigned)b);
                                    log_num("      count ", (unsigned)g_disp_bucket[b]);
                                }
                            g_disp_ms_sum = 0.0; g_disp_ms_max = 0.0;
                            g_disp_changes = 0; g_disp_presents = 0; g_disp_hitch = 0;
                            g_disp_skip = 0;
                            for (int b = 0; b < 6; ++b) g_disp_bucket[b] = 0;
                        }
                        log_num("  counted multiplier x100 ",
                                (unsigned)((unsigned long long)dp * 100ULL / 45ULL));
                    }
                }
            }
            log_burst_end();
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
