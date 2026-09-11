"""Mueve VARIOS rangos de lineas de src/proxy.cpp a un header nuevo, en su
orden original, y deja UN #include en la posicion del ultimo rango. Los
rangos anteriores desaparecen de proxy.cpp; si algo entre medio los
necesitaba, se le deja una declaracion adelantada en el lugar del primero.

    python tools/extract_blocks.py "a-b,c-d,..." <header> <cabecera.txt> [fwd.txt]

fwd.txt: lineas (declaraciones adelantadas) que van donde estaba el primer
rango. Sin tocar una linea del cuerpo.
"""
import io, os, sys

ROOT = r"C:\Users\matia\dev\mfg-unlock"

def main():
    ranges = [tuple(int(x) for x in r.split("-")) for r in sys.argv[1].split(",")]
    header, cab = sys.argv[2], sys.argv[3]
    fwd = io.open(sys.argv[4], encoding="utf-8").read().rstrip("\n").split("\n") if len(sys.argv) > 4 else []
    p = os.path.join(ROOT, "src", "proxy.cpp")
    lines = io.open(p, encoding="utf-8", errors="surrogateescape").read().split("\n")
    ranges.sort()
    for a, b in ranges:
        assert 1 <= a <= b <= len(lines), (a, b)
    for (a1, b1), (a2, b2) in zip(ranges, ranges[1:]):
        assert b1 < a2, "rangos solapados"
    block = []
    for a, b in ranges:
        block += lines[a - 1:b] + [""]
    head = io.open(cab, encoding="utf-8").read().rstrip("\n").split("\n")
    hp = os.path.join(ROOT, "src", header)
    assert not os.path.exists(hp), hp
    io.open(hp, "w", encoding="utf-8", errors="surrogateescape", newline="\n").write("\n".join(head + ["#pragma once", ""] + block))
    # de atras para adelante para no mover los numeros de linea
    for i, (a, b) in enumerate(reversed(ranges)):
        es_ultimo = (i == 0)
        es_primero = (i == len(ranges) - 1)
        repl = []
        if es_ultimo:
            repl.append('#include "%s"' % header)
        if es_primero and fwd:
            repl = fwd + repl
        lines[a - 1:b] = repl
    io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="\n").write("\n".join(lines))
    print("movidas %d lineas a src/%s; proxy.cpp queda en %d" % (len(block), header, len(lines)))

if __name__ == "__main__":
    main()
