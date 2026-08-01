/* =============================================================================
 * uaccess.c — portable half of the fault-tolerant user-access layer (§1.1).
 *
 * Only the table lookup lives here; the instructions that actually touch user
 * memory (and the .ex_table entries that describe them) are arch-specific and
 * live in kernel/hal/<arch>/uaccess.c.  See uaccess.h for the design.
 * ============================================================================= */

#include "uaccess.h"

/* Linear scan of the exception table.  The table has a handful of entries (one
 * per user-touching instruction in the arch primitives), so a scan is cheaper
 * than any index — and, unlike a sorted/binary search, it needs no construction
 * step at boot and no assumptions about link order.  MUST stay lock-free and
 * allocation-free: it runs inside a page-fault handler. */
int uaccess_fixup_lookup(uintptr_t* pc) {
    if (!pc) return 0;
    for (const struct uaccess_fixup* e = __start_ex_table; e < __stop_ex_table; e++) {
        if (e->insn == *pc) { *pc = e->fixup; return 1; }
    }
    return 0;
}
