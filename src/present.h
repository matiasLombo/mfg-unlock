// present.h -- el swapchain y Present: como se adopta el swapchain del juego,
// como se cuentan las presentaciones y como se engancha (o no) Present.
//
// ENTRA: la creacion de swapchains por la fabrica DXGI (hk_csc / hk_cscfh /
//   hk_f0-f2, instalados por arm_dxgi_recorder), la adopcion por vtable
//   compartida (adopt_existing_swapchain), y por frame el hook de Present
//   (hk_dxgi_present) cuando el slot se pudo escribir; con el overlay de
//   Steam presente el slot NO se escribe y presentes_del_runtime() lee
//   GetLastPresentCount desde el frame token.
// SALE: g_rt_present_count (el contador del runtime, lo lee measurement.h),
//   g_present_count (la cuenta del hook, el cross-check), g_swapchain (la
//   ultima instancia adoptada), g_game_hwnd, las estadisticas de
//   presentacion y de pantalla (g_pres_*, g_disp_*) que measurement.h vuelca,
//   y hook_frame_latency (el clamp de SetMaximumFrameLatency).
// DEPENDE DE: DXGI, MinHook para la fabrica, log_line/log_num, el grabador
//   (note_present) y los globales compartidos que todavia viven en proxy.cpp.
//
// Movido de proxy.cpp tal cual (2026-09-11): mismas lineas, mismo orden de
// declaracion, el #include quedo donde estaba el bloque. Renombrar es el
// paso siguiente. Reglas que este archivo carga: [[never-byte-detour-present]]
// y [[measure-with-the-runtime-counter]].
#pragma once

// Globales que solo usa este modulo (movidas de proxy.cpp).
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
static int    g_freeze_said = 0;          // tope de lineas del diagnostico
static long long g_pres_qpc = 0;            // when the last present went out
// The swap chain, kept so the runtime's own present counter can be sampled
// from anywhere -- specifically from the frame-token hook, once per rendered
// frame, off the present path entirely.
static IDXGISwapChain *g_swapchain = nullptr;

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

// El PRIMER original enganchado. Se conserva solo como bandera de "ya hay
// enganche" para el codigo que pregunta != nullptr; para llamar al original
// se usa present_original_de(self), nunca este puntero.
static PFN_DXGIPresent g_orig_dxgi_present = nullptr;
// Cierto cuando NO pudimos enganchar Present (overlay de Steam): el contador de
// presentaciones pasa a leerse del runtime. Ver presentes_del_runtime.
static bool g_present_via_runtime = false;

// Un original POR VTABLE, no uno por proceso.
//
// Halo, 2026-09-10, 0xC00000FD a los 53 s: tres swapchains y dos vtables
// distintas en la misma sesion. El interposer de Streamline envuelve el
// swapchain del juego con un proxy que tiene su propia vtable, y el Present
// del proxy llama al Present del chain real por la vtable de este. Con las dos
// vtables apuntando a hk_dxgi_present y UN solo original guardado (el de la
// primera), el hook llamaba al Present del proxy con el chain real como self,
// el proxy volvia a entrar por el slot del chain real -- que es el hook --
// y asi hasta agotar la pila. Cyberpunk no lo mostraba porque ahi las dos
// comparten vtable, y de ese negativo se habia concluido que la teoria estaba
// muerta.
//
// La regla: el original que se llama es el de la vtable del objeto que llega,
// buscado en esta tabla. Ocho entradas es holgado: son CLASES de swapchain
// (DXGI real, proxy de SL, el descartable de la adopcion), no instancias.
struct VtPresent { void **vt; PFN_DXGIPresent orig; };
static VtPresent g_vt_present[8];
static volatile LONG g_vt_present_n = 0;
// Cuantas veces el hook corrio anidado dentro de si mismo (ver hk_dxgi_present).
static volatile LONG g_present_nested = 0;
// Profundidad de hk_dxgi_present en ESTE hilo. Sube al entrar y baja al salir
// por RAII, asi que una excepcion adentro del original tampoco la deja torcida.
static thread_local int g_present_depth = 0;
struct NestedPresent {
    NestedPresent() { ++g_present_depth; }
    ~NestedPresent() { --g_present_depth; }
};

static PFN_DXGIPresent present_original_for(IDXGISwapChain *self) {
    if (self == nullptr) return nullptr;
    void **vt = *reinterpret_cast<void ***>(self);
    const LONG n = g_vt_present_n;
    for (LONG i = 0; i < n; ++i)
        if (g_vt_present[i].vt == vt) return g_vt_present[i].orig;
    return nullptr;
}

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
// Sin hook de Present, el contador sale de donde siempre debio salir.
//
// g_present_count lo alimentaba nuestro propio hook. Con el overlay de Steam no
// lo podemos enganchar, asi que se lee PresentCount de GetFrameStatistics, que
// es el contador del runtime -- el que este proyecto ya habia concluido que era
// el honesto ([[measure-with-the-runtime-counter]]). Se llama desde el camino
// del frame token, que corre igual.
static IDXGISwapChain *live_swapchain(void);   // mas abajo, con la adopcion
static void runtime_presents(void) {
    if (!g_present_via_runtime) return;
    IDXGISwapChain *chain = live_swapchain();
    if (chain == nullptr) return;
    // GetLastPresentCount, no GetFrameStatistics.
    //
    // La primera version usaba GetFrameStatistics y no conto NUNCA: esa llamada
    // falla si el swapchain no esta en modo flip -- devuelve
    // DXGI_ERROR_FRAME_STATISTICS_DISJOINT o directamente error -- y salia por
    // el FAILED sin sumar. Medido en Cyberpunk: 108 s, cero ventanas de
    // medicion, cero lineas de PresentCount, con el bloque de la ventana
    // corriendo igual ("measured: rendered fps 54"). El HUD quedaba en cero,
    // que es lo que el usuario reporto.
    //
    // GetLastPresentCount devuelve el contador del runtime sin depender del
    // modo de presentacion, que es justo lo que hace falta aca.
    UINT now_qpc = 0;
    const HRESULT hr = chain->GetLastPresentCount(&now_qpc);
    if (FAILED(hr)) {
        static bool said = false;
        if (!said) {
            said = true;
            diag::Line l = diag::invariant(diag::Layer::PRESENTATION, "contador",
                                             "GetLastPresentCount fallo; el contador queda en cero");
            l.pair("hr", (long long)(long)hr);
            log_line(l.b);
        }
        return;
    }
    // Al contador del RUNTIME, no al del hook.
    //
    // La primera version sumaba el delta a g_present_count, que es la cuenta
    // de NUESTRO hook de Present -- el cross-check -- y no a
    // g_rt_present_count, que es de donde leen la ventana de medicion, el
    // controlador DYNAMIC y la sonda. Medido en Cyberpunk desde Steam con la
    // adopcion ya arreglada: 351 ventanas de "present count disagrees"
    // (hook > 0, runtime = 0) y cero lineas de PresentCount. El contador
    // andaba; alimentaba al instrumento equivocado.
    //
    // GetLastPresentCount es un total acumulado, igual que el PresentCount
    // que el hook muestrea de GetFrameStatistics: se asigna, no se suma.
    g_rt_present_count = (LONG)now_qpc;
}

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
    // Primero que nada: a quien se le devuelve la llamada. Si este objeto tiene
    // una vtable que no enganchamos -- no deberia pasar, porque solo se llega
    // aca por un slot que nosotros escribimos -- se avisa una vez y se usa el
    // primer original, que es lo que hacia el codigo anterior siempre.
    PFN_DXGIPresent orig = present_original_for(self);
    if (orig == nullptr) {
        static LONG said = 0;
        if (InterlockedCompareExchange(&said, 1, 0) == 0)
            log_line("present: objeto con vtable desconocida; se usa el primer original");
        orig = g_orig_dxgi_present;
        if (orig == nullptr) return (HRESULT)0x887A0001L;   // DXGI_ERROR_INVALID_CALL
    }
    // Anidamiento: el Present del proxy de Streamline llama al Present del
    // chain real, y con las dos vtables enganchadas este hook corre dos veces
    // por presentacion del juego (mas las presentaciones generadas, que van
    // solo por el chain real). Se deja constancia una vez para que quien lea
    // los contadores por hook sepa que cuentan los dos niveles; el contador
    // que no depende de esto es PresentCount de GetFrameStatistics.
    NestedPresent nested;
    // FRENO DURO DE RECURSION.
    //
    // Con la tabla por vtable el anidamiento deberia ser de DOS niveles y
    // terminar: el proxy de Streamline presenta, eso entra por el slot del chain
    // real, y el original de ESE chain es el Present de DXGI, que no vuelve.
    //
    // Halo, 2026-09-10, con su interposer 2.7.30: dos vtables enganchadas, cero
    // "vtable desconocida" -- las dos con original propio -- y 0xC00000FD 16 ms
    // despues de la primera llamada anidada. Si desborda, la recursion NO esta
    // acotada en dos, y la tabla sola no alcanza.
    //
    // A partir del tercer nivel se llama al original y se vuelve, sin hacer nada
    // mas: sin contar la presentacion, sin cadencia, sin pacer. Se pierde
    // instrumentacion en esos frames; no se pierde el proceso. Y la profundidad
    // queda en el log, que es el dato que falta para entender por que hay mas de
    // dos niveles.
    if (g_present_depth > 2) {
        static LONG max_seen = 0;
        if (g_present_depth > max_seen) {
            max_seen = g_present_depth;
            log_num("present: RECURSION, profundidad ", (unsigned)g_present_depth);
        }
        return orig(self, interval, flags);
    }
    if (g_present_depth > 1) {
        InterlockedIncrement(&g_present_nested);
        static LONG said_nested = 0;
        if (InterlockedCompareExchange(&said_nested, 1, 0) == 0)
            log_line("present: llamada anidada (proxy de SL -> chain real); "
                     "los contadores por hook cuentan los dos niveles");
    }
    // Presented frames, counted. Nothing else in this file has ever counted
    // one, and that is why three separate multiplier instruments in one
    // session were all circular -- the last of them reduced to "what fraction
    // of my samples did I take while the high count was selected" and returned
    // the requested ratio to within 0.3% even at 2.10x and 2.90x, fractions its
    // own scheduler cannot represent. An instrument that cannot fail measures
    // nothing.
    InterlockedIncrement(&g_present_count);
    // Tambien desde aca, no solo desde el lazo de teclas: ese lazo NO corre bajo
    // el banco (lo dice el comentario de arm_frametoken_hook, y por eso el
    // enganche de DXGI se armaba tarde ahi). Sin esto el veredicto de M1 no se
    // podria medir en la unica herramienta con la que se permite probar.
    // Se auto-limita: despues de la primera emision es una lectura de bool.
    emit_verdict_if_due();
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
            // Y el reloj de la pantalla, del mismo muestreo. Una presentacion
            // que no mueve SyncQPCTime no puso una imagen nueva en el panel.
            ++g_disp_presents;
            if (g_ref_first == 0) { g_ref_first = fs.PresentRefreshCount; g_ref_qpc0 = now.QuadPart; }
            g_ref_last = fs.PresentRefreshCount;
            if (fs.SyncQPCTime.QuadPart != g_disp_qpc) {
                if (g_disp_qpc != 0) {
                    const double dms = (double)(fs.SyncQPCTime.QuadPart - g_disp_qpc)
                                     / (double)g_qpc_freq * 1000.0;
                    if (dms > 0.0 && dms < 500.0) {
                        g_disp_ms_sum += dms;
                        ++g_disp_changes;
                        if (dms > g_disp_ms_max) g_disp_ms_max = dms;
                        const int b = dms < 4.0 ? 0 : dms < 8.0 ? 1 : dms < 12.0 ? 2
                                    : dms < 20.0 ? 3 : dms < 33.0 ? 4 : 5;
                        ++g_disp_bucket[b];
                        if (dms > 33.0) ++g_disp_hitch;
                        if (fs.PresentRefreshCount - g_disp_refresh > 1) ++g_disp_skip;
                    }
                }
                g_disp_qpc = fs.SyncQPCTime.QuadPart;
                g_disp_refresh = fs.PresentRefreshCount;
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
                    {
                        g_cv_sum += ms; g_cv_sq += ms * ms; ++g_cv_n;
                    }
                }
                if (ms > 33.0) {
                    ++g_pres_hitch;
                    // How many presents ago the count last changed. Zero means
                    // this hitch is the change itself.
                    const LONG since = g_present_count - g_last_change_pres;
                    if (since < 4) ++g_hitch_near;
                    else ++g_hitch_far;
                    // El ESTADO en el instante del congelamiento, no el resumen
                    // de la ventana.
                    //
                    // El resumen dice "presMax 284.7 ms, 12 hitches" y con eso no
                    // se puede decidir nada: no dice si la cuenta acababa de
                    // cambiar ni si el byte y la reserva estaban en el mismo
                    // escalon. La tabla medida tiene las dos filas fatales --
                    // byte por encima de la reserva crashea, por debajo DETIENE
                    // LA PRESENTACION -- y entrar al menu es justo cuando el
                    // juego apaga la generacion y la cuenta se mueve.
                    //
                    // Umbral alto y tope de lineas: esto escribe al log desde el
                    // hilo de Present, y log_line abre el archivo por linea.
                    if (ms > 60.0 && g_freeze_said < 40) {
                        ++g_freeze_said;
                        log_num("CONGELAMIENTO: present ms x10 ", (unsigned)(ms * 10.0));
                        log_num("  presentaciones desde el ultimo cambio de cuenta ",
                                (unsigned)since);
                        log_num("  byte vivo ", (unsigned)g_count_live);
                        log_num("  cuenta aplicada en la API ", (unsigned)g_api_applied);
                        log_num("  cuenta pedida ", (unsigned)g_force_generated);
                        log_num("  seleccion ", (unsigned)g_force_sel);
                        log_num("  interpolacion encendida ", (unsigned)(g_interp_on ? 1 : 0));
                        log_num("  ventana al frente ",
                                (unsigned)(g_game_hwnd != nullptr &&
                                           GetForegroundWindow() == g_game_hwnd ? 1 : 0));
                        log_num("  lo ultimo que pidio el juego, cuenta ",
                                (unsigned)g_last_seen_generated);
                    }
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
            const HRESULT hr = orig(self, 0, flags | 0x00000200 /*ALLOW_TEARING*/);
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
        if (g_host_on) host_pcl(sl::PCLMarker::ePresentStart);
        const HRESULT hr = orig(self, interval, flags);
        if (g_host_on) host_pcl(sl::PCLMarker::ePresentEnd);
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
static PFN_SMFL g_orig_smfl = nullptr;

// Lo adoptado se suelta cuando se destruye. "La ultima instancia gana" dejo
// un puntero colgado en GTA V (2026-09-11): a los 52 s alguien creo un
// swapchain transitorio (CreateSwapChain flags 2, no el del juego), se adopto,
// lo liberaron, y a los 111 s hk_slGetNewFrameToken leyo su vtable ya basura
// (0xC0000005 en GetLastPresentCount, vtable 0xAE000). Antes no pasaba solo
// porque con ReShade en el slot no se adoptaba nada.
//
// La primera version enganchaba el slot 2 de la vtable (IUnknown::Release)
// para enterarse de la destruccion. El overlay de Steam tambien lo engancha
// -- la misma trampa que Present, [[never-byte-detour-present]] -- y
// Cyberpunk cayo con 0xC00000FD en gameoverlayrenderer64.dll al crear su
// swapchain. Asi que sin hooks: antes de usar el adoptado se comprueba que
// su pagina este mapeada y que su vtable apunte adentro de un modulo. Si no,
// se suelta y vuelve el anterior que siga vivo. Dos VirtualQuery por frame
// renderizado.
static IDXGISwapChain *g_adopted[4] = { nullptr, nullptr, nullptr, nullptr };

static void adopt_swapchain(void *sc) {
    IDXGISwapChain *c = reinterpret_cast<IDXGISwapChain *>(sc);
    int slot = -1;
    for (int i = 0; i < 4; ++i) if (g_adopted[i] == c) { slot = i; break; }
    if (slot < 0) {
        for (int i = 3; i > 0; --i) g_adopted[i] = g_adopted[i - 1];   // el mas nuevo primero
        g_adopted[0] = c;
    }
    g_swapchain = c;
}
static void forget_swapchain(IDXGISwapChain *c) {
    for (int i = 0; i < 4; ++i) {
        if (g_adopted[i] != c) continue;
        for (int j = i; j < 3; ++j) g_adopted[j] = g_adopted[j + 1];
        g_adopted[3] = nullptr;
        break;
    }
    if (g_swapchain == c) g_swapchain = g_adopted[0];
}
static bool swapchain_looks_alive(const IDXGISwapChain *c) {
    MEMORY_BASIC_INFORMATION mb;
    if (VirtualQuery(c, &mb, sizeof mb) != sizeof mb) return false;
    if (mb.State != MEM_COMMIT || (mb.Protect & PAGE_GUARD) || (mb.Protect & PAGE_NOACCESS)) return false;
    const void *vt = *reinterpret_cast<const void *const *>(c);
    HMODULE owner = nullptr;
    return vt != nullptr &&
           GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCWSTR)vt, &owner) && owner != nullptr;
}
// El adoptado vivo, o nullptr. Descarta los muertos al pasar.
static IDXGISwapChain *live_swapchain(void) {
    for (int k = 0; k < 4; ++k) {
        IDXGISwapChain *c = g_swapchain;
        if (c == nullptr) return nullptr;
        if (swapchain_looks_alive(c)) return c;
        log_line("present: el swapchain adoptado ya no existe; se suelta");
        forget_swapchain(c);
    }
    return nullptr;
}

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
    if (sc == nullptr) return;
    void **vt = *reinterpret_cast<void ***>(sc);
    // Una vtable ya enganchada no se toca: su original ya esta en la tabla y
    // volver a leer vt[8] guardaria nuestro propio hook como "original", que
    // es una recursion garantizada. Esto cubre tambien el caso de la adopcion
    // ganandole la carrera al juego (medido en Cyberpunk: el descartable a los
    // 6,5 s, el del juego a los 10,6 s, misma vtable): la segunda llegada
    // simplemente no hace nada, y el contador ya cuenta por esa vtable.
    for (LONG i = 0; i < g_vt_present_n; ++i)
        if (g_vt_present[i].vt == vt) {
            // La vtable ya esta hecha, pero la INSTANCIA es nueva y lo que es
            // por instancia se toma igual. Medido en Cyberpunk desde Steam: la
            // adopcion registra la vtable desde el descartable y lo suelta
            // (g_swapchain = nullptr); el swapchain real del juego llegaba
            // aca, salia por este return, y presentes_del_runtime se quedaba
            // sin swapchain que leer: cero lineas de PresentCount y el HUD en
            // cero. La ultima instancia gana, igual que en hk_cscfh.
            adopt_swapchain(sc);
            hook_frame_latency(sc);
            log_line("present: vtable ya enganchada; se adopta la instancia nueva");
            return;
        }
    if (vt[8] == reinterpret_cast<void *>(&hk_dxgi_present)) {
        // Slot nuestro en una vtable que no registramos: no puede pasar salvo
        // que la tabla se haya llenado o que alguien haya copiado la vtable.
        // En cualquier caso no hay original que guardar y no se toca.
        log_line("recorder: vtable con nuestro hook puesto y sin original registrado; no se toca");
        return;
    }
    if (g_vt_present_n >= (LONG)(sizeof(g_vt_present) / sizeof(g_vt_present[0]))) {
        log_line("recorder: tabla de vtables llena; este swapchain no se engancha");
        return;
    }
    if (g_orig_dxgi_present != nullptr)
        log_line("recorder: aparecio otro swapchain con vtable distinta; se engancha tambien");
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
    // Con el overlay de Steam en el proceso NO se engancha Present. Punto.
    //
    // El overlay no toma el slot de la vtable: hace byte-detour de la funcion.
    // Nuestro "original" apunta a la funcion real, cuyos primeros bytes ahora
    // saltan al overlay, y el overlay presenta por la vtable -- que somos
    // nosotros. El lazo es invisible desde el slot, y por eso la guarda de
    // "quien es el dueno de vt[8]" no lo ve: medido, cuatro enganches, misma
    // vtable, cero dueños ajenos, y recursion igual.
    //
    //   present: RECURSION, profundidad 4249
    //   EXCEPCION 0xC00000FD en gameoverlayrenderer64.dll
    //
    // [[never-byte-detour-present]] ya decia que el overlay de Steam engancha
    // los mismos bytes y que quien instala segundo gana. Lo que faltaba era
    // sacar la conclusion: con el overlay presente no hay forma segura de
    // llamar al original, asi que no se engancha.
    //
    // El costo es instrumentacion, no funcionalidad. Las presentaciones se
    // cuentan con PresentCount del runtime ([[measure-with-the-runtime-counter]]),
    // que es el instrumento honesto igual, y el pacer propio queda apagado en
    // esa configuracion.
    // No enganchar NO es abandonar: el swapchain y el hook de frame latency se
    // toman igual. De esa cadena cuelga el HUD entero -- fps y latencia -- y la
    // primera version de esta guarda hacia return aca y lo dejaba en cero.
    // Perder el contador propio es aceptable; perder el HUD no lo es.
    // En modo host el swapchain que nos interesa es el PROXY de Streamline: su
    // Present vive en sl.interposer.dll, no en dxgi.dll, asi que el byte-detour
    // del overlay no lo alcanza y el slot se puede escribir. El chain REAL que
    // el proxy envuelve sigue siendo de dxgi.dll y ahi valen las dos guardas de
    // siempre: medido en Metro 2026-09-11, exceptuar los dos dio RECURSION.
    HMODULE slot_owner = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)vt[8], &slot_owner);
    const bool host_proxy = g_host_on && slot_owner != nullptr &&
                            slot_owner == GetModuleHandleW(L"sl.interposer.dll");
    bool no_hook = false;
    {
        static bool said = false;
        if (GetModuleHandleW(L"gameoverlayrenderer64.dll") != nullptr && !host_proxy) {
            no_hook = true;
            if (!said) {
                said = true;
                log_line("present: overlay de Steam presente; NO se escribe el slot");
                log_line("  hace byte-detour de la funcion, asi que llamar al");
                log_line("  original nos devuelve a nosotros: recursion sin fin.");
                log_line("  Las presentaciones pasan a contarse con PresentCount");
                log_line("  del runtime, que es el instrumento honesto igual.");
            }
        }
    }
    // Solo se engancha un slot que TODAVIA apunta a dxgi.dll.
    //
    // Si ahi ya hay otro hook, apilarnos encima forma un lazo: nuestro hook
    // llama a lo que guardamos como original -- el hook del otro -- y ese llama
    // a lo que guardo EL como original, que es el slot, que ahora somos
    // nosotros. Cada present se llama a si mismo hasta agotar la pila.
    //
    // Medido en Cyberpunk lanzado desde Steam, 2026-09-11:
    //
    //   present: RECURSION, profundidad 4244
    //   EXCEPCION 0xC00000FD en gameoverlayrenderer64.dll
    //
    // El overlay de Steam engancha el mismo slot. Con el exe lanzado directo no
    // se inyecta, y el mismo juego corria 170 s sin una excepcion. Esa
    // diferencia estuvo a la vista horas y la descarte porque el log no nombraba
    // al overlay; lo nombro recien cuando el freno de recursion dejo la
    // profundidad escrita.
    //
    // [[never-byte-detour-present]] ya decia que el overlay de Steam engancha
    // esto mismo. Lo que faltaba era la guarda en ESTE camino.
    //
    // Perder el hook cuesta instrumentacion, no funcionalidad: las
    // presentaciones se cuentan con PresentCount del runtime, que es el
    // instrumento honesto de todas formas ([[measure-with-the-runtime-counter]]).
    //
    // Y "no apilarse" es NO escribir el slot, igual que con el overlay: la
    // instancia se adopta igual (g_swapchain, frame latency), porque de eso
    // cuelga PresentCount y el HUD. La primera version hacia return aca y
    // en GTA V con ReShade.asi en el slot el contador del HUD quedaba vacio
    // (2026-09-11: ventanas sin "runtime PresentCount", el usuario lo vio
    // como "el contador no andaba"). El mismo defecto que ya se habia
    // arreglado en la rama del overlay, en la otra rama.
    {
        HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        HMODULE owner = slot_owner;
        if (owner != nullptr && dxgi != nullptr && owner != dxgi && !host_proxy &&
            vt[8] != (void *)&hk_dxgi_present) {
            no_hook = true;
            static bool said = false;
            if (!said) {
                said = true;
                // El nombre del modulo, en ASCII, sin depender de log_ruta
                // que se define mas abajo en el archivo.
                wchar_t name_w[MAX_PATH] = { 0 };
                GetModuleFileNameW(owner, name_w, MAX_PATH);
                char short_name[96];
                int c = 0, start = 0;
                for (int i = 0; name_w[i] != 0; ++i)
                    if (name_w[i] == 92 || name_w[i] == 47) start = i + 1;
                for (int i = start; name_w[i] != 0 && c < 94; ++i)
                    short_name[c++] = (char)(name_w[i] < 128 ? name_w[i] : '?');
                short_name[c] = 0;
                log_line("present: el slot YA lo engancho otro; no nos apilamos");
                log_line(short_name);
                log_line("  (apilarse forma un lazo entre los dos hooks:");
                log_line("   medido en Cyberpunk desde Steam, profundidad 4244)");
                log_line("  la instancia se adopta igual: PresentCount del runtime y HUD");
            }
        }
    }
    DWORD prot = 0;
    if (!VirtualProtect(&vt[8], sizeof(void *), PAGE_READWRITE, &prot)) return;
    // El original de ESTA vtable, registrado antes de escribir el slot. El
    // hook lo busca por la vtable del objeto que recibe (present_original_de).
    const PFN_DXGIPresent orig_of_this = reinterpret_cast<PFN_DXGIPresent>(vt[8]);
    g_vt_present[g_vt_present_n].vt = vt;
    g_vt_present[g_vt_present_n].orig = orig_of_this;
    InterlockedIncrement(&g_vt_present_n);
    if (g_orig_dxgi_present == nullptr) g_orig_dxgi_present = orig_of_this;
    adopt_swapchain(sc);
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
    if (!no_hook) {
        vt[8] = reinterpret_cast<void *>(&hk_dxgi_present);
        log_line("recorder: present slot swapped (vt[8], not detoured)");
    }
    VirtualProtect(&vt[8], sizeof(void *), prot, &prot);
    g_present_via_runtime = no_hook;
    log_num("  vtables enganchadas ", (unsigned)g_vt_present_n);
}

// Adoptar el swapchain que el juego YA tiene.
//
// El hook de la factory solo puede ver swapchains creados DESPUES de armarse.
// En Halo sl.interposer aparece recien a los 11,7 s, con el juego andando: su
// swapchain ya existe, CreateSwapChain nunca vuelve a pasar por nosotros, y el
// contador de presentaciones se queda en cero para siempre. Desarmar la bandera
// de arriba no alcanza para ese caso.
//
// La salida ya estaba medida y no hubo que inventarla: M2 (poc/m2-vtable). La
// vtable de IDXGISwapChain es COMPARTIDA por todos los swapchains del proceso,
// asi que se crea uno propio y descartable, se le parchea el slot 8, y el del
// juego queda parcheado tambien. M2 conto 300 de 300 presentaciones de un
// swapchain ajeno por esta via, con la vtable en la misma direccion para los dos.
//
// No se detoura Present: se cambia el puntero del slot, que es la regla que
// este proyecto ya pago aprender con el overlay de Steam.
typedef HRESULT(WINAPI *PFN_D3D11CDSC)(void *, int, HMODULE, UINT, const int *, UINT,
        UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, void **, int *, void **);

static bool g_adopt_done = false;
static void adopt_existing_swapchain(void) {
    if (g_adopt_done || g_orig_dxgi_present != nullptr) return;
    // Primero se le da su chance al camino normal: si el juego crea su swapchain
    // despues que nosotros, el hook de la factory lo agarra y esto no hace falta.
    static unsigned long long seen = 0;
    if (seen == 0) { seen = GetTickCount64(); return; }
    if (GetTickCount64() - seen < 5000ULL) return;
    g_adopt_done = true;

    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (d3d11 == nullptr) { log_line("adopcion: no hay d3d11.dll"); return; }
    PFN_D3D11CDSC create = reinterpret_cast<PFN_D3D11CDSC>(
            GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    if (create == nullptr) { log_line("adopcion: no hay D3D11CreateDeviceAndSwapChain"); return; }

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"mfg_adopt";
    RegisterClassExW(&wc);
    HWND hw = CreateWindowExW(0, L"mfg_adopt", L"", WS_POPUP, 0, 0, 8, 8,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (hw == nullptr) { log_line("adopcion: no se pudo crear la ventana oculta"); return; }

    DXGI_SWAP_CHAIN_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 8;
    sd.BufferDesc.Height = 8;
    sd.BufferDesc.Format = static_cast<DXGI_FORMAT>(28);   // R8G8B8A8_UNORM
    sd.BufferUsage = 0x20u;                                // RENDER_TARGET_OUTPUT
    sd.OutputWindow = hw;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = static_cast<DXGI_SWAP_EFFECT>(0);      // DISCARD

    IDXGISwapChain *sc = nullptr;
    void *dev = nullptr;
    void *ctx = nullptr;
    const HRESULT hr = create(nullptr, 1 /* HARDWARE */, nullptr, 0, nullptr, 0,
                             7 /* D3D11_SDK_VERSION */, &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr) || sc == nullptr) {
        log_num("adopcion: D3D11CreateDeviceAndSwapChain fallo, hr ", (unsigned)hr);
        DestroyWindow(hw);
        return;
    }

    hook_swapchain_present(sc);
    const bool ok = (g_orig_dxgi_present != nullptr);
    // Lo que hook_swapchain_present logueo recien (flags, buffers, waitable) es
    // del descartable, NO del swapchain del juego. Se aclara para que nadie lea
    // esos numeros como si fueran del juego.
    log_line("  (los flags de arriba son del swapchain descartable, no del juego)");
    // El descartable ya cumplio. La vtable vive en el modulo de DXGI, no en el
    // objeto, asi que el parche sobrevive a soltarlo.
    forget_swapchain(sc);
    g_swapchain = nullptr;
    sc->Release();
    if (ctx != nullptr) reinterpret_cast<IUnknown *>(ctx)->Release();
    if (dev != nullptr) reinterpret_cast<IUnknown *>(dev)->Release();
    DestroyWindow(hw);
    log_line(ok ? "adopcion: vtable compartida parcheada desde un swapchain propio"
                : "adopcion: no se pudo parchear el slot");
}

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
    host_set_device(dev);
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
    host_set_device(dev);
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

// La fabrica PROXY de Streamline (modo host) se engancha aparte: hook_factory
// ya tomo la fabrica real -- la adopcion del swapchain descartable la crea
// antes que el juego -- y con eso hk_cscfh solo ve el chain nativo que el
// proxy crea por dentro. Medido en Metro 2026-09-11: "vtable ya enganchada"
// sobre la vtable de dxgi, cero llamadas a nuestro hook, y sin ellas no salen
// los marcadores de present. Aca el *out es el proxy y su vtable vive en
// sl.interposer.dll, que es el unico slot de Present que se puede escribir
// con el overlay de Steam en el proceso.
static PFN_CSC g_orig_csc_host = nullptr;
static PFN_CSCFH g_orig_cscfh_host = nullptr;
static HRESULT STDMETHODCALLTYPE hk_csc_host(IDXGIFactory *self, IUnknown *dev,
        DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **out) {
    host_set_device(dev);
    const HRESULT hr = g_orig_csc_host(self, dev, desc, out);
    log_num("host: CreateSwapChain por la fabrica proxy -> ", (unsigned)hr);
    if (SUCCEEDED(hr) && out != nullptr) hook_swapchain_present(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_cscfh_host(void *self, IUnknown *dev, HWND hwnd,
        const void *d1, const void *fs, void *restrict_to, IDXGISwapChain **out) {
    host_set_device(dev);
    const HRESULT hr = g_orig_cscfh_host(self, dev, hwnd, d1, fs, restrict_to, out);
    log_num("host: CreateSwapChainForHwnd por la fabrica proxy -> ", (unsigned)hr);
    if (SUCCEEDED(hr) && out != nullptr) hook_swapchain_present(*out);
    return hr;
}
static void hook_factory_host(void *factory) {
    if (factory == nullptr || g_orig_cscfh_host != nullptr) return;
    void **vt = *reinterpret_cast<void ***>(factory);
    if (MH_CreateHook(vt[15], reinterpret_cast<void *>(&hk_cscfh_host),
                      reinterpret_cast<void **>(&g_orig_cscfh_host)) != MH_OK ||
        MH_EnableHook(vt[15]) != MH_OK)
        g_orig_cscfh_host = nullptr;
    if (MH_CreateHook(vt[10], reinterpret_cast<void *>(&hk_csc_host),
                      reinterpret_cast<void **>(&g_orig_csc_host)) != MH_OK ||
        MH_EnableHook(vt[10]) != MH_OK)
        g_orig_csc_host = nullptr;
    log_num("host: fabrica proxy enganchada (1 = si) ", (unsigned)(g_orig_cscfh_host ? 1 : 0));
}

typedef HRESULT(WINAPI *PFN_F1)(REFIID, void **);
typedef HRESULT(WINAPI *PFN_F2)(UINT, REFIID, void **);
static PFN_F1 g_orig_f0 = nullptr, g_orig_f1 = nullptr;
static PFN_F2 g_orig_f2 = nullptr;

// One detour per export: a shared one could not tell which entry point it was
// reached through, and would forward half its calls to the wrong original.
static HRESULT WINAPI hk_f0(REFIID riid, void **out) {
    if (host_take()) {
        const HRESULT hr0 = h_CreateDXGIFactory ? h_CreateDXGIFactory(riid, out) : g_orig_f0(riid, out);
        host_release();
        log_num("host: CreateDXGIFactory por el interposer -> ", (unsigned)hr0);
        if (SUCCEEDED(hr0) && out != nullptr) hook_factory_host(*out);
        return hr0;
    }
    HRESULT hr = g_orig_f0(riid, out);
    if (SUCCEEDED(hr) && out != nullptr) hook_factory(*out);
    return hr;
}
static HRESULT WINAPI hk_f1(REFIID riid, void **out) {
    if (host_take()) {
        const HRESULT hr0 = h_CreateDXGIFactory1 ? h_CreateDXGIFactory1(riid, out) : g_orig_f1(riid, out);
        host_release();
        log_num("host: CreateDXGIFactory1 por el interposer -> ", (unsigned)hr0);
        if (SUCCEEDED(hr0) && out != nullptr) hook_factory_host(*out);
        return hr0;
    }
    HRESULT hr = g_orig_f1(riid, out);
    if (SUCCEEDED(hr) && out != nullptr) hook_factory(*out);
    return hr;
}
static HRESULT WINAPI hk_f2(UINT flags, REFIID riid, void **out) {
    if (host_take()) {
        const HRESULT hr0 = h_CreateDXGIFactory2(flags, riid, out);
        host_release();
        log_num("host: CreateDXGIFactory2 por el interposer -> ", (unsigned)hr0);
        if (SUCCEEDED(hr0) && out != nullptr) hook_factory_host(*out);
        return hr0;
    }
    HRESULT hr = g_orig_f2(flags, riid, out);
    if (SUCCEEDED(hr) && out != nullptr) hook_factory(*out);
    return hr;
}

static bool g_dxgi_armed = false;


static void arm_dxgi_recorder() {
    // Esto estuvo detras de mfg-debug.txt y no debia estarlo.
    //
    // De esta cadena cuelgan el fps Y la latencia del HUD: la factory arma el
    // hook del swapchain, el hook cuenta presentaciones, y el bloque que
    // escribe g_hud_fps_x10 y g_hud_lat_us solo corre si ese contador avanza.
    // Con la bandera apagada el HUD muestra 0 FPS y "-- ms" para siempre.
    //
    // Se vio en Halo, donde no habia mfg-debug.txt, y se confirmo por control:
    // GTA V y Cyberpunk tienen el archivo y el HUD anda; Halo no lo tenia y no
    // andaba. Pero el caso que importa no es Halo: quien use esto va a tener el
    // dll y nada mas, asi que NINGUN usuario veia fps ni latencia. El archivo de
    // diagnostico tapaba el agujero en las dos maquinas donde se probaba.
    //
    // g_debug sigue gobernando el logueo verboso, que si es diagnostico.
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
