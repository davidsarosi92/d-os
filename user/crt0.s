; =============================================================================
; crt0.s — i386 user-program startup (M25 stage 7 + M34 argv).
;
; The ELF entry.  The kernel (proc.c build_initial_stack) lays out the System V
; initial stack so that at _start the stack pointer points at argc:
;
;     [esp]    = argc
;     [esp+4]  = argv[0]   (argv is &[esp+4])
;     ...      = argv[argc-1], NULL, envp..., NULL, auxv
;
; We pass (argc, argv) to main via cdecl, then SYS_EXIT with main's return.
; A `main(void)` simply ignores the two pushed args.
; =============================================================================

bits 32
section .text
global _start
global __sig_trampoline
extern main

; M34 — signal-return trampoline.  The kernel makes a signal handler `ret` here
; (it pushes this address as the handler's return address); we then issue
; SYS_SIGRETURN so the kernel restores the pre-signal context.  NO prologue —
; esp must be exactly what the kernel expects (pointing at the sig argument, with
; the saved context just above) when `int 0x80` fires.
__sig_trampoline:
    mov  eax, 21               ; SYS_SIGRETURN
    int  0x80

_start:
    mov  eax, [esp]            ; argc
    lea  edx, [esp + 4]        ; argv = &argv[0]
    push edx                   ; main arg 1 (argv)
    push eax                   ; main arg 0 (argc)
    call main
    add  esp, 8                ; pop the two args
    mov  ebx, eax              ; exit code = main's return (SYS_EXIT reads EBX)
    mov  eax, 1                ; SYS_EXIT
    int  0x80
.hang:
    jmp  .hang                  ; unreachable
