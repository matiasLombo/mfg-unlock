"""Dump per-function annotated disassembly of any PE. usage: an.py <dll> <out.asm>"""
import struct, capstone, sys, pefile
path, outp = sys.argv[1], sys.argv[2]
pe=pefile.PE(path, fast_load=True); base=pe.OPTIONAL_HEADER.ImageBase
data=open(path,'rb').read()
secs=[(s.VirtualAddress,s.Misc_VirtualSize,s.PointerToRawData,s.SizeOfRawData,s.Name.decode().rstrip('\0')) for s in pe.sections]
def rva2off(r):
    for va,vs,po,rs,n in secs:
        if va<=r<va+max(vs,rs): return po+(r-va)
def read(r,n):
    o=rva2off(r); return data[o:o+n] if o is not None else b''
pd=next((s for s in secs if s[4]=='.pdata'), None)
raw=data[pd[2]:pd[2]+pd[3]]
funcs=sorted({struct.unpack_from('<III',raw,i)[:2] for i in range(0,len(raw)-11,12)}-{(0,0)})
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64); md.detail=True
cache={}
def sat(t):
    if t not in cache:
        s=read(t,80).split(b'\0')[0]
        try: sd=s.decode('ascii')
        except: sd=''
        cache[t]=f" ; ->{hex(t)} {sd[:70]!r}" if len(sd)>3 and sd.isprintable() else f" ; ->{hex(t)}"
    return cache[t]
with open(outp,'w',encoding='utf-8') as o:
    for s,e in funcs:
        off=rva2off(s)
        if off is None: continue
        o.write(f"\n;=== FUNC {hex(s)} - {hex(e)} ({e-s}) ===\n")
        for i in md.disasm(data[off:off+(e-s)], base+s):
            extra=""
            for op in i.operands:
                if op.type==capstone.x86.X86_OP_MEM and op.mem.base==capstone.x86.X86_REG_RIP:
                    extra=sat(i.address+i.size+op.mem.disp-base)
            o.write(f"{hex(i.address-base):>9s}: {i.mnemonic:<10s} {i.op_str}{extra}\n")
print("funcs:",len(funcs),"->",outp)
