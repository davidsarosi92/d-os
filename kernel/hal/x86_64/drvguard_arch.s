; =============================================================================
; drvguard_arch.s — §M33 Tier 0, the x86_64 half.  See the i386 file for the
; design; this one differs only in the register set and the calling convention
; (SysV: the first argument is RDI).
;
; regs[] layout:
;   [0] rbx  [1] rbp  [2] r12  [3] r13  [4] r14  [5] r15  [6] rsp  [7] return RIP
;
; The landing pad takes its argument in RDI, which the fault handler stuffs into
; the trap frame.  RDI rather than RAX because it is already the first-argument
; register here, so the pad reads like an ordinary one-argument function even
; though nothing ever calls it as one.
; =============================================================================

section .text
global drvguard_save
global drvguard_land

; int drvguard_save(uintptr_t* regs)   — regs in RDI
drvguard_save:
    mov     [rdi +  0], rbx
    mov     [rdi +  8], rbp
    mov     [rdi + 16], r12
    mov     [rdi + 24], r13
    mov     [rdi + 32], r14
    mov     [rdi + 40], r15
    ; The caller's RSP: after our own return address is popped.
    lea     rax, [rsp + 8]
    mov     [rdi + 48], rax
    mov     rax, [rsp]              ; return address = where to resume
    mov     [rdi + 56], rax
    xor     eax, eax
    ret

; void drvguard_land(void) — entered from the fault handler, regs in RDI.
drvguard_land:
    mov     rbx, [rdi +  0]
    mov     rbp, [rdi +  8]
    mov     r12, [rdi + 16]
    mov     r13, [rdi + 24]
    mov     r14, [rdi + 32]
    mov     r15, [rdi + 40]
    mov     rdx, [rdi + 56]         ; resume address (read before RSP moves)
    mov     rsp, [rdi + 48]
    mov     eax, 1
    jmp     rdx
