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
extern main

_start:
    call main
    mov  ebx, eax              ; exit code = main's return (SYS_EXIT reads RBX)
    mov  eax, 1                ; SYS_EXIT
    int  0x80
.hang:
    jmp  .hang                  ; unreachable
