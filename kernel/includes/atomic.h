/* =============================================================================
 * atomic.h — portable atomic primitives.
 *
 * Built on top of GCC's `__sync_*` / `__atomic_*` builtins, which emit
 * the right `lock`-prefixed instructions on x86 and the equivalent
 * `dmb`/`ldrex`/`strex` on ARM.  Wrapping them here gives the rest of
 * the kernel a single API surface and lets us swap implementations
 * (intrinsics → inline asm) without touching call sites.
 *
 * Semantics (matches C11 memory model):
 *   - `atomic_cmpxchg`            : full barrier; returns 1 on success.
 *   - `atomic_fetch_add`          : full barrier; returns the OLD value.
 *   - `atomic_load_relaxed`       : single read, no ordering.
 *   - `atomic_store_release`      : pairs with load_acquire on the matching
 *                                   reader; ensures every prior store is
 *                                   visible before this one.
 *   - `smp_mb` / `smp_rmb` / `smp_wmb` : memory fences.
 *
 * UP correctness: on a single CPU these compile to (mostly) the same
 * thing, but the lock prefix is harmless and the barriers prevent
 * compiler reordering that would also break UP code under IRQ races.
 * Don't strip them off "because we're UP today" — the same code runs
 * on SMP starting M18.
 * ============================================================================= */

#ifndef ATOMIC_H
#define ATOMIC_H

#include <stdint.h>

/* Compare *p with `old`; if equal, store `new` and return 1; otherwise
 * leave *p unchanged and return 0.  Full memory barrier. */
static inline int atomic_cmpxchg(volatile uint32_t* p, uint32_t old, uint32_t new_) {
    return __sync_bool_compare_and_swap(p, old, new_);
}

/* Atomically *p += v; returns the OLD *p.  Full memory barrier. */
static inline uint32_t atomic_fetch_add(volatile uint32_t* p, uint32_t v) {
    return __sync_fetch_and_add(p, v);
}

/* Atomically *p = v; full memory barrier. */
static inline void atomic_store(volatile uint32_t* p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

/* Single relaxed read — no ordering implied; just guarantees the read
 * happens (i.e. compiler doesn't fold it away).  Use for diagnostic
 * reads (`/proc/`-style counters) where torn-read tolerance is OK. */
static inline uint32_t atomic_load_relaxed(const volatile uint32_t* p) {
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

/* Acquire / release pair.  Producer writes its data, then does
 * `atomic_store_release(&flag, 1)`.  Consumer does
 * `atomic_load_acquire(&flag)` and once it sees 1, every prior store
 * by the producer is visible to it. */
static inline void atomic_store_release(volatile uint32_t* p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static inline uint32_t atomic_load_acquire(const volatile uint32_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

/* Fences — most code shouldn't need these, but they make some lock-free
 * patterns explicit (e.g. publishing a per-CPU pointer that another
 * CPU may race-read). */
static inline void smp_mb (void) { __sync_synchronize(); }
static inline void smp_rmb(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
static inline void smp_wmb(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }

#endif
