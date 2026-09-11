// patches.h -- las escrituras en memoria sobre sl.dlss_g y nvngx_dlssg:
// donde se busca (sites.h) y que bytes se ponen.
//
// ENTRA: la base de un modulo recien mapeado (llamados desde loader.h /
//   on_dll_load), la configuracion (g_wic_mode, g_six, g_mfcmax, g_cubins,
//   g_meter_off, g_preset_b, ...) y el texto de cubins.h.
// SALE: los sitios parcheados que writer.h escribe por frame (g_imm_*,
//   g_wic_sites, g_gen_flag, g_lat_allow, g_pace_count, g_six_sites), los
//   conteos que on_dll_load loguea ("gates rewritten", "tope ... sitios",
//   "CPU pacer enabled"), g_wic_ok / g_wic_sites_last, y image_has para
//   reconocer builds por su contenido.
// DEPENDE DE: sites.h para los patrones, VirtualProtect/VirtualAlloc,
//   log_line/log_num, cubins.h, y los globales compartidos que todavia viven
//   en proxy.cpp.
//
// Movido de proxy.cpp en cinco rangos (2026-09-11), en su orden original,
// con el #include en la posicion del PRIMERO (writer.h, incluido despues,
// escribe sobre estos sitios). Sin tocar una linea del cuerpo.
// tools/test_sites.cpp verifica los patrones sobre los dll reales; lo que
// este archivo agrega -- la escritura -- se verifica con la corrida.
#pragma once

// arm_multiplier_override llama a esto; vive en proxy.cpp, mas abajo.
static void arm_frametoken_hook(void);

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
static const int kMaxSites = 4;
static void site_add(volatile unsigned char **list, int *n, volatile unsigned char *q) {
    for (int i = 0; i < *n; ++i) if (list[i] == q) return;
    if (*n < kMaxSites) list[(*n)++] = q;
}
static void site_write(volatile unsigned char **list, int n, unsigned char v) {
    for (int i = 0; i < n; ++i) if (list[i] != nullptr) *list[i] = v;
}
static volatile unsigned char *g_wic_sites[kMaxSites] = { nullptr, nullptr, nullptr, nullptr };
static int g_wic_n = 0;

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
static void wic_patch(bool put) {
    if ((g_wic_set != 0) == put) return;
    for (int i = 0; i < g_wic_n; ++i) {
        volatile unsigned char *imm = g_wic_sites[i];
        if (imm == nullptr) continue;
        unsigned char *q = (unsigned char *)(imm - 1);   // el opcode
        DWORD old = 0;
        if (!VirtualProtect(q, 3, PAGE_EXECUTE_READWRITE, &old)) continue;
        if (put) {
            q[2] = 0x58;                 // pop rax
            // La semilla es lo que el plugin TIENE, no 1.
            //
            // Al reponer el parche el plugin viene corriendo con su propia
            // cuenta -- la del modo entero que se acaba de dejar. Sembrar 1 deja
            // el bound sin relacion con la reserva viva, y en Halo eso crasheo
            // al pasar de 6X a DYNAMIC: +0x3ED6F, lectura de [nulo+0x40], la
            // ranura sin inicializar de siempre.
            //
            // g_last_seen_generated es la cuenta que el juego/plugin declaro
            // por la API, que es la que dimensiono la reserva.
            {
                const LONG suya = g_last_seen_generated;
                q[1] = (unsigned char)((suya >= 1 && suya <= 5) ? suya : 1);
            }
            q[0] = 0x6A;                 // push imm8   <- opcode al final
        } else {
            q[2] = 0x04;
            q[1] = 0x42;
            q[0] = 0x8B;                 // mov eax,[rdx+4]
        }
        VirtualProtect(q, 3, old, &old);
    }
    g_wic_set = put ? 1 : 0;
    log_line(put ? "wic: parche PUESTO (modo fraccional)"
                   : "wic: parche SACADO (modo entero, manda la API)");
}

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
    // La busqueda esta en src/sitios.h y se corre sobre el archivo real en
    // tools/test_sitios.cpp; aca solo se escribe.
    size_t at = 0;
    const int found = sites::find(text, len, sites::kWorkItemCount, &at, 1);
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
    site_add(g_wic_sites, &g_wic_n, q + 1);
    log_line("  work item count is ours (bound, index and metering agree)");

    // El SEGUNDO campo, doce bytes mas adelante. Sin este, todo lo demas falla.
    //
    // El constructor copia dos contadores seguidos:
    //
    //     0x47333  mov  eax, [rdx + 4]     <- lo forzabamos
    //     0x47336  mov  r8d, 0xc0
    //     0x4733c  mov  [rcx + 4], eax
    //     0x4733f  mov  eax, [rdx + 8]     <- lo dejabamos pasar
    //     0x47342  mov  [rcx + 8], eax
    //
    // Y cada campo acota un bucle distinto:
    //
    //     generacion  0x3ddfa  cmp [r13+4], ebx      <- nuestro numero
    //     llenado     0x45984  mov r14d, [rdx+8]     <- el del plugin
    //                 0x45b87  sub r12, 0xc0         (hacia atras hasta 0)
    //
    // Forzando solo +4, la generacion recorre hasta [ctx+4]-1 mientras el
    // llenado solo cubrio [ctx+8]-1. La ultima entrada queda sin puntero y el
    // functor de esa iteracion lo desreferencia: [nulo+0x40] en +0x3ED6F.
    //
    // Diez autopsias, banco y Halo, siempre lo mismo:
    //
    //     [ctx+4]  [ctx+8]  indice pedido  ranuras llenas
    //        5        4           4            0..3
    //        4        3           3            0..2
    //
    // Por eso ningun valor del byte servia: mover +4 mueve la generacion y no
    // el llenado, asi que el hueco se corre pero no se cierra. Y por eso NVIDIA
    // no chequea nulo ahi -- en operacion normal los dos campos los escribe el
    // mismo codigo y no pueden discrepar.
    //
    // Los dos sitios quedan en la misma lista, asi que set_count_now les
    // escribe el mismo numero y los dos bucles recorren el mismo rango.
    {
        unsigned char *q2 = q + 12;
        if (sites::matches(q2, sites::kFillCount)) {
            DWORD o2 = 0;
            if (VirtualProtect(q2, 3, PAGE_EXECUTE_READWRITE, &o2)) {
                q2[0] = 0x6A;
                q2[1] = 1;
                q2[2] = 0x58;
                site_add(g_wic_sites, &g_wic_n, q2 + 1);
                log_line("  fill count is ours too (el llenado sigue al bound)");
            }
        } else {
            // Sin este el parche esta a medias y el crash vuelve. Se dice.
            log_line("  ! fill count site NOT found: el llenado sigue siendo del plugin");
        }
    }
    return 1;
}

static volatile unsigned char *g_imm_sites[kMaxSites] = { nullptr, nullptr, nullptr, nullptr };
static int g_imm_n = 0;
static volatile unsigned char *g_imm2_sites[kMaxSites] = { nullptr, nullptr, nullptr, nullptr };
static int g_imm2_n = 0;
static volatile unsigned char *g_imm3_sites[kMaxSites] = { nullptr, nullptr, nullptr, nullptr };
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
    size_t at = 0;
    const int found = sites::find(text, len, sites::kGenerationFlag, &at, 1);
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
    g_wic_mode = g_cfg.wic;
    if (g_wic_mode) {
        // The comparison patches are exactly what this replaces; leaving
        // them in would put the bound back out of step with the field.
        const int wic_sites_n = patch_work_item_count(text, len);
        g_wic_sites_last = wic_sites_n;
        g_saw_any_copy = true;
        // Refleja ESTA copia, no la peor que se haya visto nunca.
        //
        // Antes solo se apagaba. Una copia que no nos interesa -- el sl.dlss_g
        // 2.7 del propio Halo -- lo dejaba en falso para el resto de la corrida
        // aunque la copia que de verdad corre hubiera enganchado antes y volviera
        // a enganchar despues. El log de Halo lo muestra entero: sites 1, sites
        // 0, y despues sites 1 otra vez, con el freno activo hasta el final.
        //
        // Un estado que solo sabe empeorar no es un estado, es una cicatriz.
        g_wic_ok = wic_sites_n > 0;
        log_line(wic_sites_n > 0
                 ? "  PARCHE DE CUENTA: engancho en esta copia"
                 : "  PARCHE DE CUENTA: NO engancho en esta copia");
        if (wic_sites_n == 0) {
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
    site_add(g_imm_sites, &g_imm_n, p + 2);

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
        size_t meter_found = 0, meter_at = 0;
        for (size_t i = 0; i + 12 <= len; ++i) {
            if (text[i] != 0x44 || text[i+1] != 0x8B || text[i+2] != 0xCB) continue;
            if (text[i+3] != 0x4D || text[i+4] != 0x85 || text[i+5] != 0xED) continue;
            if (text[i+6] != 0x74) continue;
            if (text[i+8] != 0x45 || text[i+9] != 0x8B ||
                text[i+10] != 0x4D || text[i+11] != 0x00) continue;
            ++meter_found;
            meter_at = i;
        }
        if (meter_found == 1) {
            unsigned char *m = text + meter_at + 8;
            DWORD om = 0;
            if (VirtualProtect(m, 4, PAGE_EXECUTE_READWRITE, &om)) {
                m[0] = 0x6A;    // push imm8
                m[1] = 1;       // NN
                m[2] = 0x41;    // pop r9
                m[3] = 0x59;
                VirtualProtect(m, 4, om, &om);
                DWORD ig = 0;
                VirtualProtect(m + 1, 1, PAGE_EXECUTE_READWRITE, &ig);
                site_add(g_imm3_sites, &g_imm3_n, m + 1);
                log_line("  metering count made writable too");
            }
        } else {
            log_num("  ! metering count site not unique, sites: ", (unsigned)meter_found);
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
    if (g_cfg.monoidx) patch_monotonic_index(base, text, len);
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
        size_t valid_found = 0, valid_at = 0;
        for (size_t i = 0; i + 10 <= len; ++i) {
            if (text[i] != 0x44 || text[i + 1] != 0x8B ||
                text[i + 2] != 0x47 || text[i + 3] != 0x24) continue;
            if (text[i + 4] != 0x41 || text[i + 5] != 0x83 ||
                text[i + 6] != 0xF8 || text[i + 7] != 0x01) continue;
            if (text[i + 8] != 0x0F || text[i + 9] != 0x83) continue;
            ++valid_found;
            valid_at = i;
        }
        if (valid_found == 1) {
            unsigned char *v = text + valid_at + 7;
            DWORD ov = 0;
            if (VirtualProtect(v, 1, PAGE_EXECUTE_READWRITE, &ov)) {
                *v = 0;
                VirtualProtect(v, 1, ov, &ov);
                log_line("  zero generated frames accepted (ratios under 2x)");
            }
        } else {
            log_num("  ! zero-count validation not unique, sites: ", (unsigned)valid_found);
        }
    }

    size_t guard_found = 0, guard_at = 0;
    for (size_t i = 0; i + 10 <= len; ++i) {
        if (text[i] != 0x8B || text[i + 1] != 0xFB) continue;          // mov edi, ebx
        if (text[i + 2] != 0x41 || text[i + 3] != 0x39 ||
            text[i + 4] != 0x5D || text[i + 5] != 0x04) continue;      // cmp [r13+4], ebx
        if (text[i + 6] != 0x0F || text[i + 7] != 0x86) continue;      // jbe rel32
        ++guard_found;
        guard_at = i;
    }
    if (guard_found == 1) {
        unsigned char *q = text + guard_at + 2;
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
            site_add(g_imm2_sites, &g_imm2_n, q + 2);
        }
    } else {
        log_num("  ! loop entry guard not unique, sites: ", (unsigned)guard_found);
    }
    return 1;
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

// El tope que el snippet REPORTA, subido.
//
// Medido: con los dos cincos del plugin ya en 6, el campo seguia leyendo 5 y
// los bytes parcheados leian 6 en memoria. O sea que el 5 lo pone NGX, y el
// clamp min(NGX,6) lo deja pasar tal cual.
//
// El snippet lo decide por arquitectura, igual que la build de julio pero con
// otro numero:
//
//   0x16872  cmp  ebp, 0x1b0     ; 81 FD B0 01 00 00
//   0x16878  jl   ...            ; 0F 8C rel32   -> Ada: edi = 1
//   0x1687e  mov  edi, 5         ; BF 05 00 00 00           <- ESTE
//   0x1691f  mov  r8d, edi       ; -> DLSSG.MultiFrameCountMax
//
// patch_gates ya reescribe ese `cmp` a cero, y por eso Ada toma la rama de
// Blackwell y llega a 5. Subir el inmediato a 6 sube el techo reportado.
//
// El inmediato del `cmp` puede estar ya en cero cuando esto corre, asi que el
// patron no se ancla en 0x1b0: `81 FD` + imm32 + `0F 8C` + rel32 + `BF 05...`.
static int patch_snippet_max(unsigned char *base, int value) {
    if (value < 2 || value > 8) return 0;
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
    // La busqueda esta en src/sitios.h (kSnippetMaxA); aca se escribe el
    // valor sobre el 5.
    int hits = 0;
    {
        size_t at[8];
        const int n = sites::find(text, len, sites::kSnippetMaxEdi, at, 8);
        for (int k = 0; k < n && k < 8; ++k) {
            unsigned char *byte = text + at[k] + sites::kSnippetMaxEdi.write_at;
            DWORD old = 0;
            if (!VirtualProtect(byte, 1, PAGE_EXECUTE_READWRITE, &old)) continue;
            *byte = (unsigned char)value;
            VirtualProtect(byte, 1, old, &old);
            ++hits;
        }
    }
    // El TERCER techo, y el que realmente rechazaba el 6.
    //
    // Los dos de arriba (el que reporta el snippet y los del plugin) se
    // subieron y 6X seguia sin generar: el runtime lo dijo 4254 veces,
    //
    //   [EndpointCoreInputs::ComputeAndValidateTimeFactor:416]
    //   Error: input MultiFrameCount 6 is greater than the maximum
    //   supported count (5)
    //
    // El 5 de ese mensaje es %d, o sea una variable, y sale de un inmediato en
    // la funcion que valida los factores de tiempo:
    //
    //   0x618fa  mov  esi, 5          ; BE 05 00 00 00
    //   0x619d7  mov  r8d, [rbx+0x4f8] ; lo pedido
    //   0x619de  cmp  r8d, esi
    //   0x619e1  jbe  ok
    //
    // `BE 05 00 00 00` aparece UNA sola vez en todo .text, asi que el patron no
    // necesita anclas alrededor.
    //
    // Ojo: este no declara una capacidad, calcula donde cae cada sub-frame en
    // el tiempo. Subirlo puede dar interpolacion mal ubicada en vez de un
    // rechazo limpio. Se mide mirando la imagen, no solo el contador.
    // kSnippetMaxB en src/sitios.h; es unico, se escribe solo el primero.
    {
        size_t at = 0;
        if (sites::find(text, len, sites::kSnippetMaxEsi, &at, 1) >= 1) {
            unsigned char *byte = text + at + sites::kSnippetMaxEsi.write_at;
            DWORD old = 0;
            if (VirtualProtect(byte, 1, PAGE_EXECUTE_READWRITE, &old)) {
                *byte = (unsigned char)value;
                VirtualProtect(byte, 1, old, &old);
                ++hits;
            }
        }
    }
    return hits;
}

static int patch_multiframe_max(unsigned char *base) {
    if (g_mfcmax < 2 || g_mfcmax > 8) return 0;
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
    for (size_t i = 0; i + 16 <= len; ++i) {
        if (text[i] != 0x41 || text[i+1] != 0xB8) continue;        // mov r8d, imm32
        if (text[i+2] != 3 || text[i+3] || text[i+4] || text[i+5]) continue;
        if (text[i+6] != 0x81 || text[i+7] != 0xFF) continue;      // cmp edi, imm32
        if (text[i+12] != 0x44 || text[i+13] != 0x0F ||
            text[i+14] != 0x4C || text[i+15] != 0xC3) continue;    // cmovl r8d, ebx
        DWORD old = 0;
        if (!VirtualProtect(text + i + 2, 1, PAGE_EXECUTE_READWRITE, &old)) continue;
        text[i + 2] = (unsigned char)g_mfcmax;
        VirtualProtect(text + i + 2, 1, old, &old);
        ++hits;
    }
    return hits;
}

// Los dos cincos del plugin, que son lo unico que impide 6X.
//
// El array de sub-frames tiene SEIS ranuras -- una por frame real y cinco
// generados -- asi que 6X es el maximo estructural. Lo que topa antes son dos
// inmediatos en sl.dlss_g:
//
//   0x4fccd  mov   dword ptr [rdi+0x45e4], 5   ; el default del constructor
//   0x57cca  mov   edx, 5                      ; y el clamp
//   0x57cd1  cmovb edx, ecx                    ; [ctx+0x45e4] = min(NGX, 5)
//
// El 5 que se lee en runtime es ESE default, no una respuesta de NGX. Nuestro
// snippet solo acepta 1..4 para DLSSG.MultiFrameCountMax y fuera de ese rango
// no lo setea:
//
//   nvngx_dlssg 0x168e7  lea eax, [rbx-1]
//               0x168ea  cmp eax, 3
//               0x168ed  ja  ...               ; no lo setea
//
// Y aun sin que NGX lo fije, la evaluacion a 5 sub-frames corre sin un solo
// 0xbad00005. O sea que el maximo reportado es consultivo, no un limite duro:
// lo que manda es lo que el plugin se permite pedir.
//
// Se cambian los dos a la vez. Uno solo no alcanza: subir el clamp sin el
// default deja el default gobernando cuando NGX no contesta, que es justo el
// caso de esta build.
//
// Detras de mfg-seis.txt hasta que este medido. El intento anterior de subir un
// tope termino en 4346 fallos NGX y pantalla negra -- ese forzaba 5 sobre un
// binario que soportaba 3; este fuerza 6 sobre uno que ya entrega 5 limpio y
// con 6 ranuras reales detras. Es mejor apuesta, no una certeza.
// g_seis se declara arriba, junto a g_seis_sitios.

static int patch_cap_six(unsigned char *base) {
    if (!g_six) return 0;
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
    // Las dos formas estan en src/sitios.h (kTope6A, kTope6B); aca se escribe
    // el 6 sobre el 5 y se anota el sitio para "byte vivo".
    int hits = 0;
    const sites::Pattern *forms[2] = { &sites::kCap6Store, &sites::kCap6Cmov };
    for (int f = 0; f < 2; ++f) {
        size_t at[8];
        const int n = sites::find(text, len, *forms[f], at, 8);
        for (int k = 0; k < n && k < 8; ++k) {
            unsigned char *byte = text + at[k] + forms[f]->write_at;
            DWORD old = 0;
            if (VirtualProtect(byte, 1, PAGE_EXECUTE_READWRITE, &old)) {
                *byte = 6;
                VirtualProtect(byte, 1, old, &old);
                if (g_six_n < 4) g_six_sites[g_six_n++] = byte;
                ++hits;
            }
        }
    }
    return hits;
}

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

    // Las dos formas del cmp contra 0x1B0 estan en src/sitios.h; aca se
    // escribe el 0 sobre el inmediato.
    int hits = 0;
    const sites::Pattern *forms[2] = { &sites::kGateEax, &sites::kGateReg };
    for (int f = 0; f < 2; ++f) {
        size_t at[8];
        const int n = sites::find(text, len, *forms[f], at, 8);
        for (int k = 0; k < n && k < 8; ++k) {
            unsigned char *imm = text + at[k] + forms[f]->write_at;
            DWORD old = 0;
            if (!VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
            *reinterpret_cast<unsigned *>(imm) = 0;
            VirtualProtect(imm, 4, old, &old);
            ++hits;
        }
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
    {
        // Busqueda en src/sitios.h (pacer_cmovae); aca solo se escribe.
        size_t at[8];
        const int n = sites::pacer_cmovae(text, len, at, 8);
        for (int k = 0; k < n && k < 8; ++k) {
            unsigned char *cmov = text + at[k];         // the cmovae
            DWORD old = 0;
            if (!VirtualProtect(cmov, 3, PAGE_EXECUTE_READWRITE, &old)) continue;
            cmov[0] = 0x90; cmov[1] = 0x90; cmov[2] = 0x90;
            VirtualProtect(cmov, 3, old, &old);
            ++hits;
        }
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
    {
        // Busqueda en src/sitios.h (pacer_movbl); aca solo se escribe.
        size_t at[8];
        const int n = sites::pacer_movbl(text, len, at, 8);
        for (int k = 0; k < n && k < 8; ++k) {
            unsigned char *imm = text + at[k];
            DWORD old = 0;
            if (!VirtualProtect(imm, 2, PAGE_EXECUTE_READWRITE, &old)) continue;
            imm[0] = 0x90; imm[1] = 0x90;
            VirtualProtect(imm, 2, old, &old);
            ++hits;
        }
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
