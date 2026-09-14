"""Verifica que el analizador encuentre, en la sesion REAL del testbed, los
defectos que el testbed mete a proposito.

    python3 gpuprobe/tests/testbed_check.py sesion.jsonl [reporte.md]

Esto es lo que hace que el proyecto sea verificable: el testbed corre sobre
WARP en CI con el colector de verdad -- device, query heaps, fences -- y el
analizador tiene que sacar las mismas conclusiones que saca sobre el fixture
sintetico. Si el colector deja de medir bien, esto se pone en rojo aunque
todos los tests unitarios sigan en verde.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "analyzer"))

from gpuprobe_analyzer import ingest, report, rules  # noqa: E402

fails = []


def check(cond, msg):
    print(("  ok   " if cond else "  FALLO ") + msg)
    if not cond:
        fails.append(msg)


def main(path, report_path=None):
    # El warmup por defecto son 120 frames; en CI la corrida es corta, asi que
    # se usa un warmup proporcional pero nunca cero (los primeros frames de
    # WARP incluyen la compilacion de los shaders del driver).
    s = ingest.load(path, warmup=30)
    print(ingest.summary(s))
    print(f"frametime {s.frame_ms:.2f} ms, GPU {s.gpu_ms:.2f} ms, "
          f"thrashing={s.thrashing()}")

    check(s.schema == 1, "la sesion tiene cabecera de esquema 1")
    check(s.bad_lines == 0, f"ninguna linea ilegible (hubo {s.bad_lines})")
    check(len(s.frames) > 50, f"frames registrados: {len(s.frames)}")
    check(len(s.passes) >= 8, f"pasadas distinguidas: {len(s.passes)}")
    check(len(s.resources) >= 12, f"recursos registrados: {len(s.resources)}")

    # Los timestamps tienen que haber llegado: sin esto el colector estaria
    # emitiendo pasadas sin medir y nadie se enteraria.
    timed = [p for p in s.passes.values() if p.ms > 0]
    check(len(timed) >= 8, f"pasadas con tiempo de GPU: {len(timed)}")

    # Clasificacion por descriptor sobre recursos creados de verdad.
    cats = {}
    for r in s.resources.values():
        cats.setdefault(r.cat, []).append(r)
    print("  categorias:", {k: len(v) for k, v in sorted(cats.items())})
    check("shadowmap" in cats, "clasifico el atlas de 4096 como shadowmap")
    check("depthbuffer" in cats, "clasifico el depth de la escena como depthbuffer")
    check("rt_full" in cats, "clasifico los RTs de salida como full-res")
    check("rt_half" in cats, "clasifico el bloom como media resolucion")
    check("volume" in cats, "clasifico la textura 3D como volumen")

    # Async compute: la pasada de la queue 1 tiene que aparecer, y su tiempo
    # exclusivo tiene que ser menor que su costo si solapo con graficos.
    other_queue = [p for p in s.passes.values() if p.queue != 0]
    check(len(other_queue) >= 1, "registro la pasada de la queue de compute")

    cands = rules.analyze(s)
    for c in cands:
        print(f"    {c.gain_ms:6.2f} ms [{c.visual_risk}/{c.stability}] {c.title}")
    by = {c.cid.split("_")[0] for c in cands}
    check("shadow" in by, "encontro el shadow map sobredimensionado")
    check("orphan" in by, "encontro el RT que se escribe y nunca se lee")
    check("barriers" in by, "encontro los barriers redundantes")
    check(any(c.cid == "pso_stutter" for c in cands),
          "encontro el PSO compilado en gameplay")

    # El huerfano tiene hermanos por descriptor en el testbed (hay varios RTs
    # RGBA16F full-res): el reporte tiene que decirlo en vez de proponer una
    # accion que tocaria a los tres.
    orph = [c for c in cands if c.cid.startswith("orphan_")]
    if orph:
        check(any("MISMO descriptor" in e for e in orph[0].evidence),
              "avisa que el matcher del huerfano es ambiguo")

    md = report.render(s, cands)
    if report_path:
        with open(report_path, "w", encoding="utf-8") as fh:
            fh.write(md + "\n")
        print(f"reporte: {report_path}")

    if fails:
        print(f"\n{len(fails)} CHEQUEOS EN ROJO")
        return 1
    print("\ntestbed -> colector -> JSONL -> analizador: en verde")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None))
