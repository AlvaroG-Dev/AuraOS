; kernel/switch.asm
; Context switch para el scheduler de Aurora OS
;
; void task_switch(task_t *old_task, task_t *new_task);
;   rdi = old_task
;   rsi = new_task
;
; Guardamos/restauramos los registros CALLEE-SAVED y el estado FPU/SSE.

section .text
bits 64

global task_switch

task_switch:
    ; El cambio de contexto se ejecuta con IRQs deshabilitadas.
    cli

    ; Guardar registros callee-saved de la tarea actual.
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Guardar RSP actual antes de cambiar al estado de la nueva tarea.
    mov [rdi], rsp

    ; Guardar el estado FPU/SSE de la tarea actual.
    mov rax, [rdi + 24]
    fxsave64 [rax]

    ; Restaurar el estado FPU/SSE de la nueva tarea.
    mov rax, [rsi + 24]
    fxrstor64 [rax]

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
    ; Una tarea nueva llega aquí con IF=0. El wrapper habilita IRQs cuando
    ; el contexto C ya está establecido.
    cld
    mov rdi, r12
    call task_entry_wrapper
.hang:
    cli
    hlt
    jmp .hang
