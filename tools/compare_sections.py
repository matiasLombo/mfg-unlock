"""Compara las secciones de dos version.dll: un renombre puro deja .text,
.data y .rdata byte a byte iguales, y eso es una prueba mas fuerte que
cualquier corrida.

    python tools/compare_sections.py version.dll "<ruta al dll anterior>"

Sale 0 si .text, .data y .rdata coinciden; 1 si no.
"""
import hashlib, sys

def sections(path):
    f = open(path, "rb").read()
    e = int.from_bytes(f[0x3c:0x40], "little")
    n = int.from_bytes(f[e + 6:e + 8], "little")
    opt = int.from_bytes(f[e + 20:e + 22], "little")
    out = {}
    for i in range(n):
        s = e + 24 + opt + 40 * i
        name = f[s:s + 8].rstrip(b"\0").decode()
        raw = int.from_bytes(f[s + 20:s + 24], "little")
        sz = int.from_bytes(f[s + 16:s + 20], "little")
        out[name] = hashlib.sha256(f[raw:raw + sz]).hexdigest()[:12]
    return out

def main():
    a, b = sections(sys.argv[1]), sections(sys.argv[2])
    bad = 0
    for k in a:
        same = a[k] == b.get(k)
        if k in (".text", ".data", ".rdata") and not same:
            bad = 1
        print("%-14s %s %s %s" % (k, a[k], b.get(k), "igual" if same else "DIFIERE"))
    print("codigo identico" if not bad else "CODIGO DISTINTO")
    return bad

if __name__ == "__main__":
    sys.exit(main())
