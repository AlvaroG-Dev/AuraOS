; kernel/boot.asm
; Punto de entrada en ASM para el kernel x86_64
; Args del bootloader ya estan en RDI, RSI, RDX, RCX, R8, R9 (System V AMD64)

section .text
bits 64
global _start

extern kmain
extern __bss_start
extern __bss_end

_start:
    ; Usar direccionamiento RIP-relative para obtener los simbolos BSS.
    ; Esto evita que NASM/GAS genere una relocacion absoluta de 32 bits
    ; para direcciones higher-half del kernel.
    lea rax, [rel __bss_start]
    lea rbx, [rel __bss_end]
.bss_clear:
    cmp rax, rbx
    jae .bss_done
    mov byte [rax], 0
    inc rax
    jmp .bss_clear
.bss_done:

    ; Stack propio
    mov rsp, stack_top

    ; Alinear stack a 16 bytes (System V AMD64 ABI)
    ; call kmain empuja RIP (8 bytes), asi que rsp debe ser 16-byte aligned
    ; ANTES del call. Como stack_top ya esta alineado a 16 bytes (align 16),
    ; y call empuja 8 bytes, rsp+8 estara alineado a 16 bytes en kmain.
    ; En kmain, rsp = stack_top - 8. rsp+8 = stack_top, divisible por 16.

    ; kmain recibe args en RDI, RSI, RDX, RCX, R8, R9
    call kmain

.halt:
    cli
    hlt
    jmp .halt

section .bss
align 16
stack_bottom:
    resb 0x40000
stack_top:
