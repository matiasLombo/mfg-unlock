"""Resume una corrida de Cyberpunk y la compara con la anterior.

    python tools/check_run.py <log> [<log anterior>]

Imprime: excepciones, INVARIANTE, conteos de sitios, ventanas de juego,
ratio entregado (mediana/p10/p90) y base. Con el log anterior, marca
FUERA si el ratio mediano se mueve mas de 0.3, si cambia un conteo de
sitios, o si aparece una excepcion o un INVARIANTE. Sale 1 si hay FUERA.
"""
import re, sys, statistics

def resumen(path):
    log = open(path, encoding="utf-8", errors="replace").read()
    r = {}
    r["excepciones"] = log.count("EXCEPCION")
    r["invariantes"] = log.count("INVARIANTE ")
    r["veredicto"] = "ACTIVO" if "VEREDICTO ACTIVO" in log else ("PASIVO" if "VEREDICTO PASIVO" in log else "-")
    sitios = {}
    for k, pat in [("gates", r"gates rewritten: (\d+)"), ("snippet_max", r"tope del snippet subido a 6, sitios: (\d+)"),
                   ("plugin_cap", r"tope del plugin subido a 6, sitios: (\d+)"), ("subframe", r"sub-frame count made writable, sites: (\d+)"),
                   ("cpu_pacer", r"CPU pacer enabled, sites: (\d+)")]:
        m = re.findall(pat, log)
        sitios[k] = sorted(set(int(x) for x in m))
    sitios["work_item"] = log.count("work item count is ours")
    sitios["gen_flag"] = log.count("generation flag redirected")
    r["sitios"] = sitios
    blocks = re.findall(r"measured: rendered fps (\d+)(.*?)window elapsed, ms (\d+)", log, re.S)
    rows = []
    for rend, body, el in blocks:
        b = re.search(r"base por Reflex (\d+)", body); p = re.search(r"runtime PresentCount this window (\d+)", body)
        if not b or not p: continue
        b = int(b.group(1)); p = int(p.group(1)); el = int(el)
        if 20 <= b <= 140 and el > 0:
            rows.append((b, p * 1000.0 / el))
    r["ventanas"] = len(blocks); r["juego"] = len(rows)
    if rows:
        rs = sorted(x[1] / x[0] for x in rows)
        r["ratio"] = (statistics.median(rs), rs[len(rs) // 10], rs[len(rs) * 9 // 10])
        r["base"] = statistics.median(x[0] for x in rows)
        r["presentadas"] = statistics.median(x[1] for x in rows)
    else:
        r["ratio"] = (0, 0, 0); r["base"] = 0; r["presentadas"] = 0
    return r

def mostrar(nombre, r):
    print("%s: veredicto %s, excepciones %d, INVARIANTE %d, ventanas %d (juego %d)" %
          (nombre, r["veredicto"], r["excepciones"], r["invariantes"], r["ventanas"], r["juego"]))
    print("  ratio mediana %.2f (p10 %.2f, p90 %.2f), base %d, presentadas/s %d" %
          (r["ratio"][0], r["ratio"][1], r["ratio"][2], r["base"], r["presentadas"]))
    print("  sitios:", r["sitios"])

def main():
    a = resumen(sys.argv[1]); mostrar("esta", a)
    fuera = a["excepciones"] > 0 or a["invariantes"] > 0 or a["veredicto"] != "ACTIVO"
    if len(sys.argv) > 2:
        b = resumen(sys.argv[2]); mostrar("anterior", b)
        if abs(a["ratio"][0] - b["ratio"][0]) > 0.3: print("  FUERA: el ratio mediano se movio"); fuera = True
        if a["sitios"] != b["sitios"]: print("  FUERA: cambiaron los conteos de sitios"); fuera = True
    print("FUERA" if fuera else "ok")
    return 1 if fuera else 0

if __name__ == "__main__":
    sys.exit(main())
