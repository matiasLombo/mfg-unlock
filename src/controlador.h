// El controlador de DYNAMIC, tal como corre: dyn_control + dyn_apply + la
// deteccion de techo (sat) + el sesgo por tramo, como estado explicito y dos
// funciones puras.
//
// EXTRACCION de proxy.cpp (2026-09-11), rama por rama y con los mismos
// numeros. Todo lo que eran estaticos de funcion y globales g_dyn_* / g_sat_*
// esta en `Estado`; todo lo que se leia del mundo (reloj, contador de
// presentaciones, refresh, configuracion) entra por parametro; lo que se
// logueaba sale por un callback con las MISMAS cadenas, para que el log no
// cambie y para que tools/test_controlador.cpp pueda verlo.
//
// Por que asi: el controlador es la mitad del comportamiento de DYNAMIC y
// hasta hoy solo se podia probar con un juego abierto. Las mediciones que
// justifican cada regla estan en los comentarios de proxy.cpp (git) y en las
// memorias [[dynamic-controller]] y [[pacer-blocking-budget]]; aca queda la
// regla, no la historia.
#pragma once

namespace ctl {

inline int bucket(double ratio) {
    int b = (int)ratio;
    if (b < 2) b = 2;
    if (b > 5) b = 5;
    return b;
}

struct Estado {
    // dyn_control
    double last_base = 0.0;
    double asked_sum = 0.0;
    long   asked_n = 0;
    double bias[6] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };
    bool   recortado = false;      // la salida quedo pegada a un tope: no aprender
    // sat: el ratio mas barato que sostiene las presentadas
    double sat_pres = 0.0;
    double sat_ratio_max = 0.0;    // 0 = sin limite
    double sat_prev_asked = 0.0, sat_prev_pres = 0.0;
    bool   sat_quieto = false;
    // dyn_apply
    double debt = 0.0;
    unsigned long llamadas = 0, frenos_tiron = 0, frenos_base = 0;
    bool   salida_saturada = false;
    long   last_pc = 0;
    long long last_qpc = 0;
    double base_prev = 0.0;
    long   dicho_techo = -1;       // dedupe de "recortado al techo medido"
    bool   said_hi = false;        // dedupe de "target needs more than 6x"
};

// Lo que no cambia por tick.
struct Config {
    bool sat_on = true;            // mfg-sinsat.txt lo apaga
    bool usar_deuda = true;        // mfg-sin-deuda.txt lo apaga
};

inline void sat_reset(Estado &e) {
    e.sat_pres = 0.0;
    e.sat_ratio_max = 0.0;
    e.sat_quieto = false;
    e.sat_prev_asked = 0.0;
    e.sat_prev_pres = 0.0;
}

// Una vez por ventana de medicion. `log` tiene linea(const char*) y
// num(const char*, unsigned long long).
template <class Log>
inline void control(Estado &e, const Config &c, double base_fps, double presented_fps, Log &log) {
    if (base_fps <= 1.0 || presented_fps <= 1.0) return;
    const bool stable = e.last_base > 1.0 &&
                        (base_fps > e.last_base ? base_fps - e.last_base
                                                : e.last_base - base_fps) < 4.0;
    e.last_base = base_fps;
    const double asked_avg = e.asked_n > 0 ? e.asked_sum / (double)e.asked_n : 0.0;
    e.asked_sum = 0.0;
    e.asked_n = 0;
    if (!stable) {
        // Lo aprendido vale para ESTE punto de operacion.
        if (e.sat_ratio_max > 0.0) log.linea("sat: la base cambio, se olvida el techo");
        sat_reset(e);
        return;
    }
    if (e.recortado) return;       // salida recortada: el error no es del modelo
    if (asked_avg < 2.0) return;
    const double delivered = presented_fps / base_fps;
    if (delivered <= 0.5) return;
    log.num("dynbias: asked x100 ", (unsigned long long)(asked_avg * 100.0 + 0.5));
    log.num("  delivered x100 ", (unsigned long long)(delivered * 100.0 + 0.5));
    log.num("  at base ", (unsigned long long)(base_fps + 0.5));
    if (c.sat_on) {
        if (presented_fps > e.sat_pres) e.sat_pres = presented_fps;
        // Subir el ratio y no cobrar frames: eso es el techo.
        if (e.sat_ratio_max <= 0.0 && e.sat_prev_asked > 0.0 &&
            asked_avg > e.sat_prev_asked + 0.15 &&
            presented_fps < e.sat_prev_pres * 1.02) {
            e.sat_ratio_max = e.sat_prev_asked;
            log.num("sat: techo detectado, presentadas x10 ",
                    (unsigned long long)(e.sat_pres * 10.0 + 0.5));
            log.num("  el ratio se limita a x100 ",
                    (unsigned long long)(e.sat_ratio_max * 100.0 + 0.5));
            log.num("  se venia pidiendo x100 ", (unsigned long long)(asked_avg * 100.0 + 0.5));
        } else if (e.sat_ratio_max > 0.0 && !e.sat_quieto) {
            // Con techo: se tantea hacia abajo mientras las presentadas
            // aguanten; el primer paso que duele define el piso y se congela.
            if (presented_fps >= e.sat_pres * 0.98) {
                if (e.sat_ratio_max > 2.10) {
                    e.sat_ratio_max -= 0.10;
                    log.num("sat: mas barato, ratio x100 ",
                            (unsigned long long)(e.sat_ratio_max * 100.0 + 0.5));
                }
            } else if (presented_fps < e.sat_pres * 0.97) {
                e.sat_ratio_max += 0.10;
                e.sat_quieto = true;
                log.num("sat: piso encontrado, ratio queda en x100 ",
                        (unsigned long long)(e.sat_ratio_max * 100.0 + 0.5));
            }
        }
        e.sat_prev_asked = asked_avg;
        e.sat_prev_pres = presented_fps;
    }
    const double inst = asked_avg / delivered;
    const int bk = bucket(asked_avg);
    e.bias[bk] += (inst - e.bias[bk]) * 0.25;
    if (e.bias[bk] < 0.80) e.bias[bk] = 0.80;
    if (e.bias[bk] > 1.25) e.bias[bk] = 1.25;
}

struct EntradaAplicar {
    double base_fps;
    double target;             // fps presentados que se quieren (dynfps o refresh)
    long long now_qpc;
    long long qpc_freq;
    long   pc;                 // contador de presentaciones del runtime
    long   dyn_target;         // el ratio en vigor, x100
};

struct SalidaAplicar {
    bool   cambia;             // hay ratio nuevo
    long   next;               // el ratio nuevo, x100
    bool   sonda;              // arranco la sonda de caida de ratio
    // para el diagnostico opcional (mfg-dyndiag.txt)
    double target_eff, raw, sesgo;
};

// Una vez por frame renderizado. Devuelve si cambia el ratio y a cuanto; el
// que llama escribe g_dyn_target y arma la sonda. `log` como en control().
template <class Log>
inline SalidaAplicar aplicar(Estado &e, const Config &c, const EntradaAplicar &in, Log &log) {
    SalidaAplicar s{};
    const double target = in.target;
    const double base_fps = in.base_fps;
    const long pc = in.pc;
    if (e.last_qpc != 0 && in.now_qpc > e.last_qpc && in.qpc_freq > 0 && pc >= e.last_pc) {
        const double secs = (double)(in.now_qpc - e.last_qpc) / (double)in.qpc_freq;
        if (secs < 0.5) {              // un salto largo es un cambio de escena
            double inc = target * secs - (double)(pc - e.last_pc);
            if (e.salida_saturada && inc > 0.0) inc = 0.0;
            const double esperado = (base_fps > 1.0) ? (1.0 / base_fps) : 0.02;
            const bool tiron = (secs > esperado * 2.0);
            bool base_inestable = false;
            if (e.base_prev > 1.0 && base_fps > 1.0) {
                const double d = (base_fps > e.base_prev)
                                 ? (base_fps - e.base_prev) / e.base_prev
                                 : (e.base_prev - base_fps) / e.base_prev;
                base_inestable = (d > 0.12);
            }
            e.base_prev = base_fps;
            ++e.llamadas;
            if ((tiron || base_inestable) && inc > 0.0) {
                inc = 0.0;
                if (tiron) ++e.frenos_tiron; else ++e.frenos_base;
            }
            e.debt += inc;
            const double lim = target * 0.33;
            if (e.debt > lim) e.debt = lim;
            if (e.debt < -lim) e.debt = -lim;
        } else {
            e.debt = 0.0;
        }
    }
    e.last_pc = pc;
    e.last_qpc = in.now_qpc;
    const double target_eff = c.usar_deuda ? (target + e.debt * 2.0) : target;
    const double raw = (target_eff > 1.0 ? target_eff : 1.0) / base_fps;
    double want = raw * e.bias[bucket(raw)];
    e.salida_saturada = (want >= 6.0);
    e.recortado = (want >= 6.0 || want <= 2.0);
    {
        const double techo = 6.0, piso = 2.0;
        const double recorte = want > techo ? want - techo
                             : (want < piso ? want - piso : 0.0);
        if (recorte != 0.0 && c.usar_deuda) {
            e.debt -= recorte * base_fps / 2.0;
            const double lim2 = target * 0.33;
            if (e.debt > lim2) e.debt = lim2;
            if (e.debt < -lim2) e.debt = -lim2;
        }
    }
    if (c.sat_on && e.sat_ratio_max >= 2.0 && want > e.sat_ratio_max) {
        const long q = (long)(e.sat_ratio_max * 100.0 + 0.5);
        if (e.dicho_techo != q) {
            e.dicho_techo = q;
            log.num("dynamic: recortado al techo medido, ratio x100 ", (unsigned long long)q);
        }
        want = e.sat_ratio_max;
    }
    if (want > 6.0) {
        if (!e.said_hi) {
            e.said_hi = true;
            log.num("dynamic: target needs more than 6x at this base, fps ",
                    (unsigned long long)target);
            log.num("  base is ", (unsigned long long)(base_fps + 0.5));
        }
        want = 6.0;
    }
    if (want < 2.0) want = 2.0;
    e.asked_sum += (double)in.dyn_target / 100.0;
    ++e.asked_n;
    s.target_eff = target_eff;
    s.raw = raw;
    s.sesgo = e.bias[bucket(raw)];
    const long next = (long)(want * 100.0 + 0.5);
    const long cur = in.dyn_target;
    const long diff = next > cur ? next - cur : cur - next;
    if (diff < 10) return s;
    s.cambia = true;
    s.next = next;
    s.sonda = (cur - next > 80);
    return s;
}

}  // namespace ctl
