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

Identical shared-memory layout and access offsets either way. That was long
read here as "a speed change, not an image change" -- it is not. Swapping these
in is what made Avatar: Frontiers of Pandora fluid at 4x after nine other
hypotheses had been measured and discarded; without them the same build, same
snippet and same settings judder on camera movement. The mvec-estimate kernel is
the one that matters, which fits: it is what camera motion is reconstructed
from.

Nothing NVIDIA-derived is redistributed: this reads the snippet already installed
on the machine and writes src/cubins.h next to it.

Needs ptxas and nvdisasm, which pip provides without a full CUDA toolkit:

    pip install nvidia-cuda-nvcc nvidia-cuda-nvdisasm nvidia-cuda-cuobjdump

Usage:
    python tools/rebuild_cubins.py [snippet ...]

With no argument it takes every snippet in NVIDIA's OTA cache plus any passed by
hand, and emits one header covering all of them. That matters because games do
not agree on which snippet they load: DOOM and GTA V take the newest OTA copy,
while Avatar loads the 310.3 it ships with. A header built for one build simply
reports "slot moved" against every other, which is how the kernels quietly
stopped being applied in DOOM without anyone noticing.

Kernels are discovered, not listed. Any kernel carrying Blackwell PTX whose
sm_89 rebuild fits the space the original occupies is taken -- that was always
the real criterion; the fixed fingerprint table just froze one snippet's answer
and made every other build look like it had no kernels at all. 310.3 has five
that fit where 310.9 has three, and they were being missed entirely.
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
# Names only, for the log. Keyed by .nv.shared size, which is the part of the
# fingerprint that survives a snippet rebuild: 310.3 and 310.9 compile the same
# kernel to different .text sizes and register counts but the same shared
# layout. Anything not listed still gets rebuilt, just described by its size.
KNOWN_BY_SHARED = {
    7776: "mvec estimate",
    3920: "inpaint",
     784: "inpaint decision",
}


def describe(fp):
    n = KNOWN_BY_SHARED.get(fp[1])
    return n if n else f"kernel {fp[0]}/{fp[1]}/{fp[2]}"

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


def harvest(snippet, ptxas, cuobjdump, tmp, seen, out):
    """Every kernel in one snippet whose Blackwell rebuild fits its own slot."""
    d, secs = sections(snippet)
    took = 0
    for start, end in fatbins(d, secs):
        ents = list(entries(d, start, end))
        if not any(k == 1 and sm == 120 for k, sm, *_ in ents):
            continue
        orig = next(((off, psz) for k, sm, off, psz, csz in ents
                     if k == 2 and sm == 89 and csz == 0), None)
        if not orig:
            continue
        fp = fingerprint(d[orig[0]:orig[0] + orig[1]])
        # Keyed by fingerprint *and* slot size, because the proxy matches on
        # both. 310.5 and 310.9 give the mvec kernel the same (text, shared,
        # regs) but different slots -- 39712 against 39968 -- so deduping on
        # the fingerprint alone silently dropped whichever build came second,
        # and that was 310.9: the one DOOM and GTA V actually load.
        key = (fp, orig[1]) if fp else None
        if not fp or key in seen:
            continue

        fat = os.path.join(tmp, f"{fp[0]}_{fp[1]}_{fp[2]}.fatbin")
        open(fat, "wb").write(d[start:end])
        ptx = subprocess.run([cuobjdump, "-ptx", "-arch=sm_120", fat],
                             capture_output=True, text=True).stdout
        i = ptx.find(".version")
        if i < 0:
            continue
        src = fat[:-7] + ".ptx"
        open(src, "w").write(ptx[i:].replace(".target sm_120", ".target sm_89"))
        dst = fat[:-7] + ".cubin"
        r = subprocess.run([ptxas, "-arch=sm_89", "-O3", src, "-o", dst],
                           capture_output=True, text=True)
        if r.returncode != 0:
            continue
        blob = open(dst, "rb").read()
        if len(blob) > orig[1]:
            continue                      # would not fit; leave NVIDIA's alone
        seen.add(key)
        out.append((fp, describe(fp), orig[1], blob))
        took += 1
        print(f"  ok {describe(fp):<22} {fp}  {orig[1]} -> {len(blob)} bytes")
    print(f"  -> {took} kernel(s) from {os.path.basename(snippet)}")
    return took


def main():
    given = sys.argv[1:]
    cache = sorted(glob.glob(OTA), key=os.path.getmtime)
    snippets = given if given else cache
    if not snippets:
        sys.exit("No snippet in the OTA cache; pass one explicitly.")
    # Newest last so that, where two builds share a fingerprint, the first one
    # seen wins and the table stays stable across reruns.
    ptxas = tool("ptxas.exe")
    cuobjdump = tool("cuobjdump.exe")
    tmp = tempfile.mkdtemp(prefix="mfgcubins")

    built, seen, builds = [], set(), []
    for sn in snippets:
        print(f"snippet : {sn}")
        if harvest(sn, ptxas, cuobjdump, tmp, seen, built):
            m = re.search(r"[\/]versions[\/]([^\/]+)[\/]", sn)
            builds.append(m.group(1) if m else os.path.basename(sn))

    if not built:
        sys.exit("Nothing to write.")
    print("")
    print(f"{len(built)} kernel(s) across {len(builds)} build(s).")

    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    dst = os.path.join(here, "src", "cubins.h")
    L = ["// Generated by tools/rebuild_cubins.py -- do not edit, do not commit.",
         "//",
         "// Built from the PTX in the snippets installed on this machine. See that",
         "// script for what these kernels are and why the Blackwell PTX is the one",
         "// worth assembling for Ada.",
         "//",
         "// Covers several snippet builds at once, because games do not agree on",
         "// which one they load: the proxy matches each entry by fingerprint, so a",
         "// snippet takes its own kernels and ignores the rest.", "",
         "// builds: " + ", ".join(builds), "",
         "#pragma once", "",
         f'static const char kCubinsBuiltFor[] = "{", ".join(builds)}";', "",
         "struct CubinPatch {",
         "    unsigned text;      // .text size          } fingerprint of the",
         "    unsigned shared;    // .nv.shared size     } original, unique within",
         "    unsigned regs;      // registers           } a snippet build",
         "    unsigned orig_size; // bytes it must fit into",
         "    unsigned size;      // bytes of `data`",
         "    const unsigned char *data;",
         "    const char *what;",
         "};", ""]
    for fp, what, orig_size, blob in built:
        # The slot size belongs in the symbol name too: two builds can give a
        # kernel the same (text, shared, regs) and different slots, and naming
        # by fingerprint alone then emits the same array twice.
        tag = f"{fp[0]}_{fp[1]}_{fp[2]}_{orig_size}"
        L.append(f"// {what}: {orig_size} -> {len(blob)} bytes")
        L.append(f"static const unsigned char kCubin{tag}[] = {{")
        for o in range(0, len(blob), 16):
            L.append("    " + "".join(f"0x{c:02x}," for c in blob[o:o + 16]))
        L.append("};")
        L.append("")
    L.append("static const CubinPatch kCubinPatches[] = {")
    for fp, what, orig_size, blob in built:
        # The slot size belongs in the symbol name too: two builds can give a
        # kernel the same (text, shared, regs) and different slots, and naming
        # by fingerprint alone then emits the same array twice.
        tag = f"{fp[0]}_{fp[1]}_{fp[2]}_{orig_size}"
        L.append(f'    {{ {fp[0]}u, {fp[1]}u, {fp[2]}u, {orig_size}u, '
                 f'sizeof(kCubin{tag}), kCubin{tag}, "{what}" }},')
    L.append("};")
    L.append("")
    open(dst, "w", newline=chr(10)).write(chr(10).join(L))
    print(f"wrote {dst}")


if __name__ == "__main__":
    main()
