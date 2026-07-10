/* =============================================================================
 * usermode.h — drop into ring 3 and back.
 *
 * `enter_user_mode_wrap` is the main entry: it saves the current kernel
 * context, drops to ring 3 at the given user IP / SP, and returns
 * normally when (and only when) ring 3 issues the SYS_EXIT syscall.
 *
 * If ring 3 never calls SYS_EXIT, this function never returns — the CPU
 * stays in ring 3 indefinitely (until a fault or external event).
 *
 * Params are `uintptr_t` so the same prototype serves both i386
 * (32-bit) and x86_64 (64-bit).  i386 callers passing uint32_t
 * literals are source-compatible.
 * ============================================================================= */

#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>
#include <stddef.h>

void enter_user_mode_wrap(uintptr_t user_ip, uintptr_t user_sp);

/* Arch-portable ring-3/EL0 self-test — the `ringtest` shell command.  Each
 * arch implements it (x86: kernel/hal/x86/ringtest.c hand-codes an i386 ring-3
 * program; aarch64: kernel/hal/aarch64/syscall.c drops to EL0).  Returns 0 on
 * success.  This keeps shell.c free of arch-specific user-mode plumbing. */
int arch_ringtest(void);

/* Fill `buf` with this arch's minimal user "hello" program — machine code
 * that SYS_PRINTs a greeting then SYS_EXITs — laid out to load at virtual
 * address `base` (its entry is at offset 0, so a loader sets e_entry = base).
 * x86 needs `base` to encode an absolute message pointer; aarch64's stub is
 * position-independent and ignores it.  Returns the payload byte length, or 0
 * if it does not fit in `cap`.  Used by the M25 stage-2b ELF exec path
 * (proc.c) to obtain a runnable payload without a userland toolchain yet. */
size_t arch_user_hello(uint8_t* buf, size_t cap, uintptr_t base);

#endif
