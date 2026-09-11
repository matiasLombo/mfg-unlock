"""Genera los forwarders de winmm.dll para que el mismo dll pueda cargarse
con ese nombre.

Un proxy de winmm.dll tiene que exportar TODO lo que exporta el de sistema:
cualquier modulo que haga LoadLibrary("winmm.dll") nos recibe a nosotros, y
un GetProcAddress que falle lo tira abajo (Metro Exodus EE se reiniciaba en
bucle con solo cuatro forwarders). No se puede reenviar por nombre a
"winmm.X" -- volveria a nosotros -- asi que cada export es un stub en
ensamblador que resuelve la direccion real la primera vez y salta.

    python tools/gen_forwards.py

Lee los nombres de C:\\Windows\\System32\\winmm.dll con objdump y escribe
src/forwards_winmm.S (los stubs), src/forwards_winmm_names.h (la tabla de
nombres) y src/exports.def (para el linker).
"""
import subprocess, re, os

ROOT = r"C:\Users\matia\dev\mfg-unlock"
out = subprocess.check_output(["objdump", "-p", r"C:\Windows\System32\winmm.dll"]).decode(errors="replace")
names = sorted(set(re.findall(r"^\s+\[\s*\d+\] \+base\[\s*\d+\]\s+[0-9a-f]+\s+([A-Za-z_][A-Za-z0-9_]*)\s*$", out, re.M)))
assert len(names) > 100, len(names)

S = [
    "# Generado por tools/gen_forwards.py -- no editar a mano.",
    "# Cada export de winmm.dll: guarda su indice en eax y salta al resolvedor",
    "# comun, que llama a forward_resolve(idx) preservando los registros de",
    "# argumentos y salta a la direccion real. rax es scratch en x64, asi que",
    "# usarlo para el indice no pisa ningun argumento.",
    "    .text",
    "    .p2align 4",
    "forward_common:",
    "    push %rcx",
    "    push %rdx",
    "    push %r8",
    "    push %r9",
    "    sub $0x68, %rsp                 # shadow (0x20) + 4 xmm (0x40) + relleno: queda alineado a 16",
    "    movdqu %xmm0, 0x20(%rsp)",
    "    movdqu %xmm1, 0x30(%rsp)",
    "    movdqu %xmm2, 0x40(%rsp)",
    "    movdqu %xmm3, 0x50(%rsp)",
    "    mov %eax, %ecx",
    "    call forward_resolve",
    "    movdqu 0x20(%rsp), %xmm0",
    "    movdqu 0x30(%rsp), %xmm1",
    "    movdqu 0x40(%rsp), %xmm2",
    "    movdqu 0x50(%rsp), %xmm3",
    "    add $0x68, %rsp",
    "    pop %r9",
    "    pop %r8",
    "    pop %rdx",
    "    pop %rcx",
    "    jmp *%rax",
    "",
]
for i, n in enumerate(names):
    S += ["    .globl %s" % n, "    .def %s; .scl 2; .type 32; .endef" % n, "%s:" % n,
          "    mov $%d, %%eax" % i, "    jmp forward_common", ""]
open(os.path.join(ROOT, "src", "forwards_winmm.S"), "w", newline="\n").write("\n".join(S))

H = ["// Generado por tools/gen_forwards.py -- no editar a mano.",
     "// Los nombres de winmm.dll, en el mismo orden que los stubs de forwards_winmm.S.",
     "static const char *const kWinmmNames[] = {"] + ['    "%s",' % n for n in names] + ["};",
     "static const int kWinmmNamesN = %d;" % len(names)]
open(os.path.join(ROOT, "src", "forwards_winmm_names.h"), "w", newline="\n").write("\n".join(H) + "\n")

D = ["EXPORTS"] + names
open(os.path.join(ROOT, "src", "exports.def"), "w", newline="\n").write("\n".join(D) + "\n")
print("%d forwarders" % len(names))
