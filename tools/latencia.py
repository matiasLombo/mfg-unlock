"""Latencia de render por corrida, del reporte de Reflex en el log.

    python tools/latencia.py <log> [<log> ...]

Por cada log (una corrida) saca la mediana, p90 y max de "latency sim to
driver end us" -- sim -> driver end, la latencia de render que reporta Reflex
([[lab-measures-latency]]) -- y el ratio y presentados medios para poner el
numero en contexto. No es click-to-photon: no hay timestamp del input.

Pensado para lat_modes.ps1 (una corrida por modo): pasarle los
mfg-unlock.lat-mN.log y da la tabla latencia-por-modo.
"""
import re
import statistics
import sys


def resumen(path):
    lat, ratios, pres = [], [], []
    tok = pc = None
    for l in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"latency sim to driver end us (\d+)", l)
        if m:
            lat.append(int(m.group(1)))
        m = re.search(r"runtime PresentCount this window (\d+)", l)
        if m:
            pc = int(m.group(1))
        m = re.search(r"raw token calls (\d+)", l)
        if m:
            tok = int(m.group(1))
            if pc is not None and tok > 0:
                ratios.append(pc / tok)
        m = re.match(r"\[\d+ms\] measured: rendered fps (\d+)", l)
        if m and ratios:
            pres.append(int(m.group(1)) * ratios[-1])
    if not lat:
        return path, None
    lat.sort()
    return path, {
        "n": len(lat),
        "med": statistics.median(lat),
        "p90": lat[int(len(lat) * 0.9)],
        "max": lat[-1],
        "ratio": statistics.median(ratios) if ratios else 0,
        "pres": statistics.median(pres) if pres else 0,
    }


def main():
    print("%-22s %5s  %8s %8s %8s   %6s %6s" % (
        "log", "n", "lat med", "lat p90", "lat max", "ratio", "pres/s"))
    for p in sys.argv[1:]:
        name, r = resumen(p)
        short = name.replace("\\", "/").split("/")[-1]
        if r is None:
            print("%-22s  sin datos de Reflex (activo?)" % short)
            continue
        print("%-22s %5d  %6d us %6d us %6d us   %5.2f  %5d" % (
            short, r["n"], r["med"], r["p90"], r["max"], r["ratio"], r["pres"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
