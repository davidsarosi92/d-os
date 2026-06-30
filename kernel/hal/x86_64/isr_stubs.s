; =============================================================================
; isr_stubs.s — per-vector asm stubs + common interrupt prologue/epilogue
;                (x86_64).
;
; Differences from the i386 version:
;
;   1. The CPU pushes 5 quadwords on every interrupt in long mode (not 3
;      like i386's "only on privilege change"): ss, rsp, rflags, cs, rip.
;      So the interrupt stack frame is always 5*8 = 40 bytes from the
;      CPU, even for kernel-mode interrupts.
;
;   2. There are 16 general-purpose registers (rax..r15), not 8.  We
;      save 15 of them — rsp is implicit in the iretq frame.  No
;      segment-register save/restore: long mode mostly ignores ds/es/
;      fs/gs (only the *.base fields, set via MSR, are honored, and
;      those don't change on interrupt entry).
;
;   3. C handler is called with the frame pointer in rdi (first arg per
;      System V x86_64 ABI), not on the stack.
;
;   4. `iretq` instead of `iret` — the qword-width return that pops the
;      full 5-quadword frame.
;
; Stack layout by the time isr_handler runs (low address first):
;    [rsp +   0]  r15
;    [rsp +   8]  r14
;    [rsp +  16]  r13
;    [rsp +  24]  r12
;    [rsp +  32]  r11
;    [rsp +  40]  r10
;    [rsp +  48]  r9
;    [rsp +  56]  r8
;    [rsp +  64]  rdi
;    [rsp +  72]  rsi
;    [rsp +  80]  rbp
;    [rsp +  88]  rbx
;    [rsp +  96]  rdx
;    [rsp + 104]  rcx
;    [rsp + 112]  rax
;    [rsp + 120]  int_no
;    [rsp + 128]  err_code
;    [rsp + 136]  rip       \
;    [rsp + 144]  cs         |  pushed by CPU
;    [rsp + 152]  rflags     |
;    [rsp + 160]  rsp        |
;    [rsp + 168]  ss        /
;
; That matches `struct int_frame` in kernel/includes/idt.h.
;
; Stack alignment: CPU pushes 5 quadwords (40 B), stub pushes 2 + 15 =
; 17 quadwords (136 B), total 22 quadwords = 176 B.  176 % 16 == 0,
; so rsp is 16-aligned when we `call isr_handler`.  After call's
; implicit return-addr push, rsp is 8 mod 16, which is the System V
; ABI's expected state at function entry.
; =============================================================================

bits 64
section .text

extern isr_handler                      ; C dispatcher in idt.c

; -----------------------------------------------------------------------------
; ISR stub generators.  Same two-macro pattern as i386, but the immediate
; pushes are qword-sized.
; -----------------------------------------------------------------------------

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0                        ; fake error code (CPU didn't push one)
    push qword %1                       ; vector number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1                       ; vector number; CPU already pushed err
    jmp isr_common
%endmacro

; -----------------------------------------------------------------------------
; CPU exceptions 0..31.  Error-code list is identical to i386 — same
; vectors push real error codes per the SDM table.
; -----------------------------------------------------------------------------
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; -----------------------------------------------------------------------------
; IRQ stubs for vectors 32..47 (after PIC remap).  None carry an error.
; -----------------------------------------------------------------------------
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

; -----------------------------------------------------------------------------
; LAPIC vectors + MSI pool + syscall (same numbering as i386).
; -----------------------------------------------------------------------------
ISR_NOERR 64
ISR_NOERR 65
ISR_NOERR 80
ISR_NOERR 81
ISR_NOERR 82
ISR_NOERR 83
ISR_NOERR 128

; -----------------------------------------------------------------------------
; Common continuation.
; -----------------------------------------------------------------------------
isr_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Frame pointer in rdi per System V ABI.  rsp now points at the
    ; lowest field of int_frame (r15).
    mov rdi, rsp
    call isr_handler

    ; Restore GPRs in reverse push order.
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    ; Discard int_no + err_code (2 qwords = 16 bytes).
    add rsp, 16

    iretq                               ; pop CPU's 5-quadword frame, resume
