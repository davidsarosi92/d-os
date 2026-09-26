/* =============================================================================
 * vmm_account.c — what a process's memory costs, decided in ONE place (§M75).
 *
 * See vmm.h's "§M75 — what does a space actually hold?" note for why this is a
 * portable file over three arch-specific walkers rather than a counter inside
 * each `vmm.c`.  The short version: `vmm.c` exists three times, three counters
 * drift, and the drift is invisible on whichever arch nobody is running.
 *
 * THIS FILE IS THE POLICY.  It is the only place in the tree that decides what
 * a mapped page MEANS for accounting, so the three architectures cannot answer
 * the question differently.
 * ============================================================================= */

#include "vmm.h"

/* One accumulator, threaded through the walk. */
struct resident_acc {
    uint64_t priv;      /* bytes this space alone owns  */
    uint64_t shared;    /* bytes it maps but shares      */
};

/* ---------------------------------------------------------------------------
 * The policy, stated once.
 *
 * A page is PRIVATE to this space when nothing else has a claim on its frame:
 *
 *   VMM_SHARED  — a BORROWED frame (an shm object / memfd).  Its owner frees
 *                 it, and it is mapped by everyone who opened the object, so
 *                 counting it here would count one frame N times.  Never
 *                 private.
 *
 *   VMM_COW     — shared read-only after a fork.  **The refcount decides, not
 *                 the bit.**  `vmm_cow_fault` leaves the flag set on the last
 *                 sharer's entry until it faults, and `vmm_space_destroy`
 *                 already treats "COW with a count of 1" as "mine to free" —
 *                 so a COW page whose share count is 0 or 1 IS this space's
 *                 private page, and calling it shared would under-report every
 *                 process that has forked and not yet been written to.
 *
 *   anything else — an ordinary owned user page.
 *
 * The test that keeps this honest is arithmetic rather than opinion: the sum
 * of every space's PRIVATE bytes must not exceed the machine's memory, because
 * "private" means "would be handed back on exit" and two spaces cannot both
 * hand back the same frame.
 * ------------------------------------------------------------------------- */
static void resident_visit(void* ctx, uintptr_t va, uint64_t phys,
                           uint32_t flags) {
    struct resident_acc* a = (struct resident_acc*)ctx;
    (void)va;

    if (flags & VMM_SHARED) { a->shared += 4096; return; }

    if (flags & VMM_COW) {
        /* > 1 means at least one other space is still holding this frame. */
        if (vmm_frame_share_count(phys) > 1) { a->shared += 4096; return; }
    }

    a->priv += 4096;
}

void vmm_space_resident(struct vmm_space* space,
                        uint64_t* out_private, uint64_t* out_shared) {
    struct resident_acc a = { 0, 0 };

    /* A kernel thread has no private user region.  Zero is the ANSWER here,
     * not a failure to measure — and the caller can tell the two apart because
     * it knows whether it passed NULL. */
    if (space) vmm_space_walk(space, resident_visit, &a);

    if (out_private) *out_private = a.priv;
    if (out_shared)  *out_shared  = a.shared;
}
