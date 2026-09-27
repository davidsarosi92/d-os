/* =============================================================================
 * crt0_aarch64.S — AArch64 user-program startup (Tier B libc port).
 *
 * The ELF entry: call main(), then SYS_EXIT.  Syscall ABI: x8 = number,
 * x0..x5 = args; the kernel's SVC dispatcher reads x8 = SYS_EXIT and takes the
 * exit code from x0.  main() returns its value in w0/x0, so it is already the
 * exit code — just set x8 and svc.
 *
 * argc/argv (2026-09-27): the kernel lays the SysV initial stack out here as
 * on x86 — [sp] = argc, [sp+8..] = argv[] — and musl programs have always read
 * it; this crt0 did not, so every in-tree program on ARM ran with whatever x0
 * and x1 happened to hold and silently took its DEFAULT mode (found by
 * `evicttest`: `memhog verify` ran as the plain hog; `memhog fill` had done the
 * same since §M72 stage 1).  x86_64's crt0 had the identical gap (§4.39).
 * ============================================================================= */

.section .text
.global _start
.global __sig_trampoline
.global __thread_exit_tramp

/* Signals (M34) and threads (M35) are i386-only today, so these trampolines are
 * never actually invoked here — but the shared user/libc.c references them (a
 * sigaction restorer / a thread function's return address), so the symbols must
 * exist for the AArch64 user ELFs to link at all.  They mirror the x86 versions
 * with the AArch64 SVC convention (x8 = number, x0 = arg0), so they are already
 * correct if those features are brought to this arch. */
__sig_trampoline:
    mov     x8, #21             /* SYS_SIGRETURN */
    svc     #0
1:  b       1b                  /* unreachable */

__thread_exit_tramp:
    mov     x8, #1              /* SYS_EXIT — the thread fn's return value is
                                 * already in x0, i.e. the exit code */
    svc     #0
2:  b       2b                  /* unreachable */

_start:
    ldr     x0, [sp]            /* argc                                    */
    add     x1, sp, #8          /* argv                                    */
    bl      main                /* main() → return value in x0 (= exit code) */
    mov     x8, #1              /* SYS_EXIT */
    svc     #0
0:  b       0b                  /* unreachable */
