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
    ; Entramos aquí con IF=0 porque el cambio inicial procede del IRQ/PIT.
    ; NO habilitamos interrupciones antes de entrar en C: hacerlo aquí permite
    ; que un IRQ anidado observe una tarea parcialmente inicializada.
    ; task_entry_wrapper habilita las interrupciones una vez establecida la
    ; entrada C y la tarea ya está marcada como RUNNING.
    mov rdi, r12
    call task_entry_wrapper
.hang:
    cli
    hlt
    jmp .hang
