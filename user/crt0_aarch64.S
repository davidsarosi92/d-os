/* =============================================================================
 * crt0_aarch64.S — AArch64 user-program startup (Tier B libc port).
 *
 * The ELF entry: call main(), then SYS_EXIT.  Syscall ABI: x8 = number,
 * x0..x5 = args; the kernel's SVC dispatcher reads x8 = SYS_EXIT and takes the
 * exit code from x0.  main() returns its value in w0/x0, so it is already the
 * exit code — just set x8 and svc.  No argc/argv/env.
 * ============================================================================= */

.section .text
.global _start
_start:
    bl      main                /* main() → return value in x0 (= exit code) */
    mov     x8, #1              /* SYS_EXIT */
    svc     #0
0:  b       0b                  /* unreachable */
