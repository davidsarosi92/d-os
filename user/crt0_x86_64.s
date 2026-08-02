; =============================================================================
; crt0_x86_64.s — x86_64 user-program startup (Tier B libc port).
;
; The ELF entry.  The kernel (proc.c build_initial_stack) lays out the System V
; initial stack so that at _start the stack pointer points at argc:
;
;     [rsp]    = argc
;     [rsp+8]  = argv[0]   (argv is &[rsp+8])
;     ...      = argv[argc-1], NULL, envp..., NULL, auxv
;
; That layout is already 64-bit clean (the kernel writes uintptr_t slots), so
; the ONLY thing that was missing here was reading it: this crt0 used to call
; main() with whatever happened to be in RDI/RSI, which made every argv-using
; program on x86_64 read a pointer as argc and then walk off the end.
;
; amd64 SysV: arg0 = RDI, arg1 = RSI, and RSP must be 16-byte aligned at the
; `call` (so the callee sees RSP ≡ 8 mod 16).  build_initial_stack already
; 16-aligns the initial SP, so no re-alignment is needed here.
;
; The kernel's int-0x80 dispatcher reads RAX = number, RBX = arg0, so the exit
; code goes in RBX (the kernel reads f->rbx).
; =============================================================================

bits 64
section .text
global _start
global __sig_trampoline
global __thread_exit_tramp
global __thread_start
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

; M35 — thread entry trampoline.  SYS_CLONE only carries (entry, stack), so the
; thread function and its argument travel on the new thread's stack.  On i386
; that is already the calling convention (the argument IS a stack slot) and the
; kernel can start `fn` directly; amd64 SysV passes the argument in RDI, so a
; thread started that way ran with whatever RDI happened to hold — the symptom
; was a thread reading a garbage "tid" rather than any visible crash.
;
; libc's thread_create lays the stack out as:
;     [rsp]    = __thread_exit_tramp   (fn's return address)
;     [rsp+8]  = arg
;     [rsp+16] = fn
; We move the argument into RDI and JUMP (not call) to fn, so fn's own `ret`
; lands on the exit trampoline already sitting at [rsp].
__thread_start:
    mov  rdi, [rsp + 8]        ; arg
    mov  rax, [rsp + 16]       ; fn
    jmp  rax

_start:
    mov  rdi, [rsp]            ; arg0 = argc
    lea  rsi, [rsp + 8]        ; arg1 = argv
    call main
    mov  ebx, eax              ; exit code = main's return (SYS_EXIT reads RBX)
    mov  eax, 1                ; SYS_EXIT
    int  0x80
.hang:
    jmp  .hang                  ; unreachable
