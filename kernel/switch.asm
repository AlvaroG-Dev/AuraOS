; kernel/switch.asm
; Context switch para el scheduler de Aurora OS

section .text
bits 64

global task_switch

task_switch:
    ; Guardar registros callee-saved.
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Guardar RSP actual antes de tocar el estado de la tarea.
    mov [rdi], rsp

    ; task_t.fpu_raw comienza en offset 0x20 y el task_t devuelto por
    ; kmalloc() está alineado a 16 bytes. Por tanto el buffer también lo está.
    ; Usar el buffer embebido elimina una segunda capa de punteros que podría
    ; quedar corrupta y provocar #GP en FXSAVE/FXRSTOR.
    fxsave64 [rdi + 0x20]
    fxrstor64 [rsi + 0x20]

    ; Restaurar contexto de la nueva tarea.
    mov rsp, [rsi]

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; Punto de entrada inicial de una tarea nueva.
global task_trampoline
extern task_entry_wrapper

task_trampoline:
    ; Las tareas se crean desde un contexto de IRQ con IF=0.
    sti
    mov rdi, r12
    call task_entry_wrapper
.hang:
    hlt
    jmp .hang
