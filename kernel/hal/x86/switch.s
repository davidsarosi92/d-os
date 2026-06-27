; =============================================================================
; switch.s — kernel-mode context switch.
;
; void context_switch(uint32_t* save_esp_to, uint32_t new_esp);
;
;   1. Push the four callee-saved registers (ebx, esi, edi, ebp).  These
;      are the only ones the System V i386 ABI lets us assume the
;      caller cares about; eax / ecx / edx are clobber-able.
;   2. Store the resulting ESP into *save_esp_to (the previous task's
;      saved-ESP slot).
;   3. Load ESP from `new_esp` (the next task's saved-ESP value).
;   4. Pop the same four registers — these are now the new task's
;      callee-saved state.
;   5. `ret` — the new stack's top is the return address that was on
;      the stack when the next task last called context_switch (or, for
;      a brand-new task, the entry-point address `task_spawn` planted
;      there).
;
; Frame at function entry (cdecl):
;   [esp + 0]  return addr
;   [esp + 4]  save_esp_to (uint32_t*)
;   [esp + 8]  new_esp     (uint32_t)
;
; After 4 pushes (16 bytes):
;   [esp + 16] return addr
;   [esp + 20] save_esp_to
;   [esp + 24] new_esp
; =============================================================================

bits 32
section .text

global context_switch

context_switch:
    push ebp
    push edi
    push esi
    push ebx

    mov eax, [esp + 20]                 ; save_esp_to (uint32_t*)
    mov [eax], esp                      ; *save_esp_to = current esp

    mov esp, [esp + 24]                 ; load new_esp value into esp

    pop ebx
    pop esi
    pop edi
    pop ebp
    ret                                 ; jump to whatever return addr the
                                        ; new task's stack has on top
