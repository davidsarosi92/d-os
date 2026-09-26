/* =============================================================================
 * cowref.h — the copy-on-write reference count, ATOMICALLY (2026-09-26).
 *
 * Every arch's VMM keeps a 16-bit count per frame: how many address spaces
 * share it copy-on-write.  All three updated it with plain `(*rc)--` /
 * `*rc = ...`, and a forked parent and child run concurrently on two CPUs —
 * the child exiting (dropping its share) while the parent takes a COW fault on
 * the same frame is the ordinary case, not an exotic one.  A lost update there
 * is a frame freed twice (both believe they were last) or never; the first puts
 * a frame on the buddy free list twice, which makes the list a CYCLE, and the
 * next walk of it runs forever with the zone lock held and interrupts off.
 * That is what `excstorm` (eight concurrent fork/exit families) found on
 * x86_64: an NMI hard lockup in buddy_free_in_zone, and user programs faulting
 * on their own code pages.
 *
 * Three operations, each one indivisible:
 *   cow_ref_share     fork shares the frame: 0 (one untracked holder) -> 2,
 *                     n -> n+1.
 *   cow_ref_drop      a holder leaves WITHOUT copying (teardown): returns 1
 *                     when it was the last holder, i.e. must free the frame.
 *   cow_ref_put_copy  a holder leaves AFTER copying (COW fault): the count is
 *                     dropped only once the copy is complete — dropping first
 *                     would let the other holder take the frame writable and
 *                     change it under the copy.  Returns 1 when the copier
 *                     turned out to be the last holder and must free it.
 * ============================================================================= */
#ifndef COWREF_H
#define COWREF_H

#include <stdint.h>
#include "lock.h"
#include "pmm.h"

/* THE TABLE ITSELF, BUILT ONCE (2026-09-26).  Every arch built it lazily on
 * first use with no lock: `if (!tbl) { tbl = alloc; zero it; }`.  Two CPUs
 * forking for the first time together (`excstorm` right after boot) both saw
 * NULL, or one saw the pointer the other had published BEFORE zeroing it, and
 * then counted from whatever bootmem held — the double frees behind an NMI in
 * zone_remove walking a cyclic free list.  One lock around the build, and the
 * pointer published with RELEASE only after the table is zeroed. */
static inline uint16_t* cow_table_get(uint16_t** tbl, uint32_t* nr, spinlock_t* lk) {
    uint16_t* t = __atomic_load_n(tbl, __ATOMIC_ACQUIRE);
    if (t) return t;
    uint32_t fl = spin_lock_irqsave(lk);
    t = *tbl;
    if (!t) {
        uint32_t n = pmm_nr_frames;
        uint16_t* a = (uint16_t*)pmm_bootmem_alloc(n * (uint32_t)sizeof(uint16_t));
        if (a) {
            for (uint32_t i = 0; i < n; i++) a[i] = 0;
            *nr = n;
            __atomic_store_n(tbl, a, __ATOMIC_RELEASE);
            t = a;
        }
    }
    spin_unlock_irqrestore(lk, fl);
    return t;
}

static inline void cow_ref_share(uint16_t* rc) {
    uint16_t o = __atomic_load_n(rc, __ATOMIC_ACQUIRE);
    for (;;) {
        uint16_t n = (uint16_t)(o == 0 ? 2 : o + 1);
        if (__atomic_compare_exchange_n(rc, &o, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return;
    }
}

static inline int cow_ref_drop(uint16_t* rc) {
    uint16_t o = __atomic_load_n(rc, __ATOMIC_ACQUIRE);
    for (;;) {
        if (o <= 1) { __atomic_store_n(rc, 0, __ATOMIC_RELEASE); return 1; }
        if (__atomic_compare_exchange_n(rc, &o, (uint16_t)(o - 1), 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 0;
    }
}

/* Is the caller (probably) the only holder?  A snapshot: the fault path uses
 * it to choose "write in place" — which is only ever right when it is 1 or 0,
 * and then nobody else exists to change it. */
static inline int cow_ref_sole(const uint16_t* rc) {
    return __atomic_load_n(rc, __ATOMIC_ACQUIRE) <= 1;
}

static inline int cow_ref_put_copy(uint16_t* rc) {
    return cow_ref_drop(rc);
}

#endif
