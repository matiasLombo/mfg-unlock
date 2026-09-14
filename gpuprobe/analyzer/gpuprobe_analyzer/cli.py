"""CLI del analizador.

    python3 -m gpuprobe_analyzer analyze sesion.jsonl [--report r.md] [--profile p.toml]
    python3 -m gpuprobe_analyzer passes  sesion.jsonl
    python3 -m gpuprobe_analyzer ab      ab-sesion.jsonl
"""

import argparse
import json
import sys

from . import __version__, ab as ab_mod, ingest, report as report_mod
from .rules import Thresholds, analyze


def _add_thresholds(ap):
    ap.add_argument("--warmup", type=int, default=Thresholds.warmup,
                    help="frames iniciales a descartar (carga de nivel)")
    ap.add_argument("--min-gain", type=float, default=Thresholds.min_gain_ms,
                    help="ganancia minima en ms para proponer algo")
    ap.add_argument("--top", type=int, default=Thresholds.top_n,
                    help="cuantas pasadas mostrar antes de colapsar el resto")


def _thresholds(args) -> Thresholds:
    return Thresholds(min_gain_ms=args.min_gain, top_n=args.top,
                      warmup=args.warmup)


def cmd_analyze(args) -> int:
    th = _thresholds(args)
    s = ingest.load(args.session, warmup=args.warmup)
    if s.schema == 0:
        print(f"{args.session}: sin cabecera; no se puede saber la resolucion "
              f"de salida ni la version del esquema", file=sys.stderr)
        return 2
    cands = analyze(s, th)

    if args.json:
        out = {
            "exe": s.exe, "adapter": s.adapter,
            "out": [s.out_w, s.out_h], "frames": len(s.frames),
            "frame_ms": s.frame_ms, "gpu_ms": s.gpu_ms,
            "thrashing": s.thrashing(),
            "candidates": [{
                "id": c.cid, "title": c.title, "kind": c.kind,
                "gain_ms": round(c.gain_ms, 4), "hitch_ms": round(c.hitch_ms, 4),
                "visual_risk": c.visual_risk, "stability": c.stability,
                "blocked": c.blocked, "basis": c.gain_basis,
                "evidence": c.evidence, "action": c.action_toml,
            } for c in cands],
        }
        print(json.dumps(out, indent=2, ensure_ascii=False))
        return 0

    text = report_mod.render(s, cands, th)
    if args.report:
        with open(args.report, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
        print(f"reporte: {args.report}")
    else:
        print(text)

    if args.profile:
        with open(args.profile, "w", encoding="utf-8") as fh:
            fh.write(report_mod.profile_toml(s, cands) + "\n")
        print(f"perfil: {args.profile}")
    return 0


def cmd_passes(args) -> int:
    s = ingest.load(args.session, warmup=args.warmup)
    print(ingest.summary(s))
    for p in s.pass_list()[:args.top]:
        r = s.resource_for_pass(p)
        rt = f"{r.cat} {r.w}x{r.h} {r.fmt}" if r else "?"
        tag = " (tapada)" if p.overlapped else ""
        print(f"  {p.ms:7.3f} ms  excl {p.exclusive_ms:7.3f}{tag}  "
              f"{p.draws:6d} draws  q{p.queue}  {rt}")
    return 0


def cmd_ab(args) -> int:
    s = ingest.load(args.session, warmup=args.warmup)
    results = ab_mod.measure(s)
    if not results:
        print("esta sesion no tiene eventos de A/B (action con on/off). "
              "Corre el juego con ab = true en el perfil.", file=sys.stderr)
        return 2
    text = ab_mod.render(s, results)
    if args.report:
        with open(args.report, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
        print(f"reporte: {args.report}")
    else:
        print(text)
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        prog="gpuprobe", description="analizador offline de sesiones de gpuprobe")
    ap.add_argument("--version", action="version", version=f"gpuprobe {__version__}")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("analyze", help="candidatos y reporte de una sesion")
    p.add_argument("session")
    p.add_argument("--report", help="escribir el markdown a un archivo")
    p.add_argument("--profile", help="escribir el perfil TOML a un archivo")
    p.add_argument("--json", action="store_true", help="salida JSON")
    _add_thresholds(p)
    p.set_defaults(func=cmd_analyze)

    p = sub.add_parser("passes", help="las pasadas ordenadas por costo")
    p.add_argument("session")
    _add_thresholds(p)
    p.set_defaults(func=cmd_passes)

    p = sub.add_parser("ab", help="ganancia MEDIDA de una sesion de A/B")
    p.add_argument("session")
    p.add_argument("--report", help="escribir el markdown a un archivo")
    _add_thresholds(p)
    p.set_defaults(func=cmd_ab)

    args = ap.parse_args(argv)
    return args.func(args)
