#!/usr/bin/env python3
"""Rebuild three frame-generation kernels for Ada from the Blackwell PTX.

The DLSS-G snippet ships, for each of its 31 framework kernels, PTX compiled for
compute_120 *and* for compute_89 -- and they are not the same source. Ada's runs
the boolean work through a couple of hundred 16-bit predicates that Blackwell's
does without. Nothing in the Blackwell PTX is Blackwell-only: all 31 assemble for
sm_89 unchanged. Ada was simply given the worse of the two.

Three of them produce a cubin that still fits the space the original occupies,
which is what makes an in-memory swap possible:

    Shared_Prev2Curr/Curr2Prev   motion-vector estimate   2312 -> 2160 instr
    Shared_PackedInpaintDynamic  inpaint                   904 ->  648
    Shared_NeedsInpainting       inpaint decision          752 ->  576

Same algorithm either way -- identical shared-memory layout, identical access
offsets -- so this is a speed change, not an image change.

Nothing NVIDIA-derived is redistributed: this reads the snippet already installed
on the machine and writes src/cubins.h next to it.

Needs ptxas and nvdisasm, which pip provides without a full CUDA toolkit:

    pip install nvidia-cuda-nvcc nvidia-cuda-nvdisasm nvidia-cuda-cuobjdump

Usage:
    python tools/rebuild_cubins.py [path-to-snippet]

With no argument it finds the newest snippet in NVIDIA's OTA cache, which is the
one NGX actually loads -- *not* the copy in the game folder. Confirm against
sl.log: "NGXSecureLoadFeature ... snippet: <path> version: <ver>".
"""

import glob
import os
import re
import struct
import subprocess
import sys
import tempfile

# The kernels worth replacing, keyed by a fingerprint of NVIDIA's own build:
# (.text size, .nv.shared size, register count). That triple is unique across
# all 31 framework kernels, so no address is ever hardcoded and a snippet update
# that changes a kernel simply stops matching instead of being patched wrongly.
WANTED = {
    (36992, 7776, 39): "mvec estimate (Prev2Curr/Curr2Prev)",
    (14464, 3920, 23): "inpaint (PackedInpaintDynamic)",
    (12032,  784, 48): "inpaint decision (NeedsInpainting)",
}

OTA = r"C:\ProgramData\NVIDIA\NGX\models\dlssg\versions\*\files\160_*.bin"


def tool(name):
    for root in (os.path.join(os.environ.get("APPDATA", ""), "Python"),
                 sys.prefix, os.path.expanduser("~")):
        for hit in glob.glob(os.path.join(root, "**", "nvidia", "cu*", "bin", name),
                             recursive=True):
            return hit
    from shutil import which
    found = which(name)
    if found:
        return found
    sys.exit(f"{name} not found. pip install nvidia-cuda-nvcc nvidia-cuda-nvdisasm "
             f"nvidia-cuda-cuobjdump")


def sections(path):
    """(virtual addr, size, file offset, name) for each PE section."""
    d = open(path, "rb").read()
    lfanew = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, lfanew + 6)[0]
    opt = struct.unpack_from("<H", d, lfanew + 20)[0]
    first = lfanew + 24 + opt
    out = []
    for i in range(nsec):
        s = first + i * 40
        name = d[s:s + 8].rstrip(b"\0").decode("ascii", "replace")
        vsz, va, rsz, ptr = struct.unpack_from("<IIII", d, s + 8)
        out.append((va, max(vsz, rsz), ptr, name))
    return d, out


def fatbins(d, secs):
    """Every fatbin in .data, as (file offset, end offset)."""
    for va, size, ptr, name in secs:
        if name != ".data":
            continue
        blob = d[ptr:ptr + size]
        for m in re.finditer(re.escape(struct.pack("<I", 0xBA55ED50)), blob):
            o = m.start()
            hsz = struct.unpack_from("<H", blob, o + 6)[0]
            fsz = struct.unpack_from("<Q", blob, o + 8)[0]
            if hsz != 16 or not (0 < fsz < 4 << 20) or o + 16 + fsz > len(blob):
                continue
            yield ptr + o, ptr + o + 16 + fsz


def entries(d, start, end):
    """Fatbin entries: (kind, sm, payload offset, payload size, compressed size)."""
    p = start + 16
    while p + 32 <= end:
        kind = struct.unpack_from("<H", d, p)[0]
        ehsz = struct.unpack_from("<I", d, p + 4)[0]
        if not (64 <= ehsz <= 256):
            return
        psz = struct.unpack_from("<Q", d, p + 8)[0]
        csz = struct.unpack_from("<Q", d, p + 16)[0]
        sm = struct.unpack_from("<I", d, p + 28)[0]
        if p + ehsz + psz > end:
            return
        yield kind, sm, p + ehsz, psz, csz
        p += ehsz + psz


def fingerprint(b):
    """(text size, shared size, registers) of a cubin, or None."""
    if b[:4] != b"\x7fELF" or len(b) < 0x40:
        return None
    shoff = struct.unpack_from("<Q", b, 0x28)[0]
    shent, shnum, shstr = struct.unpack_from("<HHH", b, 0x3A)
    if shoff + shent * shnum > len(b) or shstr >= shnum:
        return None
    stoff = struct.unpack_from("<Q", b, shoff + shstr * shent + 0x18)[0]
    text = shared = regs = 0
    for i in range(shnum):
        s = shoff + i * shent
        name = struct.unpack_from("<I", b, s)[0]
        size = struct.unpack_from("<Q", b, s + 0x20)[0]
        info = struct.unpack_from("<I", b, s + 0x2C)[0]
        end = b.find(b"\0", stoff + name)
        n = b[stoff + name:end].decode("ascii", "replace")
        if n.startswith(".text."):
            text, regs = size, (info >> 24) & 0xFF
        elif n.startswith(".nv.sha"):
            shared = size
    return (text, shared, regs) if text else None


def main():
    snippet = sys.argv[1] if len(sys.argv) > 1 else None
    if not snippet:
        found = sorted(glob.glob(OTA), key=os.path.getmtime)
        if not found:
            sys.exit("No snippet in the OTA cache; pass one explicitly.")
        snippet = found[-1]
    print(f"snippet : {snippet}")

    ptxas = tool("ptxas.exe")
    cuobjdump = tool("cuobjdump.exe")
    d, secs = sections(snippet)

    built, tmp = [], tempfile.mkdtemp(prefix="mfgcubins")
    for start, end in fatbins(d, secs):
        ents = list(entries(d, start, end))
        if not any(k == 1 and sm == 120 for k, sm, *_ in ents):
            continue
        orig = next(((off, psz) for k, sm, off, psz, csz in ents
                     if k == 2 and sm == 89 and csz == 0), None)
        if not orig:
            continue
        fp = fingerprint(d[orig[0]:orig[0] + orig[1]])
        if fp not in WANTED:
            continue

        fat = os.path.join(tmp, f"{fp[0]}.fatbin")
        open(fat, "wb").write(d[start:end])
        ptx = subprocess.run([cuobjdump, "-ptx", "-arch=sm_120", fat],
                             capture_output=True, text=True).stdout
        i = ptx.find(".version")
        if i < 0:
            print(f"  ! {WANTED[fp]}: no Blackwell PTX"); continue
        src = os.path.join(tmp, f"{fp[0]}.ptx")
        open(src, "w").write(ptx[i:].replace(".target sm_120", ".target sm_89"))
        out = os.path.join(tmp, f"{fp[0]}.cubin")
        r = subprocess.run([ptxas, "-arch=sm_89", "-O3", src, "-o", out],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  ! {WANTED[fp]}: ptxas failed\n{r.stderr[:400]}"); continue
        blob = open(out, "rb").read()
        if len(blob) > orig[1]:
            print(f"  ! {WANTED[fp]}: {len(blob)} > {orig[1]} bytes, will not fit")
            continue
        built.append((fp, WANTED[fp], orig[1], blob))
        print(f"  ok {WANTED[fp]}: {orig[1]} -> {len(blob)} bytes")

    if len(built) != len(WANTED):
        print(f"\nRebuilt {len(built)} of {len(WANTED)}. The proxy patches whatever "
              f"is present and leaves the rest alone.")
    if not built:
        sys.exit("Nothing to write.")

    # Which build this was generated against, so the proxy can say "built for
    # 20318081, loaded 20318464" instead of only reporting that nothing matched.
    # The OTA cache names builds by id in the path; a snippet passed by hand may
    # not, so fall back to the file name.
    m = re.search(r"[\\/]versions[\\/]([^\\/]+)[\\/]", snippet)
    built_for = m.group(1) if m else os.path.basename(snippet)

    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    dst = os.path.join(here, "src", "cubins.h")
    L = ["// Generated by tools/rebuild_cubins.py -- do not edit, do not commit.",
         "//",
         "// Built from the PTX in the snippet installed on this machine. See that",
         "// script for what these kernels are and why the Blackwell PTX is the one",
         "// worth assembling for Ada.", "",
         f"// source: {os.path.basename(snippet)}",
         f"// built for dlssg build: {built_for}", "",
         "#pragma once", "",
         f'static const char kCubinsBuiltFor[] = "{built_for}";', "",
         "struct CubinPatch {",
         "    unsigned text;      // .text size          } fingerprint of the",
         "    unsigned shared;    // .nv.shared size     } original, unique across",
         "    unsigned regs;      // registers           } all 31 framework kernels",
         "    unsigned orig_size; // bytes it must fit into",
         "    unsigned size;      // bytes of `data`",
         "    const unsigned char *data;",
         "    const char *what;",
         "};", ""]
    for fp, what, orig_size, blob in built:
        L.append(f"// {what}: {orig_size} -> {len(blob)} bytes")
        L.append(f"static const unsigned char kCubin{fp[0]}[] = {{")
        for o in range(0, len(blob), 16):
            L.append("    " + "".join(f"0x{c:02x}," for c in blob[o:o + 16]))
        L.append("};")
        L.append("")
    L.append("static const CubinPatch kCubinPatches[] = {")
    for fp, what, orig_size, blob in built:
        L.append(f'    {{ {fp[0]}u, {fp[1]}u, {fp[2]}u, {orig_size}u, '
                 f'sizeof(kCubin{fp[0]}), kCubin{fp[0]}, "{what}" }},')
    L.append("};")
    L.append("")
    open(dst, "w", newline="\n").write("\n".join(L))
    print(f"\nwrote {dst}")


if __name__ == "__main__":
    main()
