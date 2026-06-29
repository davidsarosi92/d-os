; =============================================================================
; usermode.s — drop the CPU from ring 0 to ring 3 (and a way back).
;               x86_64 mirror of kernel/hal/x86/usermode.s.
;
; Differences from i386:
;   - System V AMD64 ABI: args in rdi/rsi (not on the stack).
;   - Callee-saved set: rbx, rbp, r12..r15 (not pushad).
;   - Interrupt return is `iretq` with a 5-quadword frame:
;       ss, rsp, rflags, cs, rip   — all 8 bytes wide.
;   - User segment selectors: 0x1B (user CS) / 0x23 (user DS).  Same
;     numerical values as i386 because the GDT slot layout matches.
;
; The SYS_EXIT teleport trick is identical: when ring 3 issues int 0x80
; with rax = SYS_EXIT, syscall_dispatch calls hal_syscall_exit_to_kernel
; which restores rsp = saved_rsp and jumps to saved_rip — landing on the
; `.return` label below and unwinding normally.
;
; The dedicated syscall stack (TSS.RSP0 → syscall_stack in tss.c) is
; what int 0x80 lands on; abandoning that stack on SYS_EXIT is fine
; because the next ring-3 → ring-0 transition resets RSP from TSS.RSP0
; again.
; =============================================================================

bits 64
section .text

global enter_user_mode_wrap
global saved_rsp
global saved_rip

; Internal storage exported so syscall.c can read them.  qwords because
; we're storing 64-bit register values.
section .data
align 8
saved_rsp: dq 0
saved_rip: dq 0

section .text

; void enter_user_mode_wrap(uintptr_t user_rip, uintptr_t user_rsp);
;   System V AMD64: rdi = user_rip, rsi = user_rsp.
enter_user_mode_wrap:
    ; Save callee-saved regs the caller expects preserved across the
    ; call.  System V AMD64: rbx, rbp, r12..r15 (6 regs, 48 B).
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Stash kernel context for the SYS_EXIT teleport.
    mov [rel saved_rsp], rsp
    lea rax, [rel .return]
    mov [rel saved_rip], rax

    ; Build a 5-quadword iretq frame.  Order (low-addr last, since
    ; iretq pops from rsp upward):
    ;   [rsp+32]  ss      = user DS = 0x23
    ;   [rsp+24]  rsp     = user_rsp
    ;   [rsp+16]  rflags  (with IF set so timer IRQs reach ring 3)
    ;   [rsp+ 8]  cs      = user CS = 0x1B
    ;   [rsp+ 0]  rip     = user_rip
    push qword 0x23                     ; SS3
    push rsi                            ; RSP3 (user stack)

    pushfq                              ; RFLAGS
    or qword [rsp], 0x200               ; set IF in the pushed copy

    push qword 0x1B                     ; CS3 (user code, RPL 3)
    push rdi                            ; RIP3 (user entry)

    iretq                               ; → ring 3

.return:
    ; Reached when hal_syscall_exit_to_kernel teleported us back.  RSP
    ; has been restored to its post-push value (right after the 6
    ; callee-saved pushes above).  Pop them and ret to the original
    ; caller — from their POV the function executed ring 3 and
    ; returned normally.
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret
