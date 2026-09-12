"""Lo que de verdad se entrego, por tramo, en cualquier juego.

    python tools/ventanas.py <mfg-unlock.log> [--sesion N] [--todas]

Lee las ventanas de medicion ("measured: rendered fps", "runtime PresentCount
this window", "raw token calls", "hitches over 33ms") y las agrupa en tramos
que cortan "panel: mode now N" y "settings: restored mode N". Por tramo:
ventanas, ratio presentado/renderizado (mediana, p10, p90), base mediana,
presentados mediana, hitches. Ratio = PresentCount del runtime / tokens, que
es el instrumento honesto ([[measure-with-the-runtime-counter]]); nunca
"counted multiplier".

Por defecto toma la ULTIMA sesion del log (el ultimo "--- mfg-unlock
attached ---"); --sesion N toma la N-esima desde el final (1 = ultima);
--todas no corta por sesion.

Existe porque el 11/09 se escribieron tres scripts descartables para esto
(metro_win.py, metro_modes.py, un awk) y ninguno quedo.
"""
import argparse
import re
import statistics
import sys


def sesiones(lineas):
    cortes = [i for i, l in enumerate(lineas) if "--- mfg-unlock attached ---" in l]
    if not cortes:
        return [lineas]
    cortes.append(len(lineas))
    return [lineas[cortes[k]:cortes[k + 1]] for k in range(len(cortes) - 1)]


def tramos(lineas):
    """[(etiqueta, [ventana, ...])], cada ventana un dict."""
    out = []
    etiqueta = "arranque"
    ventanas = []
    cur = None
    pats = [
        ("pc", re.compile(r"runtime PresentCount this window (\d+)")),
        ("tok", re.compile(r"raw token calls (\d+)")),
        ("hit", re.compile(r"hitches over 33ms (\d+)")),
        ("lat", re.compile(r"latency sim to driver end us (\d+)")),
        ("ms", re.compile(r"window elapsed, ms (\d+)")),
        ("cv", re.compile(r"cadencia: desvio relativo x1000 (\d+)")),
        ("near", re.compile(r"off-refresh near a change x1000 (\d+)")),
        ("far", re.compile(r"off-refresh away x1000 (\d+)")),
        ("techo", re.compile(r"techo declarado a la API (\d+)")),
    ]
    re_ofp = re.compile(r"      of presents (\d+)")
    re_fps = re.compile(r"\[(\d+)ms\] measured: rendered fps (\d+)")
    re_modo = re.compile(r"panel: mode now (\d+)|settings: restored mode (\d+)")
    for l in lineas:
        m = re_modo.search(l)
        if m:
            if cur:
                ventanas.append(cur)
                cur = None
            if ventanas:
                out.append((etiqueta, ventanas))
                ventanas = []
            etiqueta = "mode " + (m.group(1) or m.group(2))
            continue
        m = re_fps.match(l)
        if m:
            if cur:
                ventanas.append(cur)
            cur = {"t": int(m.group(1)), "fps": int(m.group(2))}
            continue
        if cur is None:
            continue
        m = re_ofp.search(l)
        if m:
            # sigue a "near" o a "far", en ese orden
            cur["far_n" if "near_n" in cur else "near_n"] = int(m.group(1))
            continue
        for k, pat in pats:
            m = pat.search(l)
            if m:
                cur[k] = int(m.group(1))
    if cur:
        ventanas.append(cur)
    if ventanas:
        out.append((etiqueta, ventanas))
    return out


def resumen(etiqueta, ws):
    ws = [w for w in ws if "pc" in w and "tok" in w and w["tok"] > 0]
    if not ws:
        return "%-16s sin ventanas con PresentCount" % etiqueta
    r = sorted(w["pc"] / w["tok"] for w in ws)
    base = statistics.median(w["fps"] for w in ws)
    pres = statistics.median(w["fps"] * w["pc"] / w["tok"] for w in ws)
    hit = sum(w.get("hit", 0) for w in ws)
    lat = [w["lat"] for w in ws if "lat" in w]
    out = "%-16s ventanas %4d  ratio med %.2f (p10 %.2f p90 %.2f)  base med %4d  presentados med %4d  hitches %3d%s" % (
        etiqueta, len(ws), statistics.median(r), r[len(r) // 10], r[int(len(r) * 0.9)], base, pres, hit,
        ("  lat med %d us" % statistics.median(lat)) if lat else "")
    # lo que solo existe con nuestro hook de Present (lanzado directo, sin overlay)
    secs = sum(w.get("ms", 0) for w in ws) / 1000.0
    # cambios de la cuenta de la API: el techo declarado cambia entre ventanas
    # consecutivas (cada cambio = slDLSSGSetOptions + 100 ms de enfriamiento)
    techos = [w["techo"] for w in ws if "techo" in w]
    api = sum(1 for i in range(1, len(techos)) if techos[i] != techos[i - 1])
    bad = sum(w.get("near", 0) * w.get("near_n", 0) + w.get("far", 0) * w.get("far_n", 0) for w in ws) / 1000.0
    tot = sum(w.get("near_n", 0) + w.get("far_n", 0) for w in ws)
    cv = [w["cv"] for w in ws if "cv" in w]
    if tot > 0 or api > 0:
        out += "\n%16s cambios API %d (%.2f/s)  fuera de cadencia %.1f%%  desvio cadencia med %.1f%%" % (
            "", api, api / secs if secs > 0 else 0.0, 100.0 * bad / tot if tot else 0.0,
            statistics.median(cv) / 10.0 if cv else 0.0)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--sesion", type=int, default=1, help="1 = ultima")
    ap.add_argument("--todas", action="store_true")
    a = ap.parse_args()
    lineas = open(a.log, encoding="utf-8", errors="replace").read().splitlines()
    if not a.todas:
        s = sesiones(lineas)
        if a.sesion > len(s):
            print("solo hay %d sesiones" % len(s))
            return 1
        lineas = s[-a.sesion]
        print("sesion %d de %d (%d lineas)" % (len(s) - a.sesion + 1, len(s), len(lineas)))
    exc = sum(1 for l in lineas if "EXCEPCION ----" in l)
    inv = sum(1 for l in lineas if "INVARIANTE" in l)
    print("excepciones %d, INVARIANTE %d" % (exc, inv))
    for etiqueta, ws in tramos(lineas):
        print(resumen(etiqueta, ws))
    return 0


if __name__ == "__main__":
    sys.exit(main())
