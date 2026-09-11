// frametoken.h -- el pulso por frame: el hook de slGetNewFrameToken, que es
// donde corre todo lo que este proyecto hace "una vez por frame renderizado".
//
// ENTRA: cada llamada del juego a slGetNewFrameToken (varias por frame en
//   Cyberpunk, [[frame-token-burst]]; se filtra por el indice del frame).
// SALE: en orden, por frame: evaluate_invariants (una vez), runtime_presents
//   (el contador sin hook de Present), la correccion de la cuenta del modo
//   fijo, read_max_generated, note_rendered_frame (measurement.h),
//   reflex_poll (reflex.h), fractional_tick / apply_override_now (writer.h),
//   query_state, settings_watch. arm_frametoken_hook lo instala y arma el
//   grabador DXGI con el.
// DEPENDE DE: measurement.h, writer.h, reflex.h, present.h, loader.h,
//   MinHook, y los globales compartidos que todavia viven en proxy.cpp.
//
// Movido de proxy.cpp tal cual (2026-09-11): un rango, mismo orden. Sin
// tocar una linea del cuerpo.
#pragma once

// Globales que solo usa este modulo (movidas de proxy.cpp).
// The game asks for a frame token once per frame, from its own render thread
// -- which is the thread that called slDLSSGSetOptions and the only one it is
// safe to call it from again. With DLSS-G on, Present is Streamline's thread,
// so the replay cannot happen there; the log said so outright.
static PFN_slGetNewFrameToken g_orig_frametoken = nullptr;

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
    // El grafo ya cargo entero: aca se deciden los invariantes, una sola vez.
    evaluate_invariants();
    // Y si no hay hook de Present, el contador se alimenta desde aca.
    runtime_presents();
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
            if (*idx == last_idx)      ++g_step_same;
            else if (d == 1u)          ++g_step_plus_one;
            else if (d < 0x80000000u)  ++g_step_forward;
            else                       ++g_step_back;
        }
        last_idx = (idx != nullptr) ? *idx : last_idx;
        last_tok = tok;
        have = true;
        ++g_token_calls;
        if (same) return r;
        ++g_frames_gated;
    }
    // La cuenta del modo fijo, corregida sin depender del orden de arranque.
    //
    // El mapeo modo->cuenta se aplica al restaurar los settings, a los 0 ms,
    // cuando todavia no se sabe si cargamos nuestro snippet -- y con el nuestro
    // la cuenta ES el multiplicador, no los generados. Recalcularlo en el
    // bloque de flags no alcanzo (ordenes distintos en cada juego). Aca se
    // compara cada frame y se corrige si hace falta: cuesta una comparacion y
    // es cierto siempre.
    if (count_is_multiplier() && g_force_sel >= 2 && g_force_sel <= kSelMaxFixed &&
        g_force_generated != g_force_sel) {
        log_num("cuenta: el modo fijo pedia ", (unsigned)g_force_generated);
        log_num("  con nuestro snippet corresponde ", (unsigned)g_force_sel);
        g_force_generated = g_force_sel;
        g_opt_pending = 1;
    }
    // Una vez por frame: el plugin lo puede recalcular al cambiar de modo.
    read_max_generated();
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
