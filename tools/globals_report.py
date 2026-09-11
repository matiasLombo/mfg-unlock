"""Quien declara, quien escribe y quien lee cada global g_* de src/.

    python tools/globals_report.py            # tabla
    python tools/globals_report.py --json     # para tools que mueven

Cuenta solo codigo (sin cadenas ni comentarios). "escribe" es una
asignacion, ++/--, op= o &g_x (Interlocked*); lo demas es "lee".
"""
import io, os, re, sys, json, collections

ROOT = r"C:\Users\matia\dev\mfg-unlock"
FILES = ["proxy.cpp"] + sorted(f for f in os.listdir(os.path.join(ROOT, "src")) if f.endswith(".h") and f != "cubins.h")
SEG = re.compile(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/', re.S)
DECL = re.compile(r'^(?:static |volatile |const |extern )*[A-Za-z_][A-Za-z_0-9:<>\* ]*?\s\*?\s*(g_[A-Za-z_0-9]+)\s*(?:\[[^\]]*\])*\s*(?:=|;|\{)', re.M)

def code_of(path):
    s = io.open(path, encoding="utf-8", errors="replace").read()
    return SEG.sub(lambda m: " " * len(m.group(0)), s)

def main():
    codes = {f: code_of(os.path.join(ROOT, "src", f)) for f in FILES}
    decl = {}
    for f, c in codes.items():
        for m in DECL.finditer(c):
            line = c[:m.start()].count("\n") + 1
            decl.setdefault(m.group(1), []).append((f, line))
    rep = {}
    for g, where in decl.items():
        uses = {}
        for f, c in codes.items():
            n = len(re.findall(r'\b' + g + r'\b', c))
            if n == 0: continue
            w = len(re.findall(r'\b' + g + r'\b\s*(?:=[^=]|\+\+|--|[-+*/|&]=)|\+\+\s*' + g + r'\b|&' + g + r'\b', c))
            uses[f] = {"n": n, "w": w}
        rep[g] = {"decl": where, "uses": uses}
    if "--json" in sys.argv:
        print(json.dumps(rep, indent=1)); return
    for g in sorted(rep):
        d = rep[g]["decl"]; u = rep[g]["uses"]
        files = [f for f in u if not (len(d) == 1 and f == d[0][0] and u[f]["n"] == 1)]
        kind = "solo-%s" % files[0] if len(files) == 1 else ("compartida(%d)" % len(files) if files else "sin-uso")
        print("%-28s decl %-16s %-22s %s" % (g, "%s:%d" % d[0], kind,
              " ".join("%s:%d%s" % (f, u[f]["n"], "w" if u[f]["w"] else "") for f in u)))

if __name__ == "__main__":
    main()
