; =============================================================================
; drvguard_arch.s — §M33 Tier 0, the i386 half.
;
; Two routines, and between them they are a setjmp/longjmp pair split across an
; exception:
;
;   int  drvguard_save(uintptr_t* regs)   — saves, returns 0
;   void drvguard_land(void)              — entered FROM THE FAULT HANDLER with
;                                           the regs pointer in EAX; restores
;                                           and returns 1 from drvguard_save
;
; WHY THE RETURN PATH GOES THROUGH A LANDING PAD RATHER THAN THE TRAP FRAME.
; A same-privilege `iret` on i386 pops only EIP/CS/EFLAGS — the stack pointer is
; whatever the handler leaves.  The `pusha` slot for ESP in `struct int_frame`
; is the one `popa` throws away, so there is no way to restore the faulting
; stack by editing the frame.  The handler therefore returns to this pad, still
; on the interrupted stack, and the pad switches ESP itself.
;
; (x86_64 would not need the indirection — `iretq` always reloads RSP — but it
; uses the same shape anyway, so the three ports read alike and a reader who
; has understood one has understood all three.)
;
; regs[] layout — SHARED WITH drvguard.h, and changing the order here means
; changing nothing there but the comment, which is exactly the kind of coupling
; worth stating out loud:
;   [0] ebx   [1] esi   [2] edi   [3] ebp   [4] esp   [5] return EIP
; =============================================================================

section .text
global drvguard_save
global drvguard_land

; int drvguard_save(uintptr_t* regs)   — cdecl: regs at [esp+4]
drvguard_save:
    mov     ecx, [esp + 4]          ; ecx = regs
    mov     [ecx +  0], ebx
    mov     [ecx +  4], esi
    mov     [ecx +  8], edi
    mov     [ecx + 12], ebp
    ; The ESP we must come back to is the caller's — i.e. after this function's
    ; own return address is popped.  Saving the value at entry would land the
    ; pad four bytes low and every later access would be off by one slot.
    lea     eax, [esp + 4]
    mov     [ecx + 16], eax
    mov     eax, [esp]              ; our return address = where to resume
    mov     [ecx + 20], eax
    xor     eax, eax                ; first time through: 0
    ret

; void drvguard_land(void) — NOT called from C.  The fault handler sets
; frame.eip to this address and frame.eax to the regs pointer, then returns
; from the exception; execution arrives here on the interrupt's stack.
drvguard_land:
    mov     ecx, eax                ; ecx = regs (eax is about to become 1)
    mov     ebx, [ecx +  0]
    mov     esi, [ecx +  4]
    mov     edi, [ecx +  8]
    mov     ebp, [ecx + 12]
    mov     esp, [ecx + 16]         ; back onto the guarded call's stack
    mov     edx, [ecx + 20]         ; the resume address
    mov     eax, 1                  ; drvguard_save() "returns" 1 this time
    jmp     edx
