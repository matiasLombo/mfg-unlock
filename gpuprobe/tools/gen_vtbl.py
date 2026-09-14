"""Genera gpuprobe/d3d12/vtbl.h: los indices de vtable de las interfaces COM
que gpuprobe engancha, sacados del header de verdad.

    python3 gpuprobe/tools/gen_vtbl.py [ruta a los headers]

Por que no escribirlos a mano: un indice equivocado no falla al compilar ni al
enganchar -- llama a OTRO metodo de la interfaz con los argumentos de este, y
el juego se cae en un lugar que no tiene nada que ver. Es el peor error posible
de este modulo y el mas facil de cometer.

Los headers de D3D12/DXGI traen, ademas de las clases de C++, la tabla de
funciones de la API de C (ID3D12DeviceVtbl y companiaa). Esa tabla ES la
vtable, en orden, incluyendo lo heredado. Contar sus miembros da el indice
exacto sin suponer nada.

El resultado se commitea: los indices de una vtable COM no pueden cambiar sin
romper toda la ABI de Windows, asi que regenerarlo es para agregar interfaces,
no para seguirle el paso a una SDK.
"""

import os
import re
import sys

HEADERS = ["d3d12.h", "dxgi.h", "dxgi1_2.h", "dxgi1_3.h", "dxgi1_4.h", "dxgi1_5.h"]

# Interfaz -> nombre del enum que se emite.
WANTED = {
    "ID3D12Device": "Device",
    "ID3D12CommandQueue": "CommandQueue",
    "ID3D12GraphicsCommandList": "GraphicsCommandList",
    "IDXGIFactory": "Factory",
    "IDXGIFactory2": "Factory2",
    "IDXGISwapChain": "SwapChain",
    "IDXGISwapChain1": "SwapChain1",
    "IDXGISwapChain3": "SwapChain3",
}

SEARCH = [
    "/usr/share/mingw-w64/include",
    "/usr/x86_64-w64-mingw32/include",
]


def find_header(name, extra):
    for d in ([extra] if extra else []) + SEARCH:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return None


def parse(path):
    """Devuelve {interfaz: [metodos en orden de vtable]}."""
    text = open(path, "r", errors="replace").read()
    out = {}
    # La tabla de C: "typedef struct ID3D12DeviceVtbl { ... } ID3D12DeviceVtbl;"
    for m in re.finditer(r"typedef struct (\w+)Vtbl\s*\{(.*?)\}\s*\1Vtbl;", text,
                         re.S):
        iface, body = m.group(1), m.group(2)
        methods = []
        # Cada entrada es un puntero a funcion: "... ( STDMETHODCALLTYPE *Nombre )("
        for mm in re.finditer(r"\*\s*(\w+)\s*\)\s*\(", body):
            methods.append(mm.group(1))
        if methods:
            out[iface] = methods
    return out


def main(extra=None):
    tables = {}
    missing = []
    for h in HEADERS:
        p = find_header(h, extra)
        if not p:
            missing.append(h)
            continue
        tables.update(parse(p))
    if missing:
        print(f"headers no encontrados: {missing}", file=sys.stderr)

    lines = [
        "// vtbl.h -- indices de vtable de las interfaces COM que gpuprobe",
        "// engancha. GENERADO por tools/gen_vtbl.py desde los headers de la SDK:",
        "// no editar a mano.",
        "//",
        "// Un indice equivocado no falla al compilar ni al enganchar: llama a OTRO",
        "// metodo de la interfaz con los argumentos de este, y el juego se cae en un",
        "// lugar que no tiene nada que ver. Por eso salen del header y no de una",
        "// tabla escrita a mano.",
        "//",
        "// Se commitea: los indices de una vtable COM no pueden cambiar sin romper la",
        "// ABI de Windows entera. Regenerar es para agregar interfaces, no para",
        "// seguirle el paso a una SDK nueva.",
        "#pragma once",
        "",
        "namespace gp {",
        "namespace vtbl {",
        "",
    ]
    for iface, enum in WANTED.items():
        methods = tables.get(iface)
        if not methods:
            print(f"AVISO: no encontre la vtable de {iface}", file=sys.stderr)
            continue
        lines.append(f"// {iface} -- {len(methods)} metodos")
        lines.append(f"namespace {enum} {{")
        seen = {}
        for i, name in enumerate(methods):
            # Los overloads (SetPrivateData y companiaa) no nos interesan, pero
            # si aparece un nombre repetido se numera para no perder el indice.
            key = name if name not in seen else f"{name}{seen[name]}"
            seen[name] = seen.get(name, 1) + 1
            lines.append(f"    inline constexpr int {key} = {i};")
        lines.append("}")
        lines.append("")
    lines += ["}  // namespace vtbl", "}  // namespace gp", ""]

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "d3d12",
                       "vtbl.h")
    with open(out, "w") as fh:
        fh.write("\n".join(lines))
    print(f"escrito {os.path.normpath(out)}")
    for iface, enum in WANTED.items():
        if iface in tables:
            print(f"  {iface}: {len(tables[iface])} metodos")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else None)
