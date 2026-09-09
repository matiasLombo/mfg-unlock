#!/usr/bin/env python3
"""Corredor del banco que solo reporta corridas donde la generacion ENGANCHO.

Por que existe: el banco entrega generacion aproximadamente 1 de cada 3 intentos,
y una corrida fallida NO parece fallida -- renderiza libre y presenta a un tope
mas bajo, lo que se lee como un multiplicador plausible. Varias conclusiones de
este proyecto salieron de corridas unicas que habian fallado en silencio.

Como se detecta una corrida fallida, medido y no supuesto:
  - target/sl.log NO tiene "interpolation state changed from disabled to enabled"
  - SetMaximumFrameLatency llega solo a 1 (una buena llega a 2)
  - nunca aparece "Achieved 'good' FC"

Y ademas, de [[verify-generation-is-running]]: con base baja, presentadas tiene
que ser mayor que renderizadas. Una corrida en el techo del sample no prueba
nada por buenos que se vean los numeros.

Esta version vivia en un scratchpad de otra sesion y se perdio. Va al repo.

    python tools/bench.py --set ota:132874 --mode 7 --intentos 9
"""
import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

LAB = Path(r"C:\Users\matia\Documents\Codex\2026-09-04"
           r"\https-github-com-matiaslombo-mfg-unlock\outputs\MFG-Lab")


def matar_colgados():
    """Un StreamlineSample.exe de un intento anterior retiene el swap chain y
    hace que el siguiente nunca consiga independent flip."""
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Get-Process StreamlineSample -ErrorAction SilentlyContinue "
                    "| Stop-Process -Force"],
                   capture_output=True)
    time.sleep(1.5)


def ultima_corrida():
    runs = sorted(LAB.glob("runs/*/"), key=lambda p: p.stat().st_mtime)
    return runs[-1] if runs else None


def leer(p):
    try:
        return p.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return ""


def evaluar(run):
    """Devuelve (engancho, motivo, datos)."""
    sl = leer(run / "target" / "sl.log")
    log = leer(run / "target" / "mfg-unlock.log")
    datos = {}

    mults = [int(m) for m in re.findall(r"counted multiplier x100 (\d+)", log)]
    if mults:
        mults.sort()
        datos["ventanas"] = len(mults)
        datos["mediana"] = mults[len(mults) // 2] / 100.0
        datos["p90"] = mults[int(len(mults) * 0.9)] / 100.0
        datos["max"] = mults[-1] / 100.0

    pres = [int(m) for m in re.findall(r"runtime PresentCount this window (\d+)", log)]
    datos["ventanas_con_contador"] = len(pres)

    copias = len(re.findall(r"sl\.dlss_g mapped", log))
    datos["copias"] = copias
    datos["descargas"] = len(re.findall(r"modulo descargado", log))
    datos["sin_sitios"] = len(re.findall(r"sitios que quedan 0", log))

    if not sl:
        return False, "no hay target/sl.log", datos
    if "interpolation state changed from disabled to enabled" not in sl:
        return False, "sl.log nunca habilito la interpolacion", datos
    if "Achieved 'good' FC" not in sl:
        return False, "sl.log nunca logro 'good' FC", datos
    lat = re.findall(r"SetMaximumFrameLatency changed from \d+ to (\d+)", sl)
    if lat and max(int(x) for x in lat) < 2:
        return False, "frame latency se quedo en 1 (no hubo independent flip)", datos
    if not mults:
        return False, "no hubo ninguna ventana con multiplicador contado", datos
    return True, "engancho", datos


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", dest="set_key", default="gtav-213-limpio")
    ap.add_argument("--mode", type=int, default=-1)
    ap.add_argument("--multiplier", type=int, default=2)
    ap.add_argument("--base-fps", type=int, default=30,
                    help="bajo a proposito: en el techo del sample una corrida "
                         "rota se ve sana")
    ap.add_argument("--duration", type=float, default=8.0)
    ap.add_argument("--intentos", type=int, default=9)
    ap.add_argument("--extra", default="", help="argumentos crudos extra")
    a = ap.parse_args()

    cmd_base = [sys.executable, "mfg_lab.py", "run", "--set", a.set_key,
                "--multiplier", str(a.multiplier), "--base-fps", str(a.base_fps),
                "--duration", str(a.duration), "--slowframe", "16000"]
    if a.mode >= 0:
        cmd_base += ["--mode", str(a.mode)]
    if a.extra:
        cmd_base += a.extra.split()

    print("banco: %s  mode=%s  mult=%s  base=%s" %
          (a.set_key, a.mode, a.multiplier, a.base_fps))
    fallidos = []
    for i in range(1, a.intentos + 1):
        matar_colgados()
        subprocess.run(cmd_base, cwd=str(LAB), capture_output=True, timeout=420)
        run = ultima_corrida()
        if run is None:
            print("  intento %d: no aparecio carpeta de corrida" % i)
            continue
        ok, motivo, d = evaluar(run)
        resumen = ""
        if "mediana" in d:
            resumen = "  mediana %.2f  p90 %.2f  max %.2f  ventanas %d" % (
                d["mediana"], d["p90"], d["max"], d["ventanas"])
        print("  intento %d: %-46s%s" % (i, motivo, resumen))
        if ok:
            print("\nCORRIDA VALIDA: %s" % run.name)
            print("  copias de sl.dlss_g: %d   descargas: %d   'sitios que quedan 0': %d"
                  % (d["copias"], d["descargas"], d["sin_sitios"]))
            print("  ventanas con contador de presentaciones: %d" % d["ventanas_con_contador"])
            print("  multiplicador contado: mediana %.2f  p90 %.2f  max %.2f (n=%d)"
                  % (d["mediana"], d["p90"], d["max"], d["ventanas"]))
            return 0
        fallidos.append(motivo)

    print("\nNINGUN INTENTO ENGANCHO en %d. No hay numero que reportar." % a.intentos)
    for m in sorted(set(fallidos)):
        print("  motivo visto: %s" % m)
    return 1


if __name__ == "__main__":
    sys.exit(main())
