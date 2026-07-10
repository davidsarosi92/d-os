; =============================================================================
; crt0.s — i386 user-program startup (M25 stage 7, in-tree libc).
;
; The ELF entry.  Calls main(), then issues SYS_EXIT (which the kernel's
; syscall dispatcher turns into a teleport back to proc_exec_elf's caller).
; No argc/argv/env yet — main() takes no arguments.
; =============================================================================

bits 32
section .text
global _start
extern main

_start:
    call main
    mov  ebx, eax              ; exit code = main's return (SYS_EXIT reads EBX)
    mov  eax, 1                ; SYS_EXIT
    int  0x80
.hang:
    jmp  .hang                  ; unreachable
