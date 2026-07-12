; =============================================================================
; crt0_x86_64.s — x86_64 user-program startup (Tier B libc port).
;
; The ELF entry: call main(), then SYS_EXIT with main's return value as the
; exit code.  The kernel's int-0x80 dispatcher reads RAX = number, RBX = arg0,
; so the exit code goes in RBX (the kernel reads f->rbx).  No argc/argv/env.
; =============================================================================

bits 64
section .text
global _start
global __sig_trampoline
global __thread_exit_tramp
extern main

; Signals (M34) and threads (M35) are i386-only today, so these trampolines are
; never actually invoked on x86_64 — but the shared user/libc.c references them
; (sigaction's restorer / a thread fn's return address), so the symbols must
; exist for the x86_64 user ELFs to link.  They mirror the i386 crt0 versions
; with the x86_64 int-0x80 convention (RAX = number, RBX = arg0), so they are
; already correct if these features are ever brought to x86_64.
__sig_trampoline:
    mov  eax, 21               ; SYS_SIGRETURN
    int  0x80

__thread_exit_tramp:
    mov  ebx, eax              ; exit code = thread fn's return value
    mov  eax, 1                ; SYS_EXIT
    int  0x80

_start:
    call main
    mov  ebx, eax              ; exit code = main's return (SYS_EXIT reads RBX)
    mov  eax, 1                ; SYS_EXIT
    int  0x80
.hang:
    jmp  .hang                  ; unreachable
