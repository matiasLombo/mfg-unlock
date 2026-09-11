#!/usr/bin/env python3
"""Corre TODOS los modos que importan y reporta lo que de verdad se entrego.

Por que existe: cada arreglo se validaba con una corrida del banco en el modo que
estaba mirando en ese momento, y despues aparecia roto otro modo o otro juego.
No hay ramas por juego en el codigo -- pero las CONDICIONES se escribian mirando
el estado de un juego, que da el mismo resultado. El caso concreto: el piso de
DYNAMIC condicionado a `g_dyn_target < 100`, cierto en GTA V y en el banco, falso
en Halo porque tenia `target 600` guardado. El piso no disparo nunca ahi.

Dos reglas que este script aplica y que me costaron el dia:

  1. El multiplicador se lee como presentadas/renderizadas, NO con
     "counted multiplier". Ese instrumento miente a 6X: dio 6.00/6.00/6.00
     donde la razon real era 5.72, y 9.57 de maximo donde el techo es 6.

  2. El banco engancha ~1 de cada 3 y una corrida fallida no parece fallida.
     bench.py ya repite; aca ademas se marca lo que quede fuera de banda para
     que nadie lo lea de reojo.

    python tools/regresion.py
    python tools/regresion.py --duration 15      # mas rapido, mas ruidoso
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

RUNS = Path(r"C:\Users\matia\Documents\Codex\2026-09-04"
            r"\https-github-com-matiaslombo-mfg-unlock\outputs\MFG-Lab\runs")

# nombre, argumentos de bench.py, minimo aceptable, maximo razonable
CASOS = [
    ("2X",           ["--mode", "2"],                          1.90, 2.10),
    ("4X",           ["--mode", "4"],                          3.80, 4.20),
    ("6X",           ["--mode", "6"],                          5.60, 6.10),
    ("DYNAMIC 165",  ["--extra", "--dynfps 165"],              2.00, 6.10),
    ("frac 2.55x",   ["--extra", "--fractional 255"],          2.40, 2.70),
]


def suma(log, patron):
    return sum(int(m) for m in re.findall(patron + r" (\d+)", log))


def correr(caso, args, duracion, set_key):
    cmd = [sys.executable, "tools/bench.py", "--set", set_key,
           "--duration", str(duracion), "--intentos", "3"] + args
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=1800).stdout
    m = re.search(r"CORRIDA VALIDA: (\S+)", out)
    if not m:
        return None, "ninguna corrida engancho", 0
    t = RUNS / m.group(1) / "target"
    log = (t / "mfg-unlock.log").read_text(encoding="utf-8", errors="replace")
    pres = suma(log, r"runtime PresentCount this window")
    rend = suma(log, r"raw token calls")
    rotos = log.count("INVARIANTE ")
    return (pres / rend if rend else 0.0), m.group(1), rotos


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=int, default=30)
    ap.add_argument("--set", dest="set_key", default="bundled-2.12.0")
    a = ap.parse_args()

    print("regresion: set %s, %ds por caso\n" % (a.set_key, a.duration))
    print("%-14s %10s %8s %11s  %s" % ("caso", "entregado", "banda", "invariante", "veredicto"))
    malos = 0
    for nombre, args, lo, hi in CASOS:
        ratio, corrida, rotos = correr(nombre, args, a.duration, a.set_key)
        if ratio is None:
            print("%-14s %10s %8s %11s  %s" % (nombre, "-", "-", "-", corrida))
            malos += 1
            continue
        ok = lo <= ratio <= hi and rotos == 0
        if not ok:
            malos += 1
        print("%-14s %10.2f %8s %11s  %s" % (
            nombre, ratio, "%.1f-%.1f" % (lo, hi),
            "ok" if rotos == 0 else "ROTO x%d" % rotos,
            "ok" if ok else "<-- FUERA"))
    print()
    if malos:
        print("%d de %d casos fuera de banda. NO instalar." % (malos, len(CASOS)))
    else:
        print("los %d casos en banda." % len(CASOS))
    # El banco descarta rapido y no decide. El criterio sigue siendo el juego.
    print("Esto descarta; no aprueba. Falta la vuelta en Halo, GTA V y Cyberpunk.")
    return 1 if malos else 0


if __name__ == "__main__":
    sys.exit(main())
