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

    ; El primer cambio de contexto ocurre desde el IRQ del PIT. No usamos
    ; FXSAVE/FXRSTOR aquí: el kernel ya inicializa SSE/FPU y el compositor
    ; usa SSE, pero preservar el estado FPU requiere un protocolo de entrada
    ;/salida de IRQ más completo que este switch voluntario. El cambio de
    ; contexto de registros enteros debe ser independiente de ese estado.

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
