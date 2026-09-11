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

// Globales que solo usa este modulo (movidas de proxy.cpp).
static unsigned char g_probe[16];

static double g_win_time = 0.0;       // elapsed in the current counting window
static int    g_win_frames = 0;       // rendered frames in it
static double g_rendered_fps = 0.0;   // frames / elapsed, unbiased
static double g_token_dt_fast = 0.0;  // la misma senal, para el controlador
static long long g_last_token_qpc = 0;


static volatile LONG g_present_mismatch = 0;   // windows where we disagree
// Gap histogram, in the token hook. Buckets in milliseconds:
//   0: <0.5   1: 0.5-2   2: 2-4   3: 4-8   4: 8-16   5: >=16
static int g_gap_hist[6] = {0,0,0,0,0,0};
static int g_raw_calls = 0;
// ---- la ventana cerrada, en tres pasos: foto, volcado, acciones ---------
//
// Antes esto era un solo bloque de 300 lineas donde cada log_num iba
// pegado al reset del contador que imprimia y a las acciones (HUD,
// controlador) que dependian de el. Ahora: snapshot_window() saca la foto de
// TODOS los contadores; dump_window() imprime esa foto, en el MISMO orden
// de lineas que antes; y close_window() hace las acciones y los resets. Lo
// unico que cambia en el log es que las lineas del controlador ("dynbias:",
// "sat:") salen despues del volcado en vez de en el medio.

// Los dos contadores acumulados de la ventana anterior. Eran estaticos de
// funcion; viven aca para que la foto los pueda leer y avanzar.
static LONG g_win_last_pres = 0;
static LONG g_win_last_hook = 0;

struct ClosedWindow {
    double elapsed;            // segundos de la ventana
    double rendered_fps;
    LONG   api_applied, force_sel, force_generated, count_live;
    bool   multiplier;
    double rfx_base;
    LONG   dp, rt_dp;          // presentaciones del runtime y del hook
    bool   mismatch;           // los dos difieren en mas de 2
    LONG   mismatch_total;     // g_present_mismatch ya incrementado
    int    raw_calls;
    double token_block_us, present_block_us;
    int    clamp_latency; LONG smfl_calls, token_calls, frames_gated;
    LONG   step_same, step_plus_one, step_forward, step_back;
    int    gap_hist[6];
    int    pres_n; double pres_ms_sum, pres_ms_max; int pres_hitch;
    LONG   rfx_n; double rfx_gpu, rfx_drv, rfx_ft; unsigned rfx_min, rfx_max;
    bool   in_front; LONG cap_mode, cap_cnt;
    int    near_n, near_bad, far_n, far_bad;
    int    cv_n; double cv_sum, cv_sq;
    int    hitch_near, hitch_far;
    int    pres_bucket[6];
    int    disp_changes, disp_presents; double disp_ms_sum, disp_ms_max;
    int    disp_hitch, disp_skip; unsigned ref_first, ref_last; double ref_seg;
    int    disp_bucket[6];
};

// La foto. Lee todo lo que el volcado imprime, antes de que nadie lo resetee,
// y avanza los dos contadores acumulados.
static void snapshot_window(ClosedWindow &w, double win_elapsed) {
    w.elapsed = win_elapsed;
    w.rendered_fps = g_rendered_fps;
    w.api_applied = g_api_applied; w.force_sel = g_force_sel;
    w.force_generated = g_force_generated; w.count_live = g_count_live;
    w.multiplier = count_is_multiplier();
    w.rfx_base = g_rfx_base;
    {
        const LONG now = g_rt_present_count;
        w.dp = now - g_win_last_pres;
        g_win_last_pres = now;
        const LONG hook_now = g_present_count;
        w.rt_dp = hook_now - g_win_last_hook;
        g_win_last_hook = hook_now;
    }
    w.mismatch = w.rt_dp > 0 && (w.dp - w.rt_dp > 2 || w.rt_dp - w.dp > 2);
    w.mismatch_total = g_present_mismatch;
    w.raw_calls = g_raw_calls;
    w.token_block_us = g_token_block_us; w.present_block_us = g_present_block_us;
    w.clamp_latency = g_clamp_latency; w.smfl_calls = g_smfl_calls;
    w.token_calls = g_token_calls; w.frames_gated = g_frames_gated;
    w.step_same = g_step_same; w.step_plus_one = g_step_plus_one;
    w.step_forward = g_step_forward; w.step_back = g_step_back;
    for (int i = 0; i < 6; ++i) w.gap_hist[i] = g_gap_hist[i];
    w.pres_n = g_pres_n; w.pres_ms_sum = g_pres_ms_sum; w.pres_ms_max = g_pres_ms_max;
    w.pres_hitch = g_pres_hitch;
    w.rfx_n = g_rfx_n; w.rfx_gpu = g_rfx_gpu; w.rfx_drv = g_rfx_drv; w.rfx_ft = g_rfx_ft;
    w.rfx_min = g_rfx_min; w.rfx_max = g_rfx_max;
    w.in_front = g_game_hwnd != nullptr &&
                 GetForegroundWindow() == GetAncestor(g_game_hwnd, GA_ROOT);
    w.cap_mode = g_cap_mode; w.cap_cnt = g_cap_cnt;
    w.near_n = g_near_n; w.near_bad = g_near_bad; w.far_n = g_far_n; w.far_bad = g_far_bad;
    w.cv_n = g_cv_n; w.cv_sum = g_cv_sum; w.cv_sq = g_cv_sq;
    w.hitch_near = g_hitch_near; w.hitch_far = g_hitch_far;
    for (int b = 0; b < 6; ++b) w.pres_bucket[b] = g_pres_bucket[b];
    w.disp_changes = g_disp_changes; w.disp_presents = g_disp_presents;
    w.disp_ms_sum = g_disp_ms_sum; w.disp_ms_max = g_disp_ms_max;
    w.disp_hitch = g_disp_hitch; w.disp_skip = g_disp_skip;
    w.ref_first = g_ref_first; w.ref_last = g_ref_last;
    w.ref_seg = 0.0;
    if (g_ref_qpc0 != 0 && g_qpc_freq > 0 && g_ref_last > g_ref_first) {
        LARGE_INTEGER now_qpc;
        QueryPerformanceCounter(&now_qpc);
        w.ref_seg = (double)(now_qpc.QuadPart - g_ref_qpc0) / (double)g_qpc_freq;
    }
    for (int b = 0; b < 6; ++b) w.disp_bucket[b] = g_disp_bucket[b];
}

// El volcado: las mismas lineas, en el mismo orden, con las mismas guardas
// (mfg-quiet.txt y dp > 0). No toca ningun contador.
static void dump_window(const ClosedWindow &w) {
    if (!g_quiet)
    log_num("measured: rendered fps ", (unsigned)(int)(w.rendered_fps + 0.5));
    {
        sonda_vtable();
        const LONG p4168 = sonda_4168();
        log_num("  [global+0x4168] ", (unsigned)p4168);
        log_num("  techo declarado a la API ", (unsigned)w.api_applied);
        log_num("  g_force_sel ", (unsigned)w.force_sel);
        log_num("  g_force_generated ", (unsigned)w.force_generated);
        log_num("  cuenta_es_multiplicador ", (unsigned)(w.multiplier ? 1 : 0));
        log_num("  byte vivo (la cadencia) ", (unsigned)w.count_live);
    }
    if (!g_quiet)
    log_num("  base por Reflex ", (unsigned)(int)(w.rfx_base + 0.5));
    if (w.mismatch && !g_quiet)
        log_num("  WARNING present count disagrees, windows so far ",
                (unsigned)w.mismatch_total);
    if (!(w.dp > 0 && !g_quiet)) return;
    log_num("  runtime PresentCount this window ", (unsigned)w.dp);
    log_num("  our hook count this window ", (unsigned)(w.rt_dp > 0 ? w.rt_dp : 0));
    log_num("  raw token calls ", (unsigned)w.raw_calls);
    log_num("  blocked in the token call, ms ", (unsigned)(w.token_block_us / 1000.0));
    log_num("  blocked in Present, ms ", (unsigned)(w.present_block_us / 1000.0));
    log_num("  window elapsed, ms ", (unsigned)(w.elapsed * 1000.0));
    if (w.clamp_latency > 0)
        log_num("  SetMaximumFrameLatency calls so far ", (unsigned)w.smfl_calls);
    log_num("  hook calls total ", (unsigned)w.token_calls);
    log_num("  frames past the gate ", (unsigned)w.frames_gated);
    log_num("    salto igual ", (unsigned)w.step_same);
    log_num("    salto +1 ", (unsigned)w.step_plus_one);
    log_num("    salto adelante ", (unsigned)w.step_forward);
    log_num("    salto atras ", (unsigned)w.step_back);
    {
        static const char *kG[6] = {
            "    gap <0.5ms ", "    gap 0.5-2ms ",
            "    gap 2-4ms ", "    gap 4-8ms ",
            "    gap 8-16ms ", "    gap >=16ms " };
        for (int i = 0; i < 6; ++i)
            if (w.gap_hist[i] > 0) log_num(kG[i], (unsigned)w.gap_hist[i]);
    }
    if (w.pres_n > 0) {
        log_num("  present ms avg x10 ", (unsigned)(w.pres_ms_sum / (double)w.pres_n * 10.0));
        log_num("  present ms max x10 ", (unsigned)(w.pres_ms_max * 10.0));
        log_num("  hitches over 33ms ", (unsigned)w.pres_hitch);
    }
    if (w.rfx_n > 0) {
        log_num("  latency sim to gpu end us ", (unsigned)(w.rfx_gpu / w.rfx_n));
        log_num("  latency sim to driver end us ", (unsigned)(w.rfx_drv / w.rfx_n));
        log_num("  reflex frame time us ", (unsigned)(w.rfx_ft / w.rfx_n));
        log_num("    driver latency min us ", (unsigned)w.rfx_min);
        log_num("    driver latency max us ", (unsigned)w.rfx_max);
        log_num("    frames reported ", (unsigned)w.rfx_n);
    }
    log_num("  game window in front (1 = yes, NO CONFIABLE en cp2077) ", (unsigned)(w.in_front ? 1 : 0));
    log_num("  the game itself last asked for mode ", (unsigned)w.cap_mode);
    log_num("    with count ", (unsigned)w.cap_cnt);
    if (w.near_n + w.far_n > 0) {
        log_num("    off-refresh near a change x1000 ",
                (unsigned)(w.near_n ? (unsigned long long)w.near_bad * 1000ULL / (unsigned)w.near_n : 0));
        log_num("      of presents ", (unsigned)w.near_n);
        log_num("    off-refresh away x1000 ",
                (unsigned)(w.far_n ? (unsigned long long)w.far_bad * 1000ULL / (unsigned)w.far_n : 0));
        log_num("      of presents ", (unsigned)w.far_n);
    }
    if (w.cv_n > 2) {
        const double mean = w.cv_sum / (double)w.cv_n;
        const double var = w.cv_sq / (double)w.cv_n - mean * mean;
        if (mean > 0.0 && var > 0.0) {
            double sd = var;
            for (int it = 0; it < 20; ++it) sd = 0.5 * (sd + var / sd);
            log_num("  cadencia: desvio relativo x1000 ", (unsigned)(sd / mean * 1000.0));
        }
    }
    if (w.hitch_near + w.hitch_far > 0) {
        log_num("    at a count change ", (unsigned)w.hitch_near);
        log_num("    away from one ", (unsigned)w.hitch_far);
    }
    for (int b = 0; b < 6; ++b)
        if (w.pres_bucket[b] > 0) {
            log_num("    bucket ", (unsigned)b);
            log_num("      count ", (unsigned)w.pres_bucket[b]);
        }
    if (w.disp_changes > 0) {
        log_num("  PANTALLA cambios de imagen ", (unsigned)w.disp_changes);
        log_num("    presentaciones ", (unsigned)w.disp_presents);
        log_num("    presentaciones por cambio x100 ",
                (unsigned)((unsigned long long)w.disp_presents * 100ULL / (unsigned)w.disp_changes));
        log_num("    display ms avg x10 ", (unsigned)(w.disp_ms_sum / (double)w.disp_changes * 10.0));
        log_num("    display ms max x10 ", (unsigned)(w.disp_ms_max * 10.0));
        log_num("    hitches de pantalla over 33ms ", (unsigned)w.disp_hitch);
        log_num("    refreshes salteados ", (unsigned)w.disp_skip);
        if (w.ref_seg > 0.05) {
            log_num("    refreshes del panel por seg ",
                    (unsigned)((double)(w.ref_last - w.ref_first) / w.ref_seg));
            log_num("    presentaciones por refresh x100 ",
                    (unsigned)((double)w.disp_presents * 100.0 / (double)(w.ref_last - w.ref_first)));
        }
        for (int b = 0; b < 6; ++b)
            if (w.disp_bucket[b] > 0) {
                log_num("    dbucket ", (unsigned)b);
                log_num("      count ", (unsigned)w.disp_bucket[b]);
            }
    }
    log_num("  counted multiplier x100 ", (unsigned)((unsigned long long)w.dp * 100ULL / 45ULL));
}

// Las acciones y los resets, con las mismas guardas que tenian. El
// log_burst_end() sigue en note_rendered_frame, una vez por frame, como
// estaba.
static void close_window(double win_elapsed) {
    // Todo el volcado de la ventana en una sola apertura.
    log_burst_begin();
    ClosedWindow w;
    snapshot_window(w, win_elapsed);
    if (w.mismatch) {
        InterlockedIncrement(&g_present_mismatch);
        w.mismatch_total = g_present_mismatch;
    }
    dump_window(w);
    if (w.dp > 0 && !g_quiet) {
        g_token_block_us = 0.0;
        if (win_elapsed > 0.0)
            g_hud_fps_x10 = (LONG)(w.dp * 10.0 / win_elapsed + 0.5);
        if (g_rfx_base > 1.0)
            g_hud_base_x10 = (LONG)(g_rfx_base * 10.0 + 0.5);
        if (win_elapsed > 0.0)
            dyn_control(g_ctrl_fps > 0.0 ? g_ctrl_fps : g_base_fps,
                        w.dp / win_elapsed);
        if (win_elapsed > 0.0 && g_rfx_base > 1.0)
            g_interp_on = (w.dp / win_elapsed) > g_rfx_base * 1.3 ? 1 : 0;
        g_present_block_us = 0.0;
        g_raw_calls = 0;
        for (int i = 0; i < 6; ++i) g_gap_hist[i] = 0;
        if (w.rfx_n > 0) {
            g_hud_lat_us = (LONG)(w.rfx_drv / w.rfx_n);
            g_rfx_gpu = 0.0; g_rfx_drv = 0.0;
            g_rfx_ft = 0.0; g_rfx_n = 0;
            g_rfx_min = 0xFFFFFFFFu; g_rfx_max = 0;
        }
        if (w.near_n + w.far_n > 0) { g_near_n = 0; g_near_bad = 0; g_far_n = 0; g_far_bad = 0; }
        if (w.cv_n > 2) { g_cv_sum = 0.0; g_cv_sq = 0.0; g_cv_n = 0; }
        if (w.hitch_near + w.hitch_far > 0) { g_hitch_near = 0; g_hitch_far = 0; }
        g_pres_ms_sum = 0.0; g_pres_ms_max = 0.0;
        g_pres_n = 0; g_pres_hitch = 0;
        for (int b = 0; b < 6; ++b) g_pres_bucket[b] = 0;
        if (w.disp_changes > 0) {
            g_ref_first = 0; g_ref_last = 0; g_ref_qpc0 = 0;
            g_disp_ms_sum = 0.0; g_disp_ms_max = 0.0;
            g_disp_changes = 0; g_disp_presents = 0; g_disp_hitch = 0;
            g_disp_skip = 0;
            for (int b = 0; b < 6; ++b) g_disp_bucket[b] = 0;
        }
    }
}
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
                close_window(win_elapsed);
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
                const bool far_off = dt < lo || dt > hi;
                const bool similar = prev_dt > 0.0 &&
                                   dt > prev_dt * 0.88 && dt < prev_dt * 1.12;
                if (far_off && similar) g_token_dt_fast = dt;
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
