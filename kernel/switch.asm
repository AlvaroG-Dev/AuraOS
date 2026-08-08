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

    ; FPU/SSE: ambos buffers se crean alineados a 16 bytes.
    ; FX* requiere una dirección alineada a 16 bytes.
    mov rax, [rdi + 24]
    test rax, rax
    jz .save_fpu_done
    test rax, 0xF
    jnz .save_fpu_done
    fxsave64 [rax]
.save_fpu_done:

    mov rax, [rsi + 24]
    test rax, rax
    jz .restore_fpu_done
    test rax, 0xF
    jnz .restore_fpu_done
    fxrstor64 [rax]
.restore_fpu_done:

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
