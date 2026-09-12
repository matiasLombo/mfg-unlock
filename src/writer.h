// writer.h -- el camino de escritura al plugin: de la seleccion del panel a
// la llamada slDLSSGSetOptions y al byte del sub-frame.
//
// ENTRA: lo que el juego escribe en cada slDLSSGSetOptions (hk_slDLSSGSetOptions
//   la intercepta y captura la llamada), la seleccion del panel (g_force_sel,
//   g_dyn_target, g_dyn_fps), el ratio que decide el controlador
//   (controller.h via dyn_apply) y la cuenta por frame del reparto
//   (scheduler.h via fractional_tick), la base medida (measurement.h).
// SALE: la struct de opciones reescrita (force_into, con la decision de
//   policy.h), la llamada repetida al plugin con latch de 150 ms / 8
//   presentaciones (apply_override_now), el byte del bound de sub-frames y el
//   del pacer (set_count_now, sobre los sitios de patches), g_api_applied y
//   g_count_live para measurement.h.
// DEPENDE DE: policy.h, controller.h, scheduler.h, los sitios parcheados
//   (g_imm_*, g_wic_*), log_line/log_num, y los globales compartidos que
//   todavia viven en proxy.cpp.
//
// Movido de proxy.cpp en cuatro rangos (2026-09-11), en su orden original;
// el #include quedo en la posicion del ultimo (despues de measurement.h,
// del que depende) y hk_slDLSSGSetOptions tiene una declaracion adelantada
// donde estaba el primero. Sin tocar una linea del cuerpo.
#pragma once

// Globales que solo usa este modulo (movidas de proxy.cpp).
static volatile LONG g_cycle_ceiling = 0;   // lo+1 del ciclo en curso
static volatile LONG g_game_asked_on = 0;
// Lo que el JUEGO pide, que no es lo mismo que lo que pedimos nosotros.
//
// g_juego_quiere: su ultimo modo, 0 = eOff, 1 = eOn, -1 = todavia no lo vimos.
// g_juego_pidio_on: si alguna vez lo vimos pedir eOn en esta corrida.
static volatile LONG g_game_wants = -1;
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
static LONG g_refresh_hz = 0;
static LONG g_saved_mode = 0;

// Applies the current selection to a live options struct, returning what was
// there so the caller can put it back.
// Set when force_into wrote the frame-rate target, so the restore afterwards
// puts back exactly the fields that were touched and no others.
static bool g_target_written = false;
static float g_saved_target = 0.0f;

// Cuando el juego apaga la generacion, no se le pelea.
//
// Esto es el congelamiento del menu de pausa, y es nuestro. GTA V escribe eOff
// al pausar -- unas 170 veces por segundo, una por frame -- y nosotros
// reescribiamos eOn con nuestra cuenta en cada una, sin mirar nunca que habia
// pedido. Medido con el juego pausado, ventanas de 45 frames renderizados:
//
//   rend/s  presentadas/renderizada
//   1991.7                    0.14
//     55.4                    5.96
//   1009.2                    0.31
//     55.6                    5.98
//
// El bucle alterna entre 6X limpio y rafagas de mil a dos mil fps que no
// presentan nada. Eso en pantalla es la imagen congelada. Pasa en TODOS los
// modos, 2X incluido, que es lo que descarta los parches de techo y deja como
// unica causa comun el eOn forzado.
//
// Y explica los eDLSSGStatusFailReflexNotDetectedAtRuntime: con la generacion
// apagada el juego deja de emitir los marcadores de Reflex, asi que forzarla
// encendida ahi pide interpolar sin los datos que hacen falta.
//
// No es apagar nada nuestro. Durante el juego el multiplicador sigue entero; lo
// que se deja de hacer es forzar generacion donde el juego no la pidio.
//
// La guarda de que alguna vez lo hayamos visto pedir eOn importa: hay juegos que
// no llaman nunca con eOn -- en Halo no aparece una sola linea de "the game
// itself last asked for" en toda la corrida -- y ahi respetar el eOff seria no
// generar jamas. Sin esa evidencia se mantiene el comportamiento de antes.
static bool game_turned_off_generation(void) {
    return g_game_asked_on != 0 && g_game_wants == 0;
}

static void force_into(unsigned char *p, LONG *savedMode, LONG *savedCount) {
    *savedMode = *(LONG *)(p + 32);
    *savedCount = *(LONG *)(p + 36);
    // La decision vive en src/politica.h, sin globales, y se pincha en
    // tools/test_politica.cpp. Aca queda lo que no es decision: leer los
    // globales, escribir la struct y loguear con el mismo dedupe de siempre.
    // Es el unico punto por el que pasa toda escritura de opciones.
    policy::ForceInput e{};
    e.passive = phase_passive();
    e.game_asked_on = g_game_asked_on != 0;
    e.game_wants = g_game_wants;
    e.sel = g_force_sel;
    e.force_generated = g_force_generated;
    e.multiplier = count_is_multiplier();
    e.cycle_ceiling = g_cycle_ceiling;
    e.six = g_six;
    e.ceilfirst = g_ceilfirst;
    e.interp_on = g_interp_on != 0;
    e.wic_ok = g_wic_ok;
    e.last_seen_generated = g_last_seen_generated;
    const LONG kTope = count_cap();
    e.cap = kTope;
    const policy::ForceOutput s = policy::decide_force(e);

    switch (s.reason) {
    case policy::Reason::PASSIVE: {
        // Un juego con topologia rota corre como si el mod no estuviera, en vez
        // de crashear. Ver evaluar_invariantes.
        static bool said = false;
        if (!said) {
            said = true;
            log_line("PASIVO: no se reescriben las opciones del juego");
        }
        return;
    }
    case policy::Reason::GAME_TURNED_OFF: {
        // El congelamiento del menu de pausa: ver juego_apago_la_generacion.
        static LONG said = -1;
        if (said != g_force_sel) {
            said = g_force_sel;
            log_num("override: el juego apago la generacion, no se le pelea; seleccion ",
                    (unsigned)g_force_sel);
        }
        return;                                // se deja tal cual la dejo el juego
    }
    default:
        break;
    }
    g_target_written = false;

    if (s.a1_declared) {
        static bool said = false;
        if (!said) {
            said = true;
            log_num("A1: declarando el techo del ciclo mientras apagado ",
                    (unsigned)s.a1_ceiling);
        }
    }
    if (s.reason == policy::Reason::BRAKE_NO_DATA) {
        // Nunca vimos que cuenta pide el juego. Sin ese dato no hay a que
        // espejarse: ni la cuenta ni el modo se tocan. Poner un valor de reserva
        // seria forzar a ciegas, que es lo que hizo crashear a Halo.
        static bool said_none = false;
        if (!said_none) {
            said_none = true;
            diag::Line l = diag::invariant(diag::Layer::POLICY, "cuenta-del-juego",
                                             "sin parche y sin saber que pide el juego: no se toca");
            l.pair("visto", g_last_seen_generated).pair("sel", g_force_sel);
            log_line(l.b);
        }
        return;
    }
    if (s.brake_limited) {
        static LONG said = -1;
        if (said != s.brake_cap) {
            said = s.brake_cap;
            log_num("freno: sin parche, la cuenta se limita a la del juego ",
                    (unsigned)s.brake_cap);
        }
    }
    if (s.reason == policy::Reason::DYN_ON) {
        // Lo que realmente se escribe, y contra que se recorta.
        static LONG said_e = -1, said_t = -1, said_g = -1;
        if (said_e != s.to_write || said_t != kTope || said_g != g_force_generated) {
            said_e = s.to_write; said_t = kTope; said_g = g_force_generated;
            log_num("force_into: g_force_generated ", (unsigned)g_force_generated);
            log_num("  escribir ", (unsigned)s.to_write);
            log_num("  kTope ", (unsigned)kTope);
            log_num("  g_max_declarado ", (unsigned)g_max_declared);
            log_num("  queda ", (unsigned)s.remains);
        }
    }
    if (s.invariant_broken) {
        // Con la semantica de multiplicador una cuenta menor a 2 es 1X. Se
        // corrige solo cuando manda el parche; con el freno se deja constancia
        // (corregirlo ahi congelo Halo 24 veces). Si esto aparece seguido, el
        // culpable es otro y hay que ir a buscarlo.
        static LONG said = -1;
        if (said != s.remains) {
            said = s.remains;
            diag::Line l = diag::invariant(diag::Layer::POLICY, "cuenta>=2",
                                             s.invariant_fixed
                                                 ? "con multiplicador es 1X; se escribe 2, el piso"
                                                 : "con multiplicador es 1X; manda el freno, NO se corrige");
            l.pair("cuenta", s.remains).pair("sel", g_force_sel).pair("gen", g_force_generated)
             .pair("objetivo", g_dyn_target).pair("tope", kTope);
            log_line(l.b);
        }
    }
    if (s.write_mode) *(LONG *)(p + 32) = (LONG)s.mode;
    if (s.write_count) *(LONG *)(p + 36) = (LONG)s.count;
}

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
        if (raw_mode == 0 || raw_mode == 1) {
            if (raw_mode == 1) g_game_asked_on = 1;
            if (g_game_wants != raw_mode) {
                g_game_wants = raw_mode;
                log_num("el juego pide generacion (0 no, 1 si) ", (unsigned)raw_mode);
            }
        }
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
    read_max_generated();      // antes de la llamada
    const unsigned r = g_orig_setoptions(viewport, options_for_call(options));
    read_max_generated();      // y despues: si la llamada lo cambia, se ve
    if (p != nullptr && memcmp(p + 8, kDlssgOptionsGuid, 16) == 0) {
        g_api_applied = *(LONG *)(p + 36);
        set_count_now(g_api_applied);
    }
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
    // Nunca dos cambios de cuenta mas juntos que el enfriamiento del plugin.
    //
    // Cada slDLSSGSetOptions que cambia la cuenta hace que el plugin libere sus
    // recursos y rearme, con 100 ms en los que no interpola (0x1800497fd escribe
    // 100.0 en [ctx+0x4488]). Ese numero estaba documentado en este archivo hace
    // rato y NO se hacia cumplir en ningun lado.
    //
    // Medido en Cyberpunk, DYNAMIC, los 94 ms antes de un 0xC0000005:
    //
    //   [78735ms] queda 4   override applied now
    //   [78750ms] queda 2   override applied now     <- 15 ms despues
    //   [78829ms] queda 4   override applied now     <- 79 ms despues
    //   [78860ms] EXCEPCION  sl.dlss_g, lectura en 0x64
    //
    // Tres cambios en 94 ms: el plugin estaba reconstruyendo su reserva con
    // frames en vuelo y leyo un puntero que el desarme anterior habia dejado en
    // cero. El crash es nuestro, no del set mezclado ni del overlay.
    //
    // 150 ms y no 100: el enfriamiento arranca cuando el plugin libera, no
    // cuando nosotros llamamos, asi que mandar justo en el borde es una carrera.
    // Y no cuesta nada -- los bloques de la cadencia son de ~800 ms.
    //
    // No se pierde el cambio: g_opt_pending queda puesto y sale en el Present
    // siguiente que pase el filtro.
    // Tres condiciones, no una. El tiempo solo no alcanza.
    //
    // (1) DWELL. 150 ms desde el ultimo envio: el enfriamiento del plugin son
    //     100 ms y arranca cuando el libera, no cuando nosotros llamamos, asi
    //     que mandar justo en el borde es una carrera.
    //
    // (2) PRESENTACIONES. El tiempo puede pasar sin que el juego presente --
    //     una carga, una pausa, un hitch -- y ahi 150 ms no significan que el
    //     plugin haya tenido frames para reconstruir. Ocho presentaciones es el
    //     equivalente en trabajo, no en reloj.
    //
    // (3) CONFIRMACION. Lo mas importante y lo que faltaba: no se manda un
    //     cambio nuevo si el ANTERIOR todavia no se observo efectivo.
    //     g_api_aplicada se lee de vuelta del struct despues de cada llamada,
    //     asi que comparar contra lo ultimo enviado dice si el plugin ya lo
    //     tomo. Sin esto se pueden encolar cambios sobre un plugin que sigue
    //     rearmando, que es exactamente la secuencia 4 -> 2 -> 4 en 94 ms que
    //     termino en 0xC0000005 leyendo [nulo+0x64].
    //
    // Lo que no se cumple no se pierde: g_opt_pending queda puesto y el envio
    // sale en el Present siguiente que pase las tres.
    {
        static LARGE_INTEGER freq = { };
        static LONGLONG last = 0;
        static LONG last_pres_count = 0;
        static LONG last_sent = -1;
        if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER now_qpc;
        QueryPerformanceCounter(&now_qpc);
        if (last != 0 && freq.QuadPart > 0) {
            const double ms = (double)(now_qpc.QuadPart - last) * 1000.0 /
                              (double)freq.QuadPart;
            if (ms < 150.0) return;                                  // (1)
            if (g_present_count - last_pres_count < 8) {                 // (2)
                static LONG said_p = -1;
                if (said_p != g_force_generated) {
                    said_p = g_force_generated;
                    log_num("override: sin 8 presentaciones desde el ultimo envio; se espera. presentes ", (unsigned)(g_present_count - last_pres_count));
                }
                return;
            }
            if (last_sent >= 0 && g_api_applied != last_sent) {
                static LONG said = -1;                              // (3)
                if (said != last_sent) {
                    said = last_sent;
                    log_num("latch: el cambio anterior aun no se observo; se espera. pedido ",
                            (unsigned)last_sent);
                    log_num("  aplicado en la API ", (unsigned)g_api_applied);
                }
                return;
            }
        }
        last = now_qpc.QuadPart;
        last_pres_count = g_present_count;
        // Un apagado (seleccion 1) escribe solo el modo, no la cuenta, asi que
        // no hay cuenta nueva que esperar ver en la API: con last_sent = 0 el
        // latch comparaba contra la cuenta vieja y esperaba para siempre.
        // Medido en Metro (modo host, 2026-09-11): despues de mode 1, ningun
        // cambio del panel volvio a salir -- "pedido 0 / aplicado en la API
        // 4" -- porque ahi nadie mas llama a slDLSSGSetOptions y el juego no
        // lo destraba como en los otros tres.
        last_sent = g_force_sel == policy::kSelOff ? -1 : g_force_generated;
    }
    // Lo mismo que en force_into: si el juego la apago, el reenvio la volveria a
    // encender por la puerta de atras.
    // Says which precondition is missing instead of returning quietly. Five
    // can fail and they need different answers: the game turned generation
    // off, the game spoke this frame, no captured call to replay, no wrapper
    // installed, or the wrong thread.
    static int said = 0;
    if (game_turned_off_generation()) {
        if (said != 4) { said = 4; log_line("override: el juego apago la generacion; no se pisa"); }
        return;
    }
    // The game already spoke for this frame; ours would be the repeated call.
    if (g_game_set_this_frame != 0) {
        if (said != 5) { said = 5; log_line("override: el juego escribio opciones en este frame; se espera al siguiente"); }
        return;
    }
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
    // Bajando la cuenta, el byte va PRIMERO; subiendola, despues.
    //
    // La tabla medida no tiene transitorio seguro: el byte por encima de la
    // reserva escribe en una ranura que nadie hizo y crashea, por debajo detiene
    // la presentacion. Cuando la cuenta no se movia daba igual el orden. Ahora
    // se mueve dos veces por ciclo, y en el instante entre la llamada y la
    // escritura del byte los dos numeros no coinciden.
    //
    // De los dos desajustes el peligroso es el primero, y solo aparece al BAJAR:
    // la reserva se achica mientras el byte todavia pide lo de antes. Bajando el
    // byte antes de la llamada, nunca hay un momento con el byte por encima. Al
    // subir el orden correcto es el contrario, y ya es el que hay: la linea de
    // abajo lo sube recien cuando el plugin reservo.
    if (count_is_multiplier()) {
        const LONG ap = g_api_applied, wants = g_force_generated;
        if (ap >= 1 && wants >= 1 && wants < ap) set_count_now(wants);
    }
    g_orig_setoptions(g_vp_copy, options_for_call(g_opt_copy));
    // El byte del bound se escribe ACA, pegado a la llamada.
    //
    // Las dos filas de la tabla medida son fatales: bound mayor que la reserva
    // escribe fuera y crashea; bound menor detiene la presentacion. No hay
    // transitorio seguro, asi que no alcanza con recortar el byte -- se probo y
    // Halo se congelo al subir a 6X: las ventanas de medicion se cortaron 1 s
    // despues del cambio y no volvieron.
    //
    // Escribirlo inmediatamente despues de que las opciones salieron deja a las
    // dos en el mismo instante desde el punto de vista del plugin.
    g_api_applied = *(LONG *)(g_opt_copy + 36);
    set_count_now(g_api_applied);
    // Recien ahora el plugin reservo esta cuenta: el byte ya puede subir.
    g_api_applied = *(LONG *)(g_opt_copy + 36);
    log_num("override applied now, selection ", (unsigned)g_force_sel);
}

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
    // La mitad del byte del tope de 6X. Ver tope_cuenta.
    {
        const LONG t = count_cap();
        if (n > t) n = t;
    }
    // El byte parcheado es el limite del bucle; la cuenta de la API dimensiona
    // la reserva. Si el byte supera a lo que la API pidio, el bucle corre mas
    // iteraciones que la memoria que hay, y eso es un acceso invalido.
    //
    // El freno limitaba SOLO la escritura de la API y dejaba el byte suelto.
    // En Halo eso quedo a la vista: "freno: la cuenta se limita a la del juego
    // 1" y en la misma corrida "slowalt: API count now 3", con el byte
    // siguiendo al 3. Crasheo a los 36 s, otra vez dentro de 190_E658703.dll.
    //
    // Las dos mitades tienen que frenarse juntas o ninguna.
    if (!g_wic_ok) {
        const LONG theirs = g_last_seen_generated;
        const LONG cap = (theirs >= 1 && theirs <= 5) ? theirs : 0;
        if (n > cap) {
            static LONG said = -1;
            if (said != cap) {
                said = cap;
                log_num("freno: el byte de la cuenta tambien se limita a ",
                        (unsigned)cap);
            }
            n = cap;
        }
    }
    if (g_wic_mode) {
        // n is generated frames, which is what the field holds: the multiplier
        // is n + 1.
        if (g_wic_n > 0) {
            if (n < 0) n = 0;
            { const LONG t6 = (g_six || count_is_multiplier()) ? 6 : 5; if (n > t6) n = t6; }
            // Solo si el parche esta PUESTO. Con el parche sacado esa
            // direccion ya no es un inmediato: es el byte 0x42 de
            // mov eax,[rdx+4], y escribirle corrompe la instruccion del plugin.
            // Medido: con el parche sacado por modo, cero ventanas de medicion.
            if (g_wic_set != 0) {
                // Ultima linea de defensa: el byte nunca por encima de la
                // reserva viva. Sin esperas ni frenos -- si la reserva es 3, se
                // escribe 3. Se pierde multiplicador en ese frame; no se pierde
                // el juego. Todo crash de esta noche fue el byte pidiendo mas
                // ranuras de las reservadas.
                LONG w = n;
                const LONG ap = g_api_applied;
                if (ap >= 1 && w > ap) w = ap;
                site_write(g_wic_sites, g_wic_n, (unsigned char)w);
                // El pacer y la cuenta viva tampoco pasan la API. El sitio
                // ya estaba recortado y el pacer no: en Metro (modo host,
                // 2026-09-11) mode 2 -> 4 dejo el pacer en 3 con la API en 2
                // durante 2 s -- "RSYNC: Present count mismatch" x400,
                // "Present queue is empty", "Potential dead-lock in Present"
                // y el juego cerro. En los otros juegos la API sube en el
                // mismo frame y la carrera no se ve. [[traducir-la-cuenta-del-juego]]:
                // al subir, API primero y despues el byte -- todos los bytes.
                n = w;
            }
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
    { const LONG t6 = (g_six || count_is_multiplier()) ? 6 : 5; if (n > t6) n = t6; }                    // the plugin's own ceiling
    // The guard first, so a frame can never see a raised bound with the old
    // gate still shut, or the reverse.
    site_write(g_imm2_sites, g_imm2_n, (unsigned char)loop_bound_for(n));
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
    site_write(g_imm3_sites, g_imm3_n, (unsigned char)n);
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
    site_write(g_imm_sites, g_imm_n, (unsigned char)loop_bound_for(n));
    g_count_live = n;
}

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
// El estado del controlador vive en src/controlador.h (ctl::Estado); aca
// queda una instancia y los dos envoltorios que leen el mundo y aplican.
static controller::State g_ctrl;
// Y el del reparto fraccional por bloques (src/reparto.h).
static scheduler::State g_sched;


// Los dos envoltorios del controlador: leen los globales, llaman a
// ctl::control / ctl::aplicar y aplican lo que sale. Las guardas de
// seleccion, de base asentada y del refresh son de este lado porque leen
// el mundo; la regla esta en controlador.h y se pincha en
// tools/test_controlador.cpp.
struct ControllerLog {
    void line(const char *t) { log_line(t); }
    void num(const char *t, unsigned long long v) { log_num(t, v); }
};

static void dyn_control(double base_fps, double presented_fps) {
    if (g_force_sel != kSelDynFuture) return;
    ControllerLog log;
    const controller::Config c{ g_sat_on, g_use_debt, g_dyn_step };
    controller::control(g_ctrl, c, base_fps, presented_fps, log);
}

// Muestras minimas antes de que la salida del estimador valga una decision.
// Cuatro de las ocho del anillo: a 33 fps son 120 ms de espera, y a 300 fps
// son 13. Ocho seria esperar el anillo entero y perder el escalon que el
// detector existe para captar.
static const int kCtrlMinMuestras = 4;

static void dyn_apply(double base_fps) {
    if (g_force_sel != kSelDynFuture) return;
    if (g_dyn_pin > 0) { g_dyn_target = g_dyn_pin; return; }   // ratio fijado: el controlador no decide
    if (base_fps <= 1.0) return;
    if (g_ctrl_fps > 0.0 && g_ctrl_n < kCtrlMinMuestras) {
        static int muted = 0;
        if (++muted % 240 == 1) {
            log_num("dynamic: base sin asentar, no se decide. muestras ",
                    (unsigned)g_ctrl_n);
            log_num("  base que habria usado ", (unsigned)(base_fps + 0.5));
        }
        return;
    }
    if (g_refresh_hz <= 0) {
        DEVMODEW dm; dm.dmSize = sizeof(dm); dm.dmDriverExtra = 0;
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
            g_refresh_hz = (LONG)dm.dmDisplayFrequency;
        if (g_refresh_hz <= 0) g_refresh_hz = 60;
        log_num("dynamic: refresh is ", (unsigned)g_refresh_hz);
    }
    LARGE_INTEGER qnow; QueryPerformanceCounter(&qnow);
    controller::ApplyInput in;
    in.base_fps = base_fps;
    in.target = (g_dyn_fps > 0) ? (double)g_dyn_fps : (double)g_refresh_hz;
    in.now_qpc = qnow.QuadPart;
    in.qpc_freq = g_qpc_freq;
    in.pc = g_rt_present_count;
    in.dyn_target = g_dyn_target;
    ControllerLog log;
    const controller::Config c{ g_sat_on, g_use_debt, g_dyn_step };
    const controller::ApplyOutput s = controller::apply(g_ctrl, c, in, log);
    if (!s.changes) return;
    const LONG cur = g_dyn_target;
    const LONG next = (LONG)s.next;
    g_dyn_target = next;
    if (!g_probe_done && g_probe_left == 0 && s.probe) {
        g_probe_left = 16;
        g_probe_i = 0;
        g_probe_pc0 = g_rt_present_count;
        log_num("probe: ratio dropped from x100 ", (unsigned)cur);
        log_num("  to x100 ", (unsigned)next);
    }
    g_dyn_said = 0;
    g_opt_pending = 1;
    if (!g_dyn_diag) return;
    log_num("dynamic: ratio now x100 ", (unsigned)next);
    log_num("  from base ", (unsigned)(base_fps + 0.5));
    log_num("  frenos por tiron ", (unsigned long long)g_ctrl.brakes_hitch);
    log_num("  frenos por base inestable ", (unsigned long long)g_ctrl.brakes_base);
    log_num("  llamadas del controlador ", (unsigned long long)g_ctrl.calls);
    log_num("  deuda x100 (mas 32768 si es negativa) ",
            (unsigned)(g_ctrl.debt < 0.0 ? 32768u + (unsigned)(-g_ctrl.debt * 100.0 + 0.5)
                                        : (unsigned)(g_ctrl.debt * 100.0 + 0.5)));
    log_num("  objetivo efectivo ", (unsigned)(s.target_eff + 0.5));
    log_num("  ratio crudo x100 ", (unsigned)(s.raw * 100.0 + 0.5));
    log_num("  sesgo x100 ", (unsigned)(s.bias_used * 100.0 + 0.5));
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
    if (g_dyn_pin > 0 && sel_is_frac()) g_dyn_target = g_dyn_pin;
    static LONG was_sel = -1;
    if (g_force_sel != was_sel) {
        was_sel = g_force_sel;
        // Un solo canal por modo: en enteros manda la API, en fraccionales el
        // byte. Ver wic_parche.
        if (g_wic_n > 0) wic_patch(sel_is_frac());
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
    // Con NUESTRO snippet la cuenta es el multiplicador, no los generados.
    //
    // Sin esto la cadencia de un 2.55x alterna entre 1 y 2 -- que con este
    // snippet significa alternar 1X y 2X -- y el promedio se clava en 2.00.
    // Medido en GTA V: pedido 2.51..2.75, entregado 2.00, error mediana 27%.
    // Tiene que alternar entre 2 y 3.
    //
    // Es el mismo desfasaje que dejo 2X sin generar y que hizo que el modo 6
    // pidiera 5. Este era el ultimo lugar donde faltaba. Ver
    // cuenta_es_multiplicador.
    const double kBase = count_is_multiplier() ? 0.0 : 1.0;
    double per_frame = (double)g_dyn_target / 100.0 - kBase;
    // DYNAMIC sin ratio decidido todavia: piso 2.0, no 0.
    //
    // El controlador necesita una base MEDIDA para decidir el ratio, y sin
    // generacion no hay base que medir. Con la semantica vieja la semilla de la
    // cuenta era 1, que eran 1 frame generado = 2X, y con eso arrancaba. Con la
    // nueva, 1 es 1X: no genera, no hay base, el controlador no decide nunca y
    // g_dyn_target se queda en 0. Se espera a si mismo.
    //
    // Medido en Halo: seleccion 8, "base sin asentar, no se decide. muestras 1"
    // y la cuenta a la API clavada en 1 durante toda la sesion -- ratio 1.00 en
    // cada ventana, mientras 4X y 6X entregaban 4.00 y 6.02 en el mismo rato.
    //
    // 2.0 es el mismo piso que el controlador aplica despues a su propia salida,
    // asi que no inventa un valor: arranca donde el iba a terminar de todas
    // formas, y desde ahi mide.
    //
    // La condicion NO puede mirar g_dyn_target. La primera version decia
    // "g_dyn_target < 100" porque en GTA V y en el banco el objetivo arranca en
    // 0 -- pero Halo tiene "target 600" guardado en su mfg-settings.txt, la
    // condicion daba falso, y el piso no disparo una sola vez mientras la cuenta
    // quedaba clavada en 1. Una condicion derivada del estado de UN juego, en
    // codigo que no tiene ramas por juego. Es la misma clase de error que
    // atarse a un flag en vez de mirar el contenido.
    //
    // Lo que decide es el invariante: con la semantica de multiplicador, una
    // cuenta menor a 2 es 1X, o sea nada generado, y eso nunca es lo que un modo
    // que genera quiso pedir.
    if (per_frame < 2.0 &&
        (g_force_sel == kSelDynamic || g_force_sel == kSelDynFuture)) {
        static bool said = false;
        if (!said) {
            said = true;
            log_line("dynamic: sin ratio decidido, se arranca en 2.0 para poder medir la base");
        }
        per_frame = 2.0 - kBase;
    }
    if (per_frame < 0.0) per_frame = 0.0;
    if (per_frame > 6.0) per_frame = 6.0;
    // fracdiff (experimento): difusion por frame en vez de bloques. La reserva
    // (g_wic_sites[1]) se fija a ceil por la API una vez; el bound
    // (g_wic_sites[0]) y el pacer se difunden por frame con Bresenham
    // (scheduler::diffuse_step, testeado sin GPU). Cero llamadas a la API por
    // frame. Solo con los dos sitios parcheados. Ver
    // docs/investigacion-fraccionales.md; el flip queue con bound<reserva es
    // lo que esto mide. Apagado por defecto.
    if (g_frac_diff && g_wic_mode && g_wic_n >= 2 && g_wic_set != 0) {
        const LONG lo = (LONG)per_frame;
        const double frac = per_frame - (double)lo;
        LONG ceil_n = frac > 0.0001 ? lo + 1 : lo;
        { const LONG t6 = (g_six || count_is_multiplier()) ? 6 : 5; if (ceil_n > t6) ceil_n = t6; }
        if (ceil_n < 2) ceil_n = 2;
        // La reserva = ceil, por la API, SOLO cuando ceil cambia (un enfriamiento).
        if (g_force_generated != ceil_n) {
            g_force_generated = ceil_n;
            g_opt_pending = 1;
            log_num("fracdiff: reserva (API) a ceil ", (unsigned)ceil_n);
        }
        // El bound de este frame, difundido; nunca por encima de la reserva.
        LONG n = scheduler::diffuse_step(g_frac_diff_acc, per_frame);
        if (n < 0) n = 0;
        if (n > ceil_n) n = ceil_n;
        // Escribir SOLO el bound y el pacer (g_wic_sites[0], g_pace_count); la
        // reserva (g_wic_sites[1]) queda en ceil, escrita por el wrapper de
        // opciones cuando la API cambio. gen_flag por si genera algo.
        if (g_wic_sites[0] != nullptr) *g_wic_sites[0] = (unsigned char)n;
        if (g_pace_count != nullptr) *g_pace_count = (unsigned char)n;
        if (g_gen_flag != nullptr) *g_gen_flag = (unsigned char)(n > 0 ? 1 : 0);
        g_count_live = n;
        return;
    }

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
    g_cycle_ceiling = lo + 1;   // A1: el maximo que este ciclo va a pedir
    const double frac = per_frame - (double)lo;

    static int pos = 0;
    static int hi_frames = 0;
    if (pos == 0) {
        if (g_slowalt) {
        // El reparto vive en src/reparto.h (rep::tick) y se pincha en
        // tools/test_reparto.cpp. Aca: el reloj, la configuracion, y aplicar
        // la cuenta que salga -- la llamada a la API y el byte.
        double dt = 0.0;
        {
            static LONGLONG prev_qpc = 0;
            LARGE_INTEGER now_qpc;
            QueryPerformanceCounter(&now_qpc);
            if (prev_qpc != 0 && g_qpc_freq > 0)
                dt = (double)(now_qpc.QuadPart - prev_qpc) / (double)g_qpc_freq;
            prev_qpc = now_qpc.QuadPart;
        }
        const scheduler::Config sched_cfg{ g_block_ms, g_blocks, g_latch_schedule,
                              g_peralt, g_nullalt, g_blockalt };
        const scheduler::Input sched_in{ per_frame, lo, frac, dt, g_last_dt };
        ControllerLog sched_log;
        const scheduler::Output sched_out = scheduler::tick(g_sched, sched_cfg, sched_in, sched_log);
        const LONG api = (LONG)sched_out.api;
        if (api != g_force_generated) {
            g_force_generated = api;
            g_opt_pending = 1;
            // Twice per cycle at most, so cheap -- and the only direct evidence
            // that the count moved. The plugin logs a count only when the
            // enabled/disabled state changes, so its log cannot answer this.
            if (g_dyn_diag) log_num("slowalt: API count now ", (unsigned)api);
            g_last_change_pres = g_present_count;
        }
        // Con la semantica nueva el byte NO lleva la cadencia: la lleva la
        // cuenta de la API, y el byte solo tiene que ir en el mismo escalon.
        // Lo escribe set_count_now con g_api_aplicada, pegado a la llamada.
        if (!count_is_multiplier()) set_count_now(api);
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
    // El techo del ratio. `per_frame` son GENERADOS (ratio - 1), que es lo que
    // el snippet del juego espera. Con el nuestro la cuenta es el MULTIPLICADOR,
    // asi que hay que declarar uno mas -- si no, un 2.55x pide 2 y entrega 2X.
    // Es el mismo desfasaje que dejo 2X sin generar. Ver cuenta_es_multiplicador.
    // per_frame ya viene en la escala correcta (ver kBase arriba), asi que el
    // techo del ratio es directamente su parte entera hacia arriba.
    const double for_ceiling = per_frame;
    const LONG ceil_n = (LONG)(for_ceiling + 0.999);
    // Varying the API count at runtime crashes, in every arrangement tried:
    // per frame, in blocks of eight, and in blocks with the loop bound clamped
    // to what the plugin had actually applied so the two could never be out of
    // step. All three end in 0xC0000005. The count is fixed for the life of the
    // run at the ceiling of the ratio.
    {
        const LONG t = (g_six || count_is_multiplier()) ? 6 : 5;
        g_force_generated = ceil_n < 1 ? 1 : (ceil_n > t ? t : ceil_n);
    }
    // Dicho una vez, porque si no es una mentira silenciosa.
    //
    // Esta rama declara el techo y deja la fraccion al byte. Con nuestro snippet
    // el byte ya no modula nada, asi que entrega el techo entero: 2.55x sale
    // 3.00. No se arregla aca -- cambiar la cuenta por frame es una llamada a
    // slDLSSGSetOptions por frame y 100 ms de enfriamiento cada una, que es
    // generacion apagada. El fraccional necesita bloques, o sea g_slowalt, que
    // viene encendido salvo que mfg-noslowalt.txt lo apague.
    if (count_is_multiplier() && g_dyn_target % 100 != 0) {
        static bool said = false;
        if (!said) {
            said = true;
            log_line("frac: sin slowalt el ratio queda en el entero de arriba");
        }
    }

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
