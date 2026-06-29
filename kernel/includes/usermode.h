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

void enter_user_mode_wrap(uintptr_t user_ip, uintptr_t user_sp);

#endif
