"""Lectura del JSONL de sesion.

Tres cosas que el lector tiene que hacer bien y que no son obvias:

1. Una linea rota no tira el archivo. Si el juego crasheo, la ultima linea
   quedo cortada; y si el ring descarto eventos, faltan lineas del medio. Se
   cuentan (bad_lines) y se sigue.
2. Los frames "deep" (instrumentacion completa) miden distinto que los light.
   Las muestras se guardan separadas y el costo por pasada se calcula con las
   light, que son las que representan al juego sin nosotros encima.
3. Los primeros frames de una sesion son carga de nivel, no gameplay. Se
   descartan por defecto (warmup) porque si no, el "top de pasadas" queda
   dominado por la primera compilacion de todo.
"""

import json
from typing import Iterable, Optional

from .model import (BarrierAgg, FrameAgg, PassAgg, PsoCompile, Resource,
                    Session, VramSample)

# Estados de D3D12_RESOURCE_STATES que nos importan (los mismos valores que
# core/types.h; ver ese archivo para por que coinciden a proposito).
STATE_COMMON = 0
STATE_RENDER_TARGET = 0x4
STATE_UNORDERED_ACCESS = 0x8
STATE_DEPTH_WRITE = 0x10
STATE_DEPTH_READ = 0x20
STATE_NON_PIXEL_SHADER = 0x40
STATE_PIXEL_SHADER = 0x80
STATE_COPY_DEST = 0x400
STATE_COPY_SOURCE = 0x800

WRITE_STATES = STATE_RENDER_TARGET | STATE_UNORDERED_ACCESS | STATE_DEPTH_WRITE
READ_STATES = STATE_NON_PIXEL_SHADER | STATE_PIXEL_SHADER | STATE_DEPTH_READ

DEFAULT_WARMUP = 120


def _int(v, default=0) -> int:
    if v is None:
        return default
    if isinstance(v, str):
        return int(v, 16) if v.startswith(("0x", "0X")) else int(v or default)
    return int(v)


def load(path: str, warmup: int = DEFAULT_WARMUP) -> Session:
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return load_lines(fh, warmup=warmup, path=path)


def load_lines(lines: Iterable[str], warmup: int = DEFAULT_WARMUP,
               path: str = "") -> Session:
    s = Session(path=path, warmup_frames=warmup)
    # Estado por frame para detectar solapamiento entre queues y round-trips
    # de barrier. Se resetea en cada frame_end.
    frame_intervals = []      # (queue, begin_ms, end_ms, pass_key)
    frame_barriers = []       # (res_key, before, after)
    first_tick = {}

    for raw in lines:
        raw = raw.strip()
        if not raw:
            continue
        try:
            ev = json.loads(raw)
        except ValueError:
            s.bad_lines += 1
            continue
        if not isinstance(ev, dict):
            s.bad_lines += 1
            continue

        t = ev.get("t")
        if t == "header":
            s.schema = _int(ev.get("schema"))
            s.version = ev.get("version", "")
            s.exe = ev.get("exe", "")
            s.adapter = ev.get("adapter", "")
            s.out_w = _int(ev.get("out_w"))
            s.out_h = _int(ev.get("out_h"))
            s.vram_budget = _int(ev.get("vram_budget"))
            continue

        f = _int(ev.get("f"))

        if t == "res":
            r = Resource(
                key=_int(ev.get("key")), dkey=_int(ev.get("dkey")),
                cat=ev.get("cat", "unknown"), fmt=ev.get("fmt", "UNKNOWN"),
                fmt_id=_int(ev.get("fmt_id")), w=_int(ev.get("w")),
                h=_int(ev.get("h")), d=_int(ev.get("d"), 1),
                mips=_int(ev.get("mips"), 1), samples=_int(ev.get("samples"), 1),
                flags=_int(ev.get("flags")), heap=ev.get("heap", "unknown"),
                bytes=_int(ev.get("bytes")), site=_int(ev.get("site")),
                first_frame=f)
            s.resources[r.key] = r
            s.by_dkey.setdefault(r.dkey, []).append(r)

        elif t == "res_free":
            s.resources.pop(_int(ev.get("key")), None)

        elif t == "pass":
            key = _int(ev.get("key"))
            p = s.passes.get(key)
            if p is None:
                p = PassAgg(key=key, rt_key=_int(ev.get("rt_key")),
                            rt_dkey=_int(ev.get("rt_dkey")),
                            ordinal=_int(ev.get("ord")), queue=_int(ev.get("queue")),
                            rt_w=_int(ev.get("rt_w")), rt_h=_int(ev.get("rt_h")),
                            rt_bytes=_int(ev.get("rt_bytes")))
                s.passes[key] = p
            p.draws = max(p.draws, _int(ev.get("draws")))
            # Que una pasada escriba sobre el recurso es evidencia directa de
            # escritura: no depende de que el juego haya emitido un barrier.
            # Un RT creado ya en RENDER_TARGET y nunca transicionado -- que es
            # exactamente como se ve un recurso huerfano -- no emite ninguno.
            rt = s.resources.get(p.rt_key)
            if rt is not None:
                rt.states_written.add(STATE_RENDER_TARGET)
            if f > warmup:
                ms = float(ev.get("gpu_ms", 0.0) or 0.0)
                if _int(ev.get("deep")):
                    p.deep_samples.append(ms)
                else:
                    p.samples.append(ms)
                p.frames_seen += 1
                begin = _int(ev.get("begin"))
                freq = _int(ev.get("freq")) or 1
                base = first_tick.setdefault((f, p.queue), begin)
                b_ms = (begin - base) * 1000.0 / freq
                frame_intervals.append((p.queue, b_ms, b_ms + ms, key))

        elif t in ("draw", "dispatch"):
            key = _int(ev.get("pass"))
            p = s.passes.get(key)
            if p is not None and not p.samples and not p.deep_samples:
                p.draws = max(p.draws, 1)

        elif t == "barrier":
            res_key = _int(ev.get("res"))
            before = _int(ev.get("from"))
            after = _int(ev.get("to"))
            k = (res_key, before, after)
            b = s.barriers.get(k)
            if b is None:
                b = BarrierAgg(res_key=res_key, before=before, after=after)
                s.barriers[k] = b
            b.count += 1
            if before == after:
                b.redundant_same_state += 1
            r = s.resources.get(res_key)
            if r is not None:
                if after & WRITE_STATES:
                    r.states_written.add(after)
                if after & READ_STATES:
                    r.states_read.add(after)
                if after == STATE_COPY_SOURCE:
                    r.was_copy_src = True
                if after == STATE_COPY_DEST:
                    r.was_copy_dst = True
            if f > warmup:
                frame_barriers.append((res_key, before, after))

        elif t == "pso":
            s.psos.append(PsoCompile(
                key=_int(ev.get("key")), frame=f,
                compile_ms=float(ev.get("compile_ms", 0.0) or 0.0),
                render_thread=bool(_int(ev.get("render_thread"))),
                bytes=_int(ev.get("bytes"))))

        elif t == "action":
            aid = _int(ev.get("id"))
            s.toggles.setdefault(aid, []).append((f, _int(ev.get("on"))))

        elif t == "vram":
            s.vram.append(VramSample(frame=f, budget=_int(ev.get("budget")),
                                     usage=_int(ev.get("usage")),
                                     committed=_int(ev.get("committed"))))

        elif t == "frame_end":
            s.frames.append(FrameAgg(
                index=f, cpu_ms=float(ev.get("cpu_ms", 0.0) or 0.0),
                present_ms=float(ev.get("present_ms", 0.0) or 0.0),
                dropped=_int(ev.get("dropped")), deep=bool(_int(ev.get("deep")))))
            _close_frame(s, frame_intervals, frame_barriers)
            frame_intervals = []
            frame_barriers = []
            first_tick = {}

    return s


def _close_frame(s: Session, intervals, barriers) -> None:
    """Cierra el frame: tiempo exclusivo por pasada y round-trips por COMMON."""
    # Tiempo exclusivo: los ms de la pasada que ninguna otra queue esta usando
    # al mismo tiempo. Es el techo de lo que se puede ganar optimizandola.
    # Dentro de una misma queue las pasadas son seriales, asi que solo tapan
    # las de las OTRAS queues.
    for q, b, e, key in intervals:
        p = s.passes.get(key)
        if p is None:
            continue
        covers = sorted((max(b, b2), min(e, e2))
                        for q2, b2, e2, _k in intervals
                        if q2 != q and b2 < e and b < e2)
        covered = 0.0
        cur_b = cur_e = None
        for cb, ce in covers:
            if cur_e is None:
                cur_b, cur_e = cb, ce
            elif cb <= cur_e:
                cur_e = max(cur_e, ce)
            else:
                covered += cur_e - cur_b
                cur_b, cur_e = cb, ce
        if cur_e is not None:
            covered += cur_e - cur_b
        p.exclusive.append(max(0.0, (e - b) - covered))

    # X -> COMMON -> X sobre el mismo recurso dentro del mismo frame: dos
    # barriers donde alcanzaba con ninguno (o con uno solo, X -> Y).
    last = {}
    for res_key, before, after in barriers:
        prev = last.get(res_key)
        if prev is not None and prev[1] == STATE_COMMON and before == STATE_COMMON:
            k = (res_key, prev[0], STATE_COMMON)
            b = s.barriers.get(k)
            if b is not None:
                b.common_roundtrip += 1
        last[res_key] = (before, after)


def summary(s: Session) -> str:
    return (f"{s.exe} @ {s.out_w}x{s.out_h} -- {len(s.frames)} frames, "
            f"{len(s.passes)} pasadas, {len(s.resources)} recursos, "
            f"{s.bad_lines} lineas ilegibles")
