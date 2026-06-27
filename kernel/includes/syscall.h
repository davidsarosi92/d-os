/* =============================================================================
 * syscall.h — minimal syscall ABI.
 *
 * Calling convention (i386):
 *   syscall number → EAX
 *   arg 0          → EBX
 *   arg 1          → ECX
 *   arg 2          → EDX
 *   trigger        → int 0x80
 *   return value   → EAX (on iret-back)
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
