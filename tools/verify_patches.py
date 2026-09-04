#!/usr/bin/env python3
"""Run every patch's signature against every DLSS-G snippet / Streamline
plugin build this machine can find -- official Streamline SDK release
binaries under corpus/, whatever NVIDIA's OTA cache has stashed locally, and
any game's own bundled copies -- and report hit counts.

This exists so a build's shape changing (the whole reason patch_enable_cpu_pacer
went to "sites: 0" against 190_E658703/134656 -- see BUGREPORT / github issue #2)
shows up here, against a known corpus, instead of being discovered by playing a
game and seeing it look wrong.

Needs pefile and capstone: pip install pefile capstone

usage: verify_patches.py [extra search roots...]
"""
import sys, os, struct, glob
import pefile, capstone

ARCH_BLACKWELL = 0x1B0
ORDERING = {
    'jl', 'jb', 'jle', 'jbe', 'jge', 'jae', 'jnb', 'jg', 'ja',
    'setl', 'setb', 'setle', 'setbe', 'setge', 'setae', 'setnb', 'setg', 'seta',
    'cmovl', 'cmovb', 'cmovle', 'cmovbe', 'cmovge', 'cmovae', 'cmovnb',
    'cmovg', 'cmova',
}
EQUALITY = {'je', 'jne', 'jz', 'jnz', 'sete', 'setne', 'cmove', 'cmovne'}


def sections(pe):
    return [(s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData,
             s.SizeOfRawData, s.Name.decode(errors='replace').rstrip('\0'))
            for s in pe.sections]


def text_and_funcs(data, pe):
    secs = sections(pe)
    text = next((s for s in secs if s[4] == '.text'), None)
    if text is None:
        return None, None, None, None
    tva, tvs, tpo, trs, _ = text
    base = pe.OPTIONAL_HEADER.ImageBase
    pd = next((s for s in secs if s[4] == '.pdata'), None)
    funcs = []
    if pd is not None:
        raw = data[pd[2]:pd[2] + pd[3]]
        funcs = sorted({struct.unpack_from('<III', raw, i)[:2]
                        for i in range(0, len(raw) - 11, 12)} - {(0, 0)})
    return (tva, tpo, trs, tvs), base, funcs, data


def count_gates(path):
    """Mirrors tools/mfg_unlock.py find_sites: cmp reg,0x1B0 whose first flag
    consumer is an ordering test. Expected: 2."""
    data = open(path, 'rb').read()
    pe = pefile.PE(path, fast_load=True)
    geo, base, funcs, data = text_and_funcs(data, pe)
    if geo is None or not funcs:
        return None
    tva, tpo, trs, tvs = geo
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    hits, refused = 0, 0
    for fs, fe in funcs:
        if not (tva <= fs < tva + max(tvs, trs)):
            continue
        off = tpo + (fs - tva)
        ins = list(md.disasm(data[off:off + (fe - fs)], base + fs))
        for i, a in enumerate(ins):
            if a.mnemonic != 'cmp' or len(a.operands) != 2:
                continue
            o0, o1 = a.operands
            if (o0.type != capstone.x86.X86_OP_REG or
                    o1.type != capstone.x86.X86_OP_IMM or o1.imm != ARCH_BLACKWELL):
                continue
            for b in ins[i + 1:i + 5]:
                if b.mnemonic in ORDERING:
                    hits += 1
                    break
                if b.mnemonic in EQUALITY:
                    refused += 1
                    break
    return hits


def count_pacer(path):
    """Form A (cmov-based, pre-134656) + Form B (jcc-based, 134656+). Each
    counted separately; total is what patch_enable_cpu_pacer would report."""
    data = bytearray(open(path, 'rb').read())
    pe = pefile.PE(path, fast_load=True)
    secs = sections(pe)
    text_sec = next((s for s in secs if s[4] == '.text'), None)
    if text_sec is None:
        return None, None
    tva, tvs, tpo, trs, _ = text_sec
    text = data[tpo:tpo + trs]
    n = len(text)

    a_hits = 0
    i = 0
    while i + 15 <= n:
        if text[i] == 0x0F and text[i+1] == 0x94 and text[i+2] == 0xC0 and \
           text[i+3] == 0x41 and text[i+4] == 0x8B:
            m1 = text[i+5]
            if (m1 & 0xC7) == 0xC7:
                reg = (m1 >> 3) & 7
                if text[i+6] == 0x0F and text[i+7] == 0xB6 and text[i+8] == 0xC8 and \
                   text[i+9] == 0x83 and text[i+10] == 0xFA and text[i+11] == 0x1E and \
                   text[i+12] == 0x0F and text[i+13] == 0x43 and \
                   text[i+14] == (0xC0 | (reg << 3) | 1):
                    a_hits += 1
        i += 1

    b_hits = 0
    i = 0
    while i + 11 <= n:
        if text[i] == 0x84 and text[i+1] == 0xDB and text[i+2] == 0x75 and \
           text[i+4] == 0x83 and text[i+5] == 0xFF and text[i+6] == 0x1E and \
           text[i+7] == 0x72 and text[i+9] == 0xB3 and text[i+10] == 0x01:
            t1 = (i + 4) + struct.unpack('b', bytes([text[i+3]]))[0]
            t2 = (i + 9) + struct.unpack('b', bytes([text[i+8]]))[0]
            if t1 == t2:
                b_hits += 1
        i += 1

    return a_hits, b_hits


def discover(extra_roots):
    snippets, plugins = [], []

    for f in sorted(glob.glob(r"C:\Users\matia\dev\mfg-unlock\corpus\streamline-releases\*\nvngx_dlssg.dll")):
        snippets.append(f)
    for f in sorted(glob.glob(r"C:\Users\matia\dev\mfg-unlock\corpus\streamline-releases\*\sl.dlss_g.dll")):
        plugins.append(f)

    for f in sorted(glob.glob(r"C:\ProgramData\NVIDIA\NGX\models\dlssg\versions\*\files\*.bin")):
        snippets.append(f)
    for f in sorted(glob.glob(r"C:\ProgramData\NVIDIA\NGX\models\sl_dlss_g_0\versions\*\files\*.dll")):
        plugins.append(f)
    for f in sorted(glob.glob(r"C:\ProgramData\NVIDIA\NGX\models\dlss_override\versions\*\files\*\sl.dlss_g.dll")):
        plugins.append(f)
    for f in sorted(glob.glob(r"C:\ProgramData\NVIDIA\NGX\models\sl_dlss_g_override_0\versions\*\files\*.dll")):
        plugins.append(f)

    driver_store = glob.glob(r"C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispi*\nvngx_dlssg.dll")
    snippets.extend(sorted(driver_store))

    for root in extra_roots:
        for dirpath, _, filenames in os.walk(root):
            for fn in filenames:
                low = fn.lower()
                if low == 'nvngx_dlssg.dll':
                    snippets.append(os.path.join(dirpath, fn))
                elif low == 'sl.dlss_g.dll':
                    plugins.append(os.path.join(dirpath, fn))

    return snippets, plugins


def label(path):
    for marker in ("streamline-releases",):
        if marker in path:
            parts = path.split(os.sep)
            idx = parts.index(marker)
            return f"release {parts[idx+1]}"
    if r"versions" in path:
        parts = path.split(os.sep)
        try:
            idx = parts.index("versions")
            return f"OTA {parts[idx+1]}"
        except ValueError:
            pass
    if "DriverStore" in path:
        return "driver store"
    return "local"


def main():
    extra_roots = sys.argv[1:] or [
        r"C:\Program Files (x86)\Steam\steamapps\common",
    ]
    snippets, plugins = discover(extra_roots)

    print(f"=== snippets (nvngx_dlssg.dll-shaped): {len(snippets)} found ===")
    print(f"{'gates':>6}  {'label':<16} {'size':>9}  path")
    for p in sorted(set(snippets)):
        try:
            hits = count_gates(p)
        except Exception as e:
            hits = f"ERR:{e}"
        flag = "" if hits == 2 else "  <-- !="
        size = os.path.getsize(p)
        print(f"{str(hits):>6}  {label(p):<16} {size:>9}  {p}{flag}")

    print(f"\n=== plugins (sl.dlss_g.dll-shaped): {len(plugins)} found ===")
    print(f"{'A':>3} {'B':>3} {'tot':>4}  {'label':<16} {'size':>9}  path")
    for p in sorted(set(plugins)):
        try:
            a, b = count_pacer(p)
        except Exception as e:
            a, b = f"ERR", 0
            print(f"pacer error on {p}: {e}")
        tot = (a if isinstance(a, int) else 0) + (b if isinstance(b, int) else 0)
        flag = "" if tot >= 1 else "  <-- 0 sites"
        size = os.path.getsize(p)
        print(f"{str(a):>3} {str(b):>3} {tot:>4}  {label(p):<16} {size:>9}  {p}{flag}")


if __name__ == '__main__':
    main()
