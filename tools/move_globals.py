"""Reparte las globales g_* segun quien las usa (tools/globals_report.py):

  - usada por un solo modulo (header) y declarada en proxy.cpp -> al principio
    de ese header;
  - usada por varios archivos -> src/state.h, con un comentario por variable
    que dice quien escribe y quien lee.

Mueve la linea de declaracion (y el bloque de comentarios pegado justo
arriba) sin cambiarla. Antes mueve a state.h los typedef PFN_*, kMaxSites y
struct Sample que las declaraciones necesitan. Lo que no se pueda mover se
lista al final.

    python tools/move_globals.py
"""
import io, os, re, json, subprocess, sys

ROOT = r"C:\Users\matia\dev\mfg-unlock"
SRC = os.path.join(ROOT, "src")
BASIC = re.compile(r'^(static |volatile |const |extern )*(unsigned |signed )?(LONG|ULONG|LONGLONG|ULONGLONG|DWORD|HWND|HANDLE|HMODULE|int|long|double|float|bool|char|wchar_t|unsigned|size_t|UINT|IDXGISwapChain|IDXGIFactory|void|Phase|Sample|config::Settings|controller::State|scheduler::State|PFN_[A-Za-z0-9_]+)\b')
PRELUDE = [
    r"typedef unsigned (*PFN_slSetMarker)(unsigned, void *);",
    r"typedef unsigned (*PFN_slDLSSGGetState)(const void *, void *, const void *);",
    r"typedef unsigned (*PFN_slDLSSGSetOptions)(const void *, const void *);",
    r"typedef unsigned (*PFN_slGetFeatureFunction)(unsigned, const char *, void *&);",
    r"typedef unsigned (*PFN_slInit)(void *, unsigned long long);",
    r"typedef unsigned (*PFN_slGetNewFrameToken)(void *&, const unsigned *);",
    r"typedef unsigned (*PFN_slReflexGetState)(void *);",
    r"typedef HRESULT(STDMETHODCALLTYPE *PFN_DXGIPresent)(IDXGISwapChain *, UINT, UINT);",
    r"typedef HRESULT (STDMETHODCALLTYPE *PFN_SMFL)(IUnknown *, UINT);",
    r"typedef int(__stdcall *PFN_Present)(void *, const void *);",
    r"static const int kMaxSites = 4;",
    r"struct Sample { long long qpc; unsigned img; int meter; unsigned char src; };",
]

def read(f): return io.open(os.path.join(SRC, f), encoding="utf-8", errors="surrogateescape").read()
def write(f, s): io.open(os.path.join(SRC, f), "w", encoding="utf-8", errors="surrogateescape", newline="\n").write(s)

def take_decl(lines, ln):
    i = ln - 1
    j = i
    while ";" not in lines[j] and j < len(lines) - 1: j += 1
    k = i
    while k > 0 and lines[k - 1].startswith("//"): k -= 1
    return k, j

def main():
    # 0) el preludio: sacar cada linea de donde este y ponerla en state.h
    files = ["proxy.cpp"] + sorted(f for f in os.listdir(SRC) if f.endswith(".h") and f not in ("cubins.h", "state.h"))
    texts = {f: read(f) for f in files}
    prel = []
    for line in PRELUDE:
        found = False
        for f in files:
            if line + "\n" in texts[f]:
                texts[f] = texts[f].replace(line + "\n", "", 1); found = True; prel.append(line); break
        if not found: print("preludio no encontrado:", line)
    for f in files: write(f, texts[f])
    st = read("state.h")
    st = st.rstrip("\n") + "\n\n// Tipos que las declaraciones de abajo necesitan.\n" + "\n".join(prel) + "\n"
    write("state.h", st)

    rep = json.loads(subprocess.check_output([sys.executable, os.path.join(ROOT, "tools", "globals_report.py"), "--json"]).decode())
    plan = {}
    skipped = []
    for g, info in sorted(rep.items()):
        decls = info["decl"]
        # la definicion es la que no es extern
        defs = []
        for (f, ln) in decls:
            l = read(f).split("\n")[ln - 1]
            if not l.lstrip().startswith("extern"): defs.append((f, ln))
        if len(defs) != 1: skipped.append((g, "definiciones: %d" % len(defs))); continue
        (df, dl) = defs[0]
        if df == "state.h": continue
        uses = info["uses"]
        users = [f for f in uses if not (f == df and uses[f]["n"] == 1)]
        if len(users) == 0: continue
        lines = read(df).split("\n")
        k, j = take_decl(lines, dl)
        decl_text = "\n".join(lines[k:j + 1])
        first = lines[dl - 1]
        if len(users) == 1:
            dest = users[0]
            if dest == df or dest == "proxy.cpp": continue
            plan.setdefault(df, []).append((k, j, dest, decl_text, g))
        else:
            if not BASIC.match(first.strip()):
                skipped.append((g, "tipo: " + first.strip()[:70])); continue
            w = sorted(f for f in uses if uses[f]["w"] and not (f == df and uses[f]["n"] == 1))
            r = sorted(f for f in uses if f not in w and not (f == df and uses[f]["n"] == 1))
            com = "// escribe: %s; lee: %s" % (", ".join(w) or "-", ", ".join(r) or "-")
            plan.setdefault(df, []).append((k, j, "state.h", com + "\n" + decl_text, g))
    appended = {}
    for f, items in plan.items():
        lines = read(f).split("\n")
        for k, j, dest, text, g in sorted(items, key=lambda x: -x[0]):
            del lines[k:j + 1]
            appended.setdefault(dest, []).append((g, text))
        write(f, "\n".join(lines))
    for dest, items in appended.items():
        if dest == "state.h":
            s = read("state.h").rstrip("\n") + "\n\n" + "\n\n".join(t for g, t in sorted(items)) + "\n"
            write("state.h", s)
        else:
            s = read(dest)
            a = "#pragma once\n"
            block = "\n// Globales que solo usa este modulo (movidas de proxy.cpp).\n" + "\n".join(t for g, t in sorted(items)) + "\n"
            write(dest, s.replace(a, a + block, 1))
    for f, items in sorted(plan.items()): print("%-16s mueve %d" % (f, len(items)))
    for g, why in skipped: print("NO MOVIDA", g, why)

if __name__ == "__main__":
    main()
