/* =============================================================================
 * uaccess.h — fault-tolerant user-memory access (§1.1, the "real" hardening).
 *
 * WHY THIS EXISTS
 * ---------------
 * Validating a ring-3 pointer by walking the page tables (vmm_user_access_ok)
 * answers "is it mapped RIGHT NOW".  That is a check, not a guarantee: between
 * the check and the copy another thread of the same process can unmap the range
 * (munmap / a concurrent fork/COW teardown), and the kernel then faults with a
 * ring-0 #PF — the exact whole-box freeze we are trying to eliminate.  The
 * classic fix is an EXCEPTION TABLE: every instruction that touches user memory
 * is registered with a fixup address; when a kernel-mode fault happens at a
 * registered instruction, the fault handler resumes at the fixup instead of
 * panicking, and the copy reports -EFAULT.
 *
 * So the two layers are complementary and BOTH are kept:
 *   - vmm_user_access_ok  = the cheap up-front check (rejects the common bad
 *     pointer with a clean error, before any work is done);
 *   - the exception table = the guarantee (a fault DURING the copy — a race, a
 *     partially mapped range, a COW page revoked mid-flight — never reaches the
 *     panic path).
 *
 * HOW THE TABLE IS BUILT
 * ----------------------
 * The arch primitives (kernel/hal/<arch>/uaccess.c) wrap each user-touching
 * instruction in inline asm and emit a `struct uaccess_fixup { insn, fixup }`
 * into the `.ex_table` linker section (see linker-<arch>.ld, same KEEP()
 * pattern as the MODULE()/DRIVER() registries).  The fault handler calls
 * uaccess_fixup_lookup() before it applies any fault policy.
 * ============================================================================= */

#ifndef UACCESS_H
#define UACCESS_H

#include <stddef.h>
#include <stdint.h>

/* One exception-table entry: a fault at `insn` resumes at `fixup`. */
struct uaccess_fixup {
    uintptr_t insn;
    uintptr_t fixup;
};

/* Section bounds emitted by the linker script. */
extern struct uaccess_fixup __start_ex_table[];
extern struct uaccess_fixup __stop_ex_table[];

/* Called from an arch fault handler on a KERNEL-mode fault, with the faulting
 * instruction pointer.  If it is a registered user-access instruction, *pc is
 * rewritten to the fixup address (so the handler simply returns and the copy
 * routine reports failure) and 1 is returned.  0 = not ours; apply the normal
 * fault policy.  Lock-free and allocation-free: safe in any fault context. */
int uaccess_fixup_lookup(uintptr_t* pc);

/* ---------------------------------------------------------------------------
 * Fault-safe primitives (arch-specific).  They do NOT validate the address —
 * that is the caller's cheap pre-check — they only guarantee that a fault while
 * touching user memory returns an error instead of panicking.
 *   uaccess_copy_in   — user  → kernel, 0 / -1
 *   uaccess_copy_out  — kernel → user,  0 / -1
 *   uaccess_str_in    — NUL-terminated user string → kernel buffer (≤ max,
 *                       always NUL-terminated); returns the length, or -1.
 * ------------------------------------------------------------------------- */
int  uaccess_copy_in (void* dst, uintptr_t user_src, size_t n);
int  uaccess_copy_out(uintptr_t user_dst, const void* src, size_t n);
long uaccess_str_in  (char* dst, uintptr_t user_src, size_t max);

#endif /* UACCESS_H */
