"""Genera los forwarders de version.dll: los stubs en asm, la tabla de nombres
y el .def del linker.

    python3 gpuprobe/tools/gen_version_forwards.py

Un proxy de version.dll tiene que exportar TODO lo que exporta el de sistema:
cualquier modulo que haga LoadLibrary("version.dll") nos recibe a nosotros, y
un GetProcAddress que falle lo tira abajo. No se puede reenviar por nombre a
"version.X" -- volveria a nosotros -- asi que cada export es un stub que
resuelve la direccion real la primera vez y salta.

version.dll tiene 17 exports y son estables desde hace veinte anios, asi que la
lista esta escrita aca en vez de leerse del DLL con objdump (como hace
tools/gen_forwards.py en la raiz para winmm, que tiene mas de cien). Si alguna
vez cambia, el log lo dice: forward_resolve escribe el nombre que no encontro.
"""

import os

NAMES = [
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW", "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW", "GetFileVersionInfoW",
    "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW",
    "VerLanguageNameA", "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW",
]

HEAD = """# Generado por tools/gen_version_forwards.py -- no editar a mano.
# Cada export de version.dll: guarda su indice en eax y salta al resolvedor
# comun, que llama a forward_resolve(idx) preservando los registros de
# argumentos y salta a la direccion real. rax es scratch en x64, asi que
# usarlo para el indice no pisa ningun argumento.
    .text
    .p2align 4
forward_common:
    push %rcx
    push %rdx
    push %r8
    push %r9
    sub $0x68, %rsp                 # shadow (0x20) + 4 xmm (0x40) + relleno
    movdqu %xmm0, 0x20(%rsp)
    movdqu %xmm1, 0x30(%rsp)
    movdqu %xmm2, 0x40(%rsp)
    movdqu %xmm3, 0x50(%rsp)
    mov %eax, %ecx
    call forward_resolve
    movdqu 0x20(%rsp), %xmm0
    movdqu 0x30(%rsp), %xmm1
    movdqu 0x40(%rsp), %xmm2
    movdqu 0x50(%rsp), %xmm3
    add $0x68, %rsp
    pop %r9
    pop %r8
    pop %rdx
    pop %rcx
    jmp *%rax
"""

here = os.path.dirname(os.path.abspath(__file__))
proxy = os.path.join(here, "..", "proxy")

S = [HEAD]
for i, n in enumerate(NAMES):
    S.append(f"""
    .globl {n}
    .def {n}; .scl 2; .type 32; .endef
{n}:
    mov ${i}, %eax
    jmp forward_common
""")
with open(os.path.join(proxy, "forwards_version.S"), "w") as fh:
    fh.write("".join(S))

H = ["// Generado por tools/gen_version_forwards.py -- no editar a mano.",
     "#pragma once", "",
     "// Los nombres, en el MISMO orden que los indices de forwards_version.S.",
     "static const char *const kVersionExports[] = {"]
H += [f'    "{n}",' for n in NAMES]
H += ["};", ""]
with open(os.path.join(proxy, "forwards_version_names.h"), "w") as fh:
    fh.write("\n".join(H))

with open(os.path.join(proxy, "exports.def"), "w") as fh:
    fh.write("EXPORTS\n" + "\n".join(NAMES) + "\n")

print(f"{len(NAMES)} exports generados en {os.path.normpath(proxy)}")
