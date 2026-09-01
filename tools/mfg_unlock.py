#!/usr/bin/env python3
"""Unlock DLSS Multi Frame Generation on pre-Blackwell NVIDIA GPUs.

NVIDIA gates MFG entirely inside the NGX snippet nvngx_dlssg.dll.  Two sites
compare the NGX GPU architecture enum against 0x1B0 (Blackwell GB20x):

  EndpointCore::Create                      -> m_multiFrameSupported
      When 0, EndpointCoreInputs::ComputeAndValidateTimeFactor rejects any
      MultiFrameCount other than 1.

  DLSSGInstanceManager::PopulateParameters  -> DLSSG.MultiFrameCountMax
      Reports 1 instead of 5.  sl.dlss_g reads this and clamps the app's
      numFramesToGenerate against it.

Nothing else is architecture dependent: the interpolation network is driven by
timeFactor = index / (count + 1), and the snippet ships native sm_89 cubins for
every kernel module.

Rewriting the immediate to 0 makes the comparison read "arch >= 0", which is
true for every real architecture id, so whichever way the compiler phrased the
predicate the Blackwell branch is the one taken.  That holds for jl/jb/jge/jae,
setae/setb, cmovl/cmovb and friends alike -- but not for an equality test, which
is why those are refused rather than patched.

Which of the two forms appears depends on the compiler, not the version:
310.6.0.0 uses `cmp esi,0x1B0 ; cmovl r8d,ebx` where 310.7/310.8 use
`cmp ebp,0x1B0 ; jl`.  Hence pattern matching on the flag consumer rather than
on a byte string.

usage: mfg_unlock.py <nvngx_dlssg.dll> [output.dll]
"""
import sys, struct, hashlib, shutil, os
import pefile, capstone

ARCH_BLACKWELL = 0x1B0
EXPECTED_SITES = 2

# Conditionals whose sense is preserved by "arch >= 0": every ordering test.
ORDERING = {
    'jl', 'jb', 'jle', 'jbe', 'jge', 'jae', 'jnb', 'jg', 'ja',
    'setl', 'setb', 'setle', 'setbe', 'setge', 'setae', 'setnb', 'setg', 'seta',
    'cmovl', 'cmovb', 'cmovle', 'cmovbe', 'cmovge', 'cmovae', 'cmovnb',
    'cmovg', 'cmova',
}
# An equality test means something else entirely; refuse rather than guess.
EQUALITY = {'je', 'jne', 'jz', 'jnz', 'sete', 'setne', 'cmove', 'cmovne'}

# Instructions that clobber the flags, ending the search for the consumer.
def writes_flags(ins):
    return capstone.x86.X86_REG_EFLAGS in ins.regs_access()[1]


def sections(pe):
    return [(s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData,
             s.SizeOfRawData, s.Name.decode().rstrip('\0')) for s in pe.sections]


def find_sites(data, pe):
    """Return [(file_off, imm_off, rva, size, orig_bytes, follower, ok)]."""
    secs = sections(pe)
    text = next(s for s in secs if s[4] == '.text')
    tva, tvs, tpo, trs, _ = text
    base = pe.OPTIONAL_HEADER.ImageBase
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    # .pdata gives reliable function starts, avoiding linear-sweep desync.
    pd = next(s for s in secs if s[4] == '.pdata')
    raw = data[pd[2]:pd[2] + pd[3]]
    funcs = sorted({struct.unpack_from('<III', raw, i)[:2]
                    for i in range(0, len(raw) - 11, 12)} - {(0, 0)})
    sites = []
    for fs, fe in funcs:
        if not (tva <= fs < tva + max(tvs, trs)):
            continue
        off = tpo + (fs - tva)
        ins = list(md.disasm(data[off:off + (fe - fs)], base + fs))
        for i, a in enumerate(ins):
            if a.mnemonic != 'cmp' or len(a.operands) != 2:
                continue
            o0, o1 = a.operands
            if (o0.type != capstone.x86.X86_OP_REG
                    or o1.type != capstone.x86.X86_OP_IMM
                    or o1.imm != ARCH_BLACKWELL):
                continue
            # First flag consumer after the compare, within a short window.
            follower, ok = None, False
            for b in ins[i + 1:i + 5]:
                if b.mnemonic in ORDERING:
                    follower, ok = b.mnemonic, True
                    break
                if b.mnemonic in EQUALITY:
                    follower, ok = b.mnemonic, False
                    break
                if writes_flags(b):
                    follower, ok = b.mnemonic, False
                    break
            rva = a.address - base
            sites.append((tpo + (rva - tva), tpo + (rva - tva) + a.size - 4,
                          rva, a.size, bytes(a.bytes), follower or '?', ok))
    return sites


def pe_checksum(buf, ck_off):
    s = 0
    for i in range(0, len(buf) & ~1, 2):
        if i in (ck_off, ck_off + 2):
            continue
        s += struct.unpack_from('<H', buf, i)[0]
        s = (s & 0xFFFF) + (s >> 16)
    if len(buf) & 1:
        s += buf[-1]
        s = (s & 0xFFFF) + (s >> 16)
    return (s + len(buf)) & 0xFFFFFFFF


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src
    data = bytearray(open(src, 'rb').read())
    pe = pefile.PE(src, fast_load=True)
    print(f"in : {src}")
    print(f"     {len(data)} bytes  sha256 {hashlib.sha256(data).hexdigest()[:16]}")

    sites = find_sites(bytes(data), pe)
    if not sites:
        sys.exit("!! no `cmp reg, 0x1B0` found. Already patched, or a build this "
                 "does not understand -- nothing written.")

    done = 0
    for foff, imm_off, rva, size, orig, follower, ok in sites:
        if not ok:
            print(f"  ? {hex(rva)}  cmp/{follower}: not an ordering test, left alone")
            continue
        struct.pack_into('<I', data, imm_off, 0)
        print(f"  + {hex(rva)} (file {hex(foff)})  cmp/{follower}  "
              f"{orig.hex()} -> {bytes(data[foff:foff+size]).hex()}")
        done += 1

    if done == 0:
        sys.exit("!! found the compares but none was patchable -- nothing written.")
    if done != EXPECTED_SITES:
        print(f"\n!! patched {done} site(s), expected {EXPECTED_SITES}.")
        print("!! Both gates must go: one enables multi-frame, the other reports how")
        print("!! many frames are allowed. Check the disassembly before using this.")

    e = struct.unpack_from('<I', data, 0x3C)[0]
    ck = e + 0x58
    struct.pack_into('<I', data, ck, 0)
    struct.pack_into('<I', data, ck, pe_checksum(data, ck))

    if dst == src and not os.path.exists(src + '.orig'):
        shutil.copy2(src, src + '.orig')
        print(f"  + backup: {src}.orig")
    open(dst, 'wb').write(data)
    print(f"out: {dst}")
    print(f"     sha256 {hashlib.sha256(data).hexdigest()[:16]}")
    if done == EXPECTED_SITES:
        print("\nBoth gates removed. MultiFrameCountMax now reports 5 (up to 6x).")


if __name__ == '__main__':
    main()
