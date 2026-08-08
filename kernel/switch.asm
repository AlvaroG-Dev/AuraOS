; kernel/switch.asm
; Context switch para el scheduler de Aurora OS

section .text
bits 64

global task_switch

task_switch:
    ; El cambio de contexto debe ser atómico respecto a IRQs. Esto también
    ; garantiza que una tarea recién creada llegue al trampoline con IF=0,
    ; independientemente de si el switch fue provocado por PIT o yield().
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
    ; IF permanece desactivado hasta que task_entry_wrapper haya establecido
    ; el contexto C de forma segura.
    cld
    mov rdi, r12
    call task_entry_wrapper
.hang:
    cli
    hlt
    jmp .hang
