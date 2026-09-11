// El reparto fraccional por bloques (slowalt), tal como corre: que cuenta se
// pide en ESTE frame para que el promedio del ciclo sea el ratio pedido.
//
// EXTRACCION del bloque `if (g_slowalt)` de fractional_tick (proxy.cpp,
// 2026-09-11), con los mismos numeros. Los estaticos de funcion y los globales
// g_lo_*/g_hi_*/g_rate_* son campos de `Estado`; el reloj entra como delta;
// la configuracion (largo de bloque, bloques por ciclo, latch, difusion) por
// parametro; el log de cierre de ciclo sale por callback con las mismas
// cadenas.
//
// Lo que hace, en una frase: el ciclo son `blocks` bloques de `block_secs`;
// `hi_blocks` de ellos piden lo+1 y el resto lo, y la particion se decide UNA
// vez por ciclo ([[latch-the-schedule]]), corrigiendo la fraccion por las
// tasas medidas de cada bloque -- el bloque alto renderiza mas lento, asi
// que necesita mas tiempo para pesar lo mismo ([[fractional-by-block-alternation]]).
// El offset aleatorio por ciclo evita que el bloque alto caiga siempre en la
// misma fase.
//
// Se quitaron tres cosas que el codigo escribia y nunca leia: sa_pos, sa_bias
// (siempre 0.0) y cyc_gen/cyc_frames. Comportamiento identico.
#pragma once

namespace scheduler {

struct State {
    double sa_clock = 0.0;         // segundos dentro del ciclo
    int    sa_offset = 0;          // bloque donde arranca el tramo alto
    unsigned sa_rng = 0x9E3779B9u; // xorshift para el offset
    int    hi_blocks = -1;         // -1 sin decidir, -2 recalcular (sin latch)
    double last_req = -1.0;        // la fraccion pedida la ultima vez
    // difusion por frame (peralt)
    double acc = 0.0;
    double last_pf = -1.0;
    // tasas medidas por bloque, para corregir la particion
    long   lo_frames_seen = 0, hi_frames_seen = 0;
    double lo_time = 0.0, hi_time = 0.0;
    double rate_lo = 0.0, rate_hi = 0.0;
};

struct Config {
    int    block_ms;      // 0 = 24 ms si lo>=1, 16 ms si no
    int    blocks;        // 0 = 32
    bool   latch;         // el reparto se decide una vez por ciclo
    bool   peralt, nullalt, blockalt;
};

struct Input {
    double per_frame;     // el ratio pedido menos kBase, ya acotado a [0, 6]
    long   lo;            // (long)per_frame
    double frac;          // per_frame - lo
    double dt;            // segundos desde el tick anterior (0 si no hay)
    double last_dt;       // g_last_dt: cuanto duro el ultimo frame
};

struct Output {
    long   want;          // la cuenta que quiere este frame
    long   api;           // acotada a [0, 5]: lo que se manda
    bool   closed;        // se cerro un ciclo con datos (se logueo)
};

template <class Log>
inline Output tick(State &e, const Config &c, const Input &in, Log &log) {
    Output s{};
    if (in.dt > 0.0 && in.dt < 0.5) e.sa_clock += in.dt;
    const double kBlockSecs = c.block_ms > 0 ? (double)c.block_ms / 1000.0
                                             : (in.lo >= 1 ? 0.024 : 0.016);
    const int kBlocks = c.blocks > 0 ? c.blocks : 32;
    const double cycle_secs = kBlockSecs * (double)kBlocks;
    bool cycle_wrapped = false;
    if (e.sa_clock >= cycle_secs) {
        e.sa_clock -= cycle_secs;
        cycle_wrapped = true;
        e.sa_rng ^= e.sa_rng << 13; e.sa_rng ^= e.sa_rng >> 17; e.sa_rng ^= e.sa_rng << 5;
        e.sa_offset = (int)(e.sa_rng % (unsigned)(kBlocks > 0 ? kBlocks : 1));
    }
    const int sa_pos_block = (int)(e.sa_clock / kBlockSecs);
    // Cierre de ciclo: las tasas de cada bloque, para la particion siguiente.
    if (e.sa_clock < 0.05 && e.lo_time + e.hi_time > 0.5) {
        if (e.lo_frames_seen + e.hi_frames_seen > 0) {
            const double tot_ren = (double)(e.lo_frames_seen + e.hi_frames_seen);
            const double tot_pres = (double)e.lo_frames_seen * (double)(in.lo + 1)
                                  + (double)e.hi_frames_seen * (double)(in.lo + 2);
            log.num("slowalt: CIRCULAR ratio x100 ",
                    (unsigned long long)(int)(tot_pres / tot_ren * 100.0 + 0.5));
            log.num("  low block fps x10 ",
                    (unsigned long long)(int)(e.lo_time > 0.0 ?
                        (double)e.lo_frames_seen / e.lo_time * 10.0 : 0.0));
            {
                const double t = e.lo_time + e.hi_time;
                if (t > 0.0) {
                    log.num("  presented fps x10 ", (unsigned long long)(int)(tot_pres / t * 10.0));
                    log.num("  rendered fps x10 ", (unsigned long long)(int)(tot_ren / t * 10.0));
                }
            }
            log.num("  high block fps x10 ",
                    (unsigned long long)(int)(e.hi_time > 0.0 ?
                        (double)e.hi_frames_seen / e.hi_time * 10.0 : 0.0));
            e.rate_lo = e.lo_time > 0.05 ? (double)e.lo_frames_seen / e.lo_time : 0.0;
            e.rate_hi = e.hi_time > 0.05 ? (double)e.hi_frames_seen / e.hi_time : 0.0;
            e.lo_frames_seen = 0;
            e.hi_frames_seen = 0;
            e.lo_time = 0.0;
            e.hi_time = 0.0;
            s.closed = true;
        }
    }
    double t_frac = in.frac;
    if (t_frac < 0.0) t_frac = 0.0;
    if (t_frac > 1.0) t_frac = 1.0;
    {
        // El bloque alto renderiza mas lento: para que pese lo pedido en
        // frames necesita mas tiempo. Se reparte por tiempo, no por bloques.
        const double r_lo = e.rate_lo;
        const double r_hi = e.rate_hi;
        if (r_lo > 1.0 && r_hi > 1.0) {
            const double a = t_frac / r_hi;
            const double b = (1.0 - t_frac) / r_lo;
            if (a + b > 0.0) t_frac = a / (a + b);
        }
    }
    bool request_changed = false;
    if (in.frac != e.last_req) {
        const double salto = in.frac > e.last_req ? in.frac - e.last_req : e.last_req - in.frac;
        e.last_req = in.frac;
        request_changed = e.hi_blocks >= 0 && salto > 0.5;
        if (e.hi_blocks >= 0 && salto <= 0.5 && !c.latch) e.hi_blocks = -2;
    }
    if (request_changed) e.sa_clock = 0.0;
    if (cycle_wrapped || request_changed || e.hi_blocks < 0) {
        e.hi_blocks = (int)(t_frac * (double)kBlocks + 0.5);
        if (e.hi_blocks < 0) e.hi_blocks = 0;
        if (e.hi_blocks > kBlocks) e.hi_blocks = kBlocks;
    }
    const bool diffuse = c.peralt && !c.nullalt && !c.blockalt;
    long want;
    if (diffuse) {
        if (in.per_frame != e.last_pf) { e.last_pf = in.per_frame; e.acc = 0.0; }
        e.acc += in.per_frame;
        want = (long)e.acc;
        e.acc -= (double)want;
        if (want < 0) want = 0;
        if (want > in.lo + 1) want = in.lo + 1;
    } else {
        const int rel = (sa_pos_block - e.sa_offset + kBlocks) % kBlocks;
        want = c.nullalt ? in.lo + 1
             : ((rel < e.hi_blocks) ? in.lo + 1 : in.lo);
    }
    const long api = want < 0 ? 0 : (want > 5 ? 5 : want);
    if (api > in.lo) { ++e.hi_frames_seen; e.hi_time += in.last_dt; }
    else             { ++e.lo_frames_seen; e.lo_time += in.last_dt; }
    s.want = want;
    s.api = api;
    return s;
}

}  // namespace rep
