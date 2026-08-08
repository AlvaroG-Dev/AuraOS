; kernel/boot.asm
; Punto de entrada en ASM para el kernel x86_64
; Args del bootloader ya estan en RDI, RSI, RDX, RCX, R8, R9 (System V AMD64)

section .text
bits 64
global _start

extern kmain

_start:
    ; El bootloader ya inicializa a cero la parte BSS de cada PT_LOAD
    ; cuando p_memsz > p_filesz. No es necesario recorrerla de nuevo aqui.
    ; Esto evita depender de __bss_start/__bss_end antes de configurar el
    ; stack y elimina un recorrido innecesario de cientos de KiB.

    ; Stack propio
    mov rsp, stack_top

    ; kmain recibe el puntero kernel_boot_info en RDI.
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
