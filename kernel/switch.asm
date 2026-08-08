; kernel/switch.asm
; Context switch para el scheduler de Aurora OS
;
; void task_switch(task_t *old_task, task_t *new_task);
;   rdi = old_task
;   rsi = new_task
;
; Solo guardamos/restauramos los registros CALLEE-SAVED (ABI System V x86-64):
;   rbx, rbp, r12, r13, r14, r15
; El compilador ya se encargo de guardar los caller-saved antes de llamarnos.

section .text
bits 64

global task_switch

task_switch:
    ; --- Guardar contexto de la tarea vieja (old_task = rdi) ---
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Guardar el RSP actual en old_task->rsp (offset 0 del struct)
    mov [rdi], rsp

    ; 2. Cargar el puntero FPU alineado (offset 24 / 0x18) y guardar estado
    mov rax, [rdi + 24]
    fxsave64 [rax]

    ; 3. Cargar el puntero FPU alineado de la nueva tarea y restaurar estado
    mov rax, [rsi + 24]
    fxrstor64 [rax]

    ; --- Restaurar contexto de la tarea nueva (new_task = rsi) ---
    ; Cargar RSP desde new_task->rsp
    mov rsp, [rsi]

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; El 'ret' salta a donde new_task se quedó la última vez
    ret

; Punto de entrada inicial de una tarea nueva.
global task_trampoline
extern task_entry_wrapper

task_trampoline:
    sti
    mov rdi, r12
    call task_entry_wrapper
.hang:
    hlt
    jmp .hang
