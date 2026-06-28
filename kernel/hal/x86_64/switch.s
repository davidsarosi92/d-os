; =============================================================================
; switch.s — kernel-mode context switch (x86_64).
;
; void context_switch(uint64_t* save_rsp_to, uint64_t new_rsp);
;
;   System V x86_64 ABI:
;     rdi = save_rsp_to (uint64_t*)
;     rsi = new_rsp     (uint64_t)
;   Callee-saved (must preserve): rbx, rbp, r12, r13, r14, r15.
;   (rax, rcx, rdx, rsi, rdi, r8-r11 are caller-saved — the caller of
;   context_switch already saved anything it cares about on entry.)
;
; Frame at function entry:
;   [rsp + 0]  return addr  (pushed by `call`)
;
; After 6 pushes (48 bytes):
;   [rsp + 0]  r15
;   [rsp + 8]  r14
;   ...
;   [rsp + 40] rbp
;   [rsp + 48] return addr
;
; We store that rsp into *save_rsp_to, then load rsp from new_rsp,
; pop the 6 saved regs, and `ret` — landing at whatever return address
; the new stack has on top.  For a brand-new task that's
; task_trampoline (see task_arch.c); for an established task, it's
; back into schedule() at the same point that called us.
;
; Why no 8-byte sub-rsp alignment: the call instruction's implicit
; push of the return address leaves rsp at 0 mod 16 - 8 = 8 mod 16 on
; entry.  Our 6 register pushes (6*8 = 48) make it 8+48 = 56 mod 16 =
; 8 mod 16 still — which is what the System V ABI's "8 mod 16 at fn
; entry" requirement gives the next callee.  When we eventually `ret`
; from a context_switch into a task that has not yet run, the task
; entry runs with rsp = 8 mod 16, exactly the right state for gcc-
; generated code.
; =============================================================================

bits 64
section .text
global context_switch

context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp                      ; *save_rsp_to = current rsp
    mov rsp, rsi                        ; switch to new stack

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret                                 ; jump to top-of-new-stack ret addr
