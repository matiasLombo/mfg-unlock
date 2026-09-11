"""Mueve un rango de lineas de src/proxy.cpp a un header nuevo, sin tocar
una linea del cuerpo, y deja un #include en su lugar.

    python tools/extract_block.py <desde> <hasta> <header> <cabecera.txt>

<desde>/<hasta> son lineas 1-indexadas inclusive. <cabecera.txt> es el
comentario de cabecera (que entra, que sale, de quien depende) que va al
principio del header. Se agrega #pragma once; el orden de declaracion de
proxy.cpp no cambia porque el include queda exactamente donde estaba el
bloque.
"""
import io, os, sys

ROOT = r"C:\Users\matia\dev\mfg-unlock"

def main():
    a, b, header, cab = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
    p = os.path.join(ROOT, "src", "proxy.cpp")
    lines = io.open(p, encoding="utf-8", errors="surrogateescape").read().split("\n")
    assert 1 <= a <= b <= len(lines), (a, b, len(lines))
    block = lines[a - 1:b]
    head = io.open(cab, encoding="utf-8").read().rstrip("\n").split("\n")
    out = head + ["#pragma once", ""] + block + [""]
    hp = os.path.join(ROOT, "src", header)
    assert not os.path.exists(hp), hp
    io.open(hp, "w", encoding="utf-8", errors="surrogateescape", newline="\n").write("\n".join(out))
    lines[a - 1:b] = ['#include "%s"' % header]
    io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="\n").write("\n".join(lines))
    print("movidas %d lineas a src/%s; proxy.cpp queda en %d" % (b - a + 1, header, len(lines)))

if __name__ == "__main__":
    main()
