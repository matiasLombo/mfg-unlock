// recorder.h -- el grabador F9 y el hilo del panel: muestras de presentacion
// para saber si un cambio de pacing hizo algo, el CSV, y el lazo de teclas
// que ademas arma los hooks tarde y relee los ajustes.
//
// ENTRA: F9 (grabar/parar), las presentaciones vistas por los hooks de
//   Present (note_present, desde present.h), los marcadores del pacer
//   (kSetPresentConfigNV); el hilo recorder() corre desde DllMain.
// SALE: mfg-frames.csv (write_samples), el resumen "pacing:" en el log
//   (log_pacing_summary), g_samples / g_nsamples / g_recording, y las
//   llamadas a arm_dxgi_recorder / arm_frametoken_hook / settings_watch
//   que el lazo hace cada intervalo.
// DEPENDE DE: present.h (arm_dxgi_recorder), settings.h (settings_watch),
//   log_line/log_num, GetAsyncKeyState, y los globales compartidos que
//   todavia viven en proxy.cpp.
//
// Movido de proxy.cpp tal cual (2026-09-11): un rango, mismo orden. Sin
// tocar una linea del cuerpo.
#pragma once

// Globales que solo usa este modulo (movidas de proxy.cpp).
// Remembered across runs: the mode picked in the panel and, for DYNAMIC, the
// frame-rate target. Nothing else -- the flag files are a separate thing and
// are not rewritten from here, so a file the player created by hand is never
// silently replaced by one of ours.
static volatile LONG g_settings_dirty = 0;

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

static LONG g_written = 0;
static const int kMaxSamples = 200000;
static int g_run_no = 0;

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

#include "present.h"

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
                wchar_t path[MAX_PATH];
                beside_dll(path, L"mfg-twocopies.txt");
                HANDLE h2 = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
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
                // La fila 6X no se puede elegir.
                //
                // Mata el juego en Halo de forma reproducible, en el instante
                // del cambio. Se limito la cuenta escrita a 4 en las dos
                // mitades -- API y byte -- y crasheo IGUAL, asi que la causa no
                // es solo el valor de la cuenta y no esta identificada.
                //
                // Se saca la fila en vez de dejar una que rompe: 2X a 5X andan
                // y sostienen la sesion. mfg-x6.txt la devuelve para
                // investigarla.
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
                            // Con nuestro snippet la cuenta ES el
                            // multiplicador. Ver cuenta_es_multiplicador.
                            g_force_generated = g_ov_hot >= 2
                                ? (count_is_multiplier() ? g_ov_hot : g_ov_hot - 1)
                                : 0;
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
        host_arm_d3d12();
        host_poll();
        emit_verdict_if_due();
        // Y si el juego ya tenia su swapchain hecho cuando llegamos, el hook de
        // la factory no lo va a ver nunca. Se adopta por la vtable compartida.
        adopt_existing_swapchain();

        // M4: preguntar y registrar la respuesta.
        //
        // Solo se pregunta si el diagnostico guardado es ROJO y no hay respuesta
        // todavia. Un juego VERDE o AMARILLO no ve nada de esto.
        read_previous_verdict();
        g_pide_permiso = (g_previous_verdict == 2 && g_consent == -1) ? 1 : 0;
        if (g_pide_permiso) {
            const bool si = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
            const bool no = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            if (si || no) {
                g_consent = si ? 1 : 0;
                g_pide_permiso = 0;
                save_consent(g_consent);
                log_line(si ? "permiso: el usuario acepto el reemplazo del Streamline"
                            : "permiso: el usuario dijo que no; no se sustituye nada");
                log_line("  (toma efecto en el proximo arranque del juego)");
            }
        }

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
