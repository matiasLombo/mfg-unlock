// config_apply.h -- de la configuracion leida (config.h) a los globales que
// cada modulo usa, en el orden y con los avisos que DllMain tenia.
//
// ENTRA: los archivos al lado de la dll (banderas, numericos, mfg-config.txt)
//   por config::read_flags / read_numerics / parse_config.
// SALE: los globales de configuracion de todos los modulos (g_six, g_wic_mode,
//   g_slowalt, g_block_ms, g_marker_*, g_slow_frame_*, ...), arm_slinit_temprano
//   cuando g_ota, la cuenta recalculada del modo fijo, settings_load(), y las
//   lineas "config:" / "tope:" / "bench:" del log.
// DEPENDE DE: config.h, beside_dll/flag_file, log_line/log_num, y los
//   globales de todos los modulos (por eso se incluye al final, antes de
//   DllMain).
//
// Salido de DllMain (2026-09-11) como funcion con nombre; el cuerpo es el
// mismo bloque. docs/configuracion.md documenta cada clave.
#pragma once

// La configuracion: leerla (config.h) y repartirla a los globales.
static void apply_config(void) {
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
                        const int seen = config::parse_config(cbuf, (unsigned)got, g_cfg);
                        log_num("config: claves leidas de mfg-config.txt ", (unsigned)seen);
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
            if (g_ota) arm_slinit_early();
            g_slowalt = a.slowalt;
            g_quiet = a.quiet;
            g_nullalt = a.nullalt;
            g_dyn_step = a.dynstep;
            if (a.dynstep != 0) log_num("dynamic: grilla del ratio (dynstep), centesimas ", (unsigned)a.dynstep);
            g_dyn_pin = a.dynpin;
            if (a.dynpin != 0) log_num("dynamic: ratio FIJADO (dynpin) en centesimas -- instrumento ", (unsigned)a.dynpin);
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
            g_fixed_cap = a.topefijo;
            if (g_fixed_cap) log_line("tope fijo en 5 (mfg-topefijo.txt): es la LINEA BASE, crashea");
            g_allow_x6 = a.x6;
            if (g_allow_x6) log_line("6X habilitado a mano (mfg-x6.txt): crashea en Halo");
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
            g_host_on = a.host;
            g_host_hudless = a.hosthudless;
            if (g_host_on && g_host_hudless) log_line("host: se taggea la salida de DLSS como color sin HUD (hosthudless 1)");
            if (g_host_on) log_line("host: modo host ENCENDIDO (Streamline nuestro en un juego sin Streamline)");
}
