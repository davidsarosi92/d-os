/* =============================================================================
 * usermode.h — drop into ring 3 and back.
 *
 * `enter_user_mode_wrap` is the main entry: it saves the current kernel
 * context, drops to ring 3 at the given user EIP / ESP, and returns
 * normally when (and only when) ring 3 issues the SYS_EXIT syscall.
 *
 * If ring 3 never calls SYS_EXIT, this function never returns — the CPU
 * stays in ring 3 indefinitely (until a fault or external event).
 * ============================================================================= */

#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>

void enter_user_mode_wrap(uint32_t user_eip, uint32_t user_esp);

#endif
