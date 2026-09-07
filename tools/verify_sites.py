"""Verifica, sin ejecutar nada, que cada parche encuentra su sitio en un
sl.dlss_g.dll dado.

La dll parchea buscando patrones de bytes en .text. Este script corre los
mismos patrones sobre el archivo, asi que contesta antes de abrir un juego si
una version nueva movio algo. Diez segundos contra descubrirlo jugando.

    python tools/verify_sites.py <sl.dlss_g.dll> [otro.dll ...]

Sin argumentos usa los dos sets del banco. Lo que importa de cada fila es que
el sitio sea UNICO: cero significa que el parche no aplica, y mas de uno que el
patron dejo de identificar un solo lugar y parchear seria adivinar.
"""
import struct
import sys
from pathlib import Path

BANCO = Path(r"C:\Users\matia\Documents\Codex\2026-09-04"
             r"\https-github-com-matiaslombo-mfg-unlock\outputs\MFG-Lab\sdk-sets")


def text_section(data):
    """La seccion .text, recorrida igual que en tiempo de ejecucion."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("no es un PE")
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    first = pe + 24 + opt
    for i in range(nsec):
        off = first + 40 * i
        if data[off:off + 8].rstrip(b"\0") == b".text":
            vsize = struct.unpack_from("<I", data, off + 8)[0]
            rsize = struct.unpack_from("<I", data, off + 16)[0]
            raw = struct.unpack_from("<I", data, off + 20)[0]
            return data[raw:raw + min(vsize, rsize)]
    raise ValueError("sin .text")


def u32(t, i):
    return struct.unpack_from("<I", t, i)[0]


# --- un predicado por parche, copiado de src/proxy.cpp ------------------------

def work_item_count(t, i):
    # mov eax,[rdx+4] / mov r8d,0xC0 / mov [rcx+4],eax
    return (t[i] == 0x8B and t[i+1] == 0x42 and t[i+2] == 0x04 and
            t[i+3] == 0x41 and t[i+4] == 0xB8 and t[i+5] == 0xC0 and
            not t[i+6] and not t[i+7] and not t[i+8] and
            t[i+9] == 0x89 and t[i+10] == 0x41 and t[i+11] == 0x04)


def frame_latency(t, i):
    # mov rax,[rcx] / lea r8,[rbx+0x40] / mov edx,[rsi+0x60] / call [rax+0x60]
    return (t[i] == 0x48 and t[i+1] == 0x8B and t[i+2] == 0x01 and
            t[i+3] == 0x4C and t[i+4] == 0x8D and t[i+5] == 0x43 and t[i+6] == 0x40 and
            t[i+7] == 0x8B and t[i+8] == 0x56 and t[i+9] == 0x60 and
            t[i+10] == 0xFF and t[i+11] == 0x50 and t[i+12] == 0x60)


def index_count(t, i):
    # test r13,r13 / je / mov eax,[r13+4] / lea rcx,[rcx*2+1]
    return (t[i] == 0x4D and t[i+1] == 0x85 and t[i+2] == 0xED and
            t[i+3] == 0x74 and
            t[i+5] == 0x41 and t[i+6] == 0x8B and t[i+7] == 0x45 and t[i+8] == 0x04 and
            t[i+9] == 0x48 and t[i+10] == 0x8D and t[i+11] == 0x0C and
            t[i+12] == 0x4D and t[i+13] == 0x01)


def generation_flag(t, i):
    # mov rcx,r14 / call rel32 / mov [r14+0x45xx],al
    return (t[i] == 0x49 and t[i+1] == 0x8B and t[i+2] == 0xCE and
            t[i+3] == 0xE8 and
            t[i+8] == 0x41 and t[i+9] == 0x88 and t[i+10] == 0x86 and
            t[i+12] == 0x45 and t[i+13] == 0x00 and t[i+14] == 0x00)


def subframe_loop(t, i):
    # inc edi / cmp edi,[r13+4] / jb rel32   -- el bucle de sub-frames
    return (t[i] == 0xFF and t[i+1] == 0xC7 and
            t[i+2] == 0x41 and t[i+3] == 0x3B and t[i+4] == 0x7D and t[i+5] == 0x04 and
            t[i+6] == 0x0F and t[i+7] == 0x82)


def metering_flagcalc(t, i):
    # cmp edx,0x1E / setae r9b / cmp byte [r14+off],0 / sete al
    return (t[i] == 0x83 and t[i+1] == 0xFA and t[i+2] == 0x1E and
            t[i+3] == 0x41 and t[i+4] == 0x0F and t[i+5] == 0x93 and
            t[i+7] == 0x41 and t[i+8] == 0x80 and (t[i+9] & 0xC7) == 0x86 and
            t[i+14] == 0x00 and
            t[i+15] == 0x0F and t[i+16] == 0x94 and t[i+17] == 0xC0)


def cpu_pacer(t, i):
    # movabs rax, 100.0  -- el enfriamiento que el pacer escribe en el contexto
    return t[i:i+10] == bytes([0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0x59, 0x40])


SIMPLES = [
    ("work item count", work_item_count, 12, "el bucle: sin esto no hay fraccionario"),
    ("frame latency", frame_latency, 13, "SetMaximumFrameLatency, para fijarla"),
    ("index count", index_count, 14, "el indice de presentacion"),
    ("generation flag", generation_flag, 15, "los frames de cuenta cero presentan igual"),
    ("subframe loop", subframe_loop, 8, "el bucle de sub-frames"),
    ("metering flag calc", metering_flagcalc, 18, "de aca sale el offset del campo"),
    ("cpu pacer", cpu_pacer, 10, "el enfriamiento de 100 ms"),
]


def metering_field(t):
    """El offset del campo, derivado del binario y no hardcodeado."""
    for i in range(len(t) - 20):
        if metering_flagcalc(t, i):
            return u32(t, i + 10)
    return None


def revisar(path):
    data = Path(path).read_bytes()
    t = text_section(data)
    filas = []
    for nombre, pred, ancho, nota in SIMPLES:
        n = sum(1 for i in range(len(t) - ancho) if pred(t, i))
        filas.append((nombre, n, nota))
    campo = metering_field(t)
    if campo is None:
        filas.append(("metering store", 0, "sin campo, no se puede buscar"))
    else:
        imm = sum(1 for i in range(len(t) - 13)
                  if t[i] == 0xC6 and t[i+1] == 0x83 and u32(t, i+2) == campo and
                  t[i+6] == 0x00 and t[i+7] == 0x80 and t[i+8] == 0x3D)
        reg = sum(1 for i in range(len(t) - 7)
                  if t[i] == 0x40 and t[i+1] == 0x88 and (t[i+2] & 0xC7) == 0x83 and
                  u32(t, i+3) == campo)
        # La dll prueba el inmediato y si no cae usa el registro: alcanza con uno.
        filas.append(("metering store", imm + reg,
                      "inmediato %d + registro %d, campo 0x%x" % (imm, reg, campo)))
    return filas


def main(argv):
    objetivos = argv[1:]
    if not objetivos:
        objetivos = [str(BANCO / "bundled-2.12.0" / "sl.dlss_g.dll"),
                     str(BANCO / "gtav-213-limpio" / "sl.dlss_g.dll")]
    malo = False
    for p in objetivos:
        if not Path(p).is_file():
            print("  no existe: %s" % p)
            malo = True
            continue
        print("=== %s" % Path(p).name)
        print("    %s  (%d bytes)" % (p, Path(p).stat().st_size))
        for nombre, n, nota in revisar(p):
            estado = "ok" if n == 1 else ("NO APLICA" if n == 0 else "AMBIGUO")
            if n != 1:
                malo = True
            print("      %-20s %2d sitio(s)  %-10s %s" % (nombre, n, estado, nota))
        print()
    print("  " + ("HAY ALGO QUE REVISAR: una fila sin exactamente un sitio."
                  if malo else
                  "Todo en orden: cada parche encuentra exactamente un sitio."))
    return 1 if malo else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
