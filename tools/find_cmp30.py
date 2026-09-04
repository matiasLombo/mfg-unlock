#!/usr/bin/env python3
"""Broad search: every `cmp <reg>, 0x1e` (30) in .text, with a disassembly
window around it. 30 is the feedback-counter threshold FINDINGS.md documents
("crosses 30 about three quarters of a second in"), used to see whether that
constant survives in a build where the sete/mov/movzx/cmp/cmovae shape
diagnose_pacer.py doesn't find at all.

Needs pefile and capstone: pip install pefile capstone

usage: find_cmp30.py <dll> [window]
"""
import sys, struct
import pefile, capstone


def sections(pe):
    return [(s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData,
             s.SizeOfRawData, s.Name.decode().rstrip('\0')) for s in pe.sections]


def main():
    path = sys.argv[1]
    window = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    data = open(path, 'rb').read()
    pe = pefile.PE(path, fast_load=True)
    secs = sections(pe)
    text = next(s for s in secs if s[4] == '.text')
    tva, tvs, tpo, trs, _ = text
    base = pe.OPTIONAL_HEADER.ImageBase

    pd = next((s for s in secs if s[4] == '.pdata'), None)
    raw = data[pd[2]:pd[2] + pd[3]]
    funcs = sorted({struct.unpack_from('<III', raw, i)[:2]
                    for i in range(0, len(raw) - 11, 12)} - {(0, 0)})

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    found = 0
    for fs, fe in funcs:
        if not (tva <= fs < tva + max(tvs, trs)):
            continue
        off = tpo + (fs - tva)
        ins = list(md.disasm(data[off:off + (fe - fs)], base + fs))
        for i, a in enumerate(ins):
            if a.mnemonic != 'cmp' or len(a.operands) != 2:
                continue
            o0, o1 = a.operands
            if o1.type != capstone.x86.X86_OP_IMM or o1.imm != 0x1e:
                continue
            if o0.type != capstone.x86.X86_OP_REG:
                continue
            found += 1
            lo = max(0, i - window)
            hi = min(len(ins), i + window + 1)
            print(f"--- cmp #{found} @ {hex(a.address - base)} in func {hex(fs)} ---")
            for x in ins[lo:hi]:
                marker = ">> " if x is a else "   "
                print(f"{marker}{hex(x.address - base):>9s}: {x.mnemonic} {x.op_str}")
            print()

    print(f"total: {found} occurrences of cmp reg, 0x1e")


if __name__ == '__main__':
    main()
