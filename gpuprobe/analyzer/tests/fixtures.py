"""Sesiones sinteticas escritas en Python, para los tests unitarios.

Ojo con la duplicacion aparente: tools/gen_session.cpp escribe la sesion con
el writer de C++ (y eso prueba el contrato entre los dos lados, ver
tests/contract.sh). Esto de aca es otra cosa: lineas armadas a mano para poder
construir el caso raro que un generador realista no produce -- un recurso sin
lecturas, una linea cortada, un frame con eventos perdidos.
"""

import json


class Builder:
    def __init__(self, out_w=2560, out_h=1440, budget=11811160064):
        self.lines = [json.dumps({
            "t": "header", "schema": 1, "version": "test", "exe": "test.exe",
            "adapter": "test", "out_w": out_w, "out_h": out_h,
            "vram_budget": budget})]
        self.budget = budget

    def res(self, key, dkey, cat, fmt, w, h, d=1, mips=1, flags=0,
            heap="default", nbytes=0, f=0):
        self.lines.append(json.dumps({
            "t": "res", "f": f, "key": hex(key), "dkey": hex(dkey), "cat": cat,
            "dim": "tex2d", "fmt": fmt, "fmt_id": 0, "w": w, "h": h, "d": d,
            "mips": mips, "samples": 1, "flags": flags, "heap": heap,
            "bytes": nbytes or w * h * 4, "site": "0x1"}))
        return self

    def pass_(self, f, key, rt_key, rt_dkey, gpu_ms, draws=1, queue=0, ordinal=0,
              begin_ms=0.0, rt_w=2560, rt_h=1440, deep=0):
        freq = 1000000000
        begin = int(begin_ms * 1e6)
        self.lines.append(json.dumps({
            "t": "pass", "f": f, "key": hex(key), "rts": hex(rt_dkey),
            "pso": "0x1", "gpu_ms": gpu_ms, "draws": draws, "ord": ordinal,
            "queue": queue, "rt_w": rt_w, "rt_h": rt_h, "rt_bytes": 0,
            "rt_key": hex(rt_key), "rt_dkey": hex(rt_dkey), "begin": begin,
            "end": begin + int(gpu_ms * 1e6), "freq": freq, "deep": deep}))
        return self

    def barrier(self, f, res_key, before, after):
        self.lines.append(json.dumps({
            "t": "barrier", "f": f, "res": hex(res_key), "from": before,
            "to": after, "kind": 0, "dropped": 0}))
        return self

    def pso(self, f, key, compile_ms, render_thread=1):
        self.lines.append(json.dumps({
            "t": "pso", "f": f, "key": hex(key), "compile_ms": compile_ms,
            "bytes": 1000, "stages": 3, "compute": 0,
            "render_thread": render_thread}))
        return self

    def vram(self, f, usage, committed=None):
        self.lines.append(json.dumps({
            "t": "vram", "f": f, "budget": self.budget, "usage": usage,
            "committed": committed if committed is not None else usage,
            "reserved": 0}))
        return self

    def action(self, f, aid, on):
        self.lines.append(json.dumps({
            "t": "action", "f": f, "id": aid, "target": "0x1", "kind": 1,
            "on": 1 if on else 0}))
        return self

    def frame_end(self, f, cpu_ms, dropped=0, deep=0):
        self.lines.append(json.dumps({
            "t": "frame_end", "f": f, "cpu_ms": cpu_ms, "present_ms": 0.2,
            "dropped": dropped, "queries": 10, "deep": deep}))
        return self

    def raw(self, line):
        self.lines.append(line)
        return self

    def build(self):
        return list(self.lines)
