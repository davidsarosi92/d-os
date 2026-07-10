/* =============================================================================
 * syscall.h — minimal syscall ABI.
 *
 * Calling convention — arch-specific registers, shared numbers:
 *   i386 / x86_64:  number → EAX/RAX, args → EBX/RBX.., trigger `int 0x80`,
 *                   return → EAX/RAX (on iret-back).
 *   aarch64:        number → x8, args → x0..x5, trigger `svc #0`,
 *                   return → x0 (on eret-back).
 * Each arch has its own dispatcher (kernel/hal/<arch>/syscall.c) that reads its
 * trapframe; only the numbers below are shared.
 *
 * Syscall numbers (kept tiny on purpose; this is a teaching set):
 *   0  SYS_PRINT  EBX = const char* — print null-terminated string to console
 *   1  SYS_EXIT  — return to the wrap caller in kernel mode (M6 plumbing)
 *
 * The `print` syscall reads the string from a user-mode address; the
 * kernel walks it directly (we still have the identity map for the
 * kernel address range and the user code's pages are USER-mapped, so
 * supervisor reads work).
 * ============================================================================= */

#ifndef SYSCALL_H
#define SYSCALL_H

#define SYS_PRINT   0
#define SYS_EXIT    1

struct int_frame;
void syscall_dispatch(struct int_frame* f);

#endif
