#!/usr/bin/env python3
"""Unlock DLSS Multi Frame Generation on pre-Blackwell NVIDIA GPUs.

NVIDIA gates MFG entirely inside the NGX snippet nvngx_dlssg.dll.  Two sites
compare the NGX GPU architecture enum against 0x1B0 (Blackwell GB20x):

  EndpointCore::Create                      cmp <r32>, 0x1B0 ; setae al
      -> m_multiFrameSupported.  When 0, EndpointCoreInputs::ComputeAndValidate-
         TimeFactor rejects any MultiFrameCount other than 1.

  DLSSGInstanceManager::PopulateParameters  cmp <r32>, 0x1B0 ; jl
      -> reports DLSSG.MultiFrameCountMax as 1 instead of 5.  sl.dlss_g reads
         this and clamps the app's numFramesToGenerate against it.

Nothing else is architecture dependent: the interpolation network is driven by
timeFactor = index / (count + 1), and the snippet ships native sm_89 cubins for
every kernel module.

Both comparisons are rewritten against 0 so every architecture takes the
supported branch.  Located by pattern, so this works across snippet versions.

usage: mfg_unlock.py <nvngx_dlssg.dll> [output.dll]
"""
import sys, struct, hashlib, shutil, os
import pefile, capstone

ARCH_BLACKWELL = 0x1B0

def sections(pe):
    return [(s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData,
             s.SizeOfRawData, s.Name.decode().rstrip('\0')) for s in pe.sections]

def find_sites(data, pe):
    """Return [(file_offset, imm_field_offset, rva, desc)] for each arch gate."""
    secs = sections(pe)
    text = next(s for s in secs if s[4] == '.text')
    va, vs, po, rs, _ = text
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
        if not (va <= fs < va + max(vs, rs)):
            continue
        off = po + (fs - va)
        ins = list(md.disasm(data[off:off + (fe - fs)], base + fs))
        for a, b in zip(ins, ins[1:]):
            if a.mnemonic != 'cmp' or len(a.operands) != 2:
                continue
            o0, o1 = a.operands
            if o0.type != capstone.x86.X86_OP_REG or o1.type != capstone.x86.X86_OP_IMM:
                continue
            if o1.imm != ARCH_BLACKWELL:
                continue
            if b.mnemonic == 'setae':
                desc = "EndpointCore::Create -> m_multiFrameSupported"
            elif b.mnemonic in ('jl', 'jb'):
                desc = "PopulateParameters -> DLSSG.MultiFrameCountMax"
            else:
                desc = f"cmp/{b.mnemonic} (unrecognised, skipped)"
                continue
            rva = a.address - base
            foff = po + (rva - va)
            # immediate is the last 4 bytes of the cmp encoding
            sites.append((foff, foff + a.size - 4, rva, desc, a.size, bytes(a.bytes)))
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
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src
    data = bytearray(open(src, 'rb').read())
    pe = pefile.PE(src, fast_load=True)
    print(f"in : {src}")
    print(f"     {len(data)} bytes  sha256 {hashlib.sha256(data).hexdigest()[:16]}")
    sites = find_sites(bytes(data), pe)
    if not sites:
        sys.exit("no arch gates found -- already patched, or an unexpected build")
    for foff, imm_off, rva, desc, size, orig in sites:
        struct.pack_into('<I', data, imm_off, 0)
        print(f"  + {hex(rva)} (file {hex(foff)})  {orig.hex()} -> "
              f"{bytes(data[foff:foff+size]).hex()}")
        print(f"      {desc}")
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
    print(f"\n{len(sites)} gate(s) removed. MultiFrameCountMax now reports 5 (up to 6x).")

if __name__ == '__main__':
    main()
