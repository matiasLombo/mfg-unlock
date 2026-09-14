# Generado por tools/gen_version_forwards.py -- no editar a mano.
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

    .globl GetFileVersionInfoA
    .def GetFileVersionInfoA; .scl 2; .type 32; .endef
GetFileVersionInfoA:
    mov $0, %eax
    jmp forward_common

    .globl GetFileVersionInfoByHandle
    .def GetFileVersionInfoByHandle; .scl 2; .type 32; .endef
GetFileVersionInfoByHandle:
    mov $1, %eax
    jmp forward_common

    .globl GetFileVersionInfoExA
    .def GetFileVersionInfoExA; .scl 2; .type 32; .endef
GetFileVersionInfoExA:
    mov $2, %eax
    jmp forward_common

    .globl GetFileVersionInfoExW
    .def GetFileVersionInfoExW; .scl 2; .type 32; .endef
GetFileVersionInfoExW:
    mov $3, %eax
    jmp forward_common

    .globl GetFileVersionInfoSizeA
    .def GetFileVersionInfoSizeA; .scl 2; .type 32; .endef
GetFileVersionInfoSizeA:
    mov $4, %eax
    jmp forward_common

    .globl GetFileVersionInfoSizeExA
    .def GetFileVersionInfoSizeExA; .scl 2; .type 32; .endef
GetFileVersionInfoSizeExA:
    mov $5, %eax
    jmp forward_common

    .globl GetFileVersionInfoSizeExW
    .def GetFileVersionInfoSizeExW; .scl 2; .type 32; .endef
GetFileVersionInfoSizeExW:
    mov $6, %eax
    jmp forward_common

    .globl GetFileVersionInfoSizeW
    .def GetFileVersionInfoSizeW; .scl 2; .type 32; .endef
GetFileVersionInfoSizeW:
    mov $7, %eax
    jmp forward_common

    .globl GetFileVersionInfoW
    .def GetFileVersionInfoW; .scl 2; .type 32; .endef
GetFileVersionInfoW:
    mov $8, %eax
    jmp forward_common

    .globl VerFindFileA
    .def VerFindFileA; .scl 2; .type 32; .endef
VerFindFileA:
    mov $9, %eax
    jmp forward_common

    .globl VerFindFileW
    .def VerFindFileW; .scl 2; .type 32; .endef
VerFindFileW:
    mov $10, %eax
    jmp forward_common

    .globl VerInstallFileA
    .def VerInstallFileA; .scl 2; .type 32; .endef
VerInstallFileA:
    mov $11, %eax
    jmp forward_common

    .globl VerInstallFileW
    .def VerInstallFileW; .scl 2; .type 32; .endef
VerInstallFileW:
    mov $12, %eax
    jmp forward_common

    .globl VerLanguageNameA
    .def VerLanguageNameA; .scl 2; .type 32; .endef
VerLanguageNameA:
    mov $13, %eax
    jmp forward_common

    .globl VerLanguageNameW
    .def VerLanguageNameW; .scl 2; .type 32; .endef
VerLanguageNameW:
    mov $14, %eax
    jmp forward_common

    .globl VerQueryValueA
    .def VerQueryValueA; .scl 2; .type 32; .endef
VerQueryValueA:
    mov $15, %eax
    jmp forward_common

    .globl VerQueryValueW
    .def VerQueryValueW; .scl 2; .type 32; .endef
VerQueryValueW:
    mov $16, %eax
    jmp forward_common
