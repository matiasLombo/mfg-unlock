"""H4: encuentra el kernel de blend temporal en un snippet y confirma el 0.5.

Confirma si el snippet hereda el bug del punto medio (RenoDx): el kernel de
interpolacion mezcla con un 0.5 compilado (104 `mul.ftz.f32 ...,0f3F000000`) en
vez del parametro temporal, asi que todos los frames generados caen al medio.
Reproducible sobre el snippet 2.12 embebido. Necesita cuobjdump (pip
nvidia-cuda-cuobjdump). Ver docs/investigacion-fraccionales.md (H4).

    python tools/h4_find_blend.py [ruta_al_nvngx_dlssg.dll]
"""
import struct, re, os, subprocess
SNIP=r"C:\Users\matia\AppData\Local\mfg-unlock\sdk\2.12\nvngx_dlssg.dll"
CUOBJ=r"C:\Users\matia\AppData\Roaming\Python\Python313\site-packages\nvidia\cu13\bin\cuobjdump.exe"
TMP=r"C:\Users\matia\AppData\Local\Temp\claude\C--Program-Files--x86--Steam-steamapps-common-Grand-Theft-Auto-V-Enhanced\d34810b4-1938-4448-86d5-782833a6fc65\scratchpad\h4tmp"
os.makedirs(TMP,exist_ok=True)
d=open(SNIP,"rb").read()
def sections():
    lf=struct.unpack_from("<I",d,0x3C)[0]; nsec=struct.unpack_from("<H",d,lf+6)[0]; opt=struct.unpack_from("<H",d,lf+20)[0]
    first=lf+24+opt; out=[]
    for i in range(nsec):
        s=first+i*40; name=d[s:s+8].rstrip(b"\0").decode("ascii","replace"); vsz,va,rsz,ptr=struct.unpack_from("<IIII",d,s+8); out.append((va,max(vsz,rsz),ptr,name))
    return out
def fatbins(secs):
    for va,size,ptr,name in secs:
        if name!=".data": continue
        blob=d[ptr:ptr+size]
        for m in re.finditer(re.escape(struct.pack("<I",0xBA55ED50)),blob):
            o=m.start(); hsz=struct.unpack_from("<H",blob,o+6)[0]; fsz=struct.unpack_from("<Q",blob,o+8)[0]
            if hsz!=16 or not (0<fsz<8<<20) or o+16+fsz>len(blob): continue
            yield ptr+o, ptr+o+16+fsz
secs=sections()
print("idx  ptx_sm89_bytes  midpoint_muls(0f3F000000)  has_BB0_3  param0+32")
hit=None
for idx,(s,e) in enumerate(fatbins(secs)):
    fat=os.path.join(TMP,f"c{idx}.fatbin"); open(fat,"wb").write(d[s:e])
    r=subprocess.run([CUOBJ,"-ptx","-arch=sm_89",fat],capture_output=True,text=True)
    ptx=r.stdout
    i=ptx.find(".version"); ptx=ptx[i:] if i>=0 else ""
    if not ptx:
        continue
    nb=len(ptx.encode())
    muls=len(re.findall(r"mul\.ftz\.f32[^;]*0f3F000000", ptx))
    bb=("$L__BB0_3:" in ptx)
    p32=("main_kernel_param_0+32" in ptx) or ("_param_0+32" in ptx)
    flag=""
    if muls>=50:
        flag=" <== BLEND KERNEL"
        hit=(idx,nb,muls,ptx)
    print(f"{idx:>3}  {nb:>13}  {muls:>6}  {str(bb):>9}  {p32}{flag}")
if hit:
    idx,nb,muls,ptx=hit
    open(os.path.join(TMP,"blend_sm89.ptx"),"w").write(ptx)
    print(f"\nBLEND kernel = idx {idx}: {nb} bytes PTX, {muls} muls con 0f3F000000")
    print("guardado: blend_sm89.ptx")
    # verificar el patron exacto de RenoDx
    print("kExpectedPtxBytes esperado por RenoDx: 99362  | el nuestro:", nb)
    print("kExpectedMidpoints esperado: 104            | el nuestro:", muls)
