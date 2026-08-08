/* =============================================================================
 * tlb.c — cross-CPU TLB shootdown for both x86 architectures (§M51).
 *
 * WHY THIS HAS TO EXIST.  x86 does not broadcast TLB invalidation.  `invlpg`
 * and a CR3 reload affect the CPU that executes them and nothing else, so a
 * page-table edit made on one core leaves every other core still translating
 * the OLD entry until something evicts it.  AArch64 has `tlbi ...is`, which is
 * a broadcast in hardware and is why the ARM port needed no equivalent of this
 * file; on x86 the broadcast is an inter-processor interrupt you write
 * yourself.
 *
 * The hole this closes is not theoretical.  Copy-on-write is a page-table edit
 * whose entire safety argument is "the next write faults":
 *
 *     fork() marks the parent's pages read-only, so the parent's own next
 *     write traps and gets a private copy.
 *
 * If the parent is also runnable on a second core — which is exactly the case
 * between `fork` and the child's `execve` — that core still holds a WRITABLE
 * entry for the very pages we just protected.  Its next write does not fault.
 * It goes straight into the frame the child is now sharing, and the two
 * processes silently scribble on each other.  No fault, no log; just a program
 * that works at `-smp 1` and dies at `-smp 2`.
 *
 * THE PROTOCOL.  A ticket pair per CPU (`percpu.tlb_req` / `tlb_ack`) and
 * nothing else — no lock, no shared request slot, no message:
 *
 *   sender   flush locally, then for every other online CPU take a ticket
 *            (atomic ++tlb_req), send the IPI, and wait for that CPU's
 *            tlb_ack to reach the ticket.
 *   target   flush everything, then publish tlb_ack = tlb_req.
 *
 * Three properties fall out of that shape, and each one is load-bearing:
 *
 *   1. THE REQUEST CARRIES NO ADDRESS.  The remote action is always a full
 *      flush.  That makes overlapping requests from different senders
 *      harmless — there is no per-request state to clobber — which is what
 *      lets the whole thing run without a lock.  An `invlpg` of a specific VA
 *      would be cheaper and would need a request slot, a lock around it, and
 *      an answer to "what if two CPUs are shooting down at once".  On a path
 *      that already costs an IPI round trip, that trade is not worth making.
 *
 *   2. `tlb_ack` IS PUBLISHED AFTER THE FLUSH, never before.  The counter is
 *      the promise; ordering it the other way would let a sender proceed while
 *      the stale entry was still live.
 *
 *   3. THE WAIT LOOP SERVICES ITS OWN SLOT.  This is the deadlock fix, and it
 *      is not optional.  A shootdown can be issued from a page-fault handler,
 *      i.e. with interrupts disabled, so a waiting CPU cannot take the IPI
 *      that another waiting CPU is waiting on.  Two CPUs shooting down at the
 *      same time would wait for each other forever.  Checking our own ticket
 *      in the spin loop means we honour a peer's request whether or not we can
 *      currently take an interrupt, and the cycle cannot form.
 *
 * UNIPROCESSOR IS FREE: with one CPU online there is no remote work at all, so
 * this compiles down to the local invalidation that was always there.
 * ============================================================================= */

#include "percpu.h"
#include "smp.h"
#include "acpi.h"      /* ACPI_MAX_CPUS — the per-CPU table's own bound */
#include "hal_api.h"
#include <stdint.h>

void lapic_send_ipi(uint8_t target_apic_id, uint8_t vector);   /* lapic.c */

/* Must match the gate installed in idt.c and the stub in isr_stubs.s. */
#define TLB_IPI_VECTOR  0x42

static inline uintptr_t read_cr3_local(void) {
    uintptr_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline void write_cr3_local(uintptr_t v) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(v) : "memory");
}

/* Drop every non-global entry on THIS CPU.  Reloading CR3 with its own value
 * is the portable way to say that on both i386 and x86_64 (we do not set
 * CR4.PGE, so nothing is global and nothing survives). */
static inline void flush_local(void) {
    write_cr3_local(read_cr3_local());
}

/* Service a pending request aimed at this CPU, if any.  Called both from the
 * IPI handler and from the shootdown wait loop — see property 3 above. */
void x86_tlb_service_local(void) {
    struct percpu* me = this_cpu();
    if (!me) return;
    uint32_t req = __atomic_load_n(&me->tlb_req, __ATOMIC_ACQUIRE);
    if (req == __atomic_load_n(&me->tlb_ack, __ATOMIC_RELAXED)) return;
    flush_local();
    /* Publish only now: the ack is the promise that the flush has happened. */
    __atomic_store_n(&me->tlb_ack, req, __ATOMIC_RELEASE);
}

/* The vector-0x42 handler (called from isr_handler on both arches). */

void x86_tlb_ipi(void) {
    x86_tlb_service_local();
}

/* Invalidate `va` (or, with va == 0, everything) in the address space rooted at
 * `root_phys`, on this CPU and on every other online CPU.
 *
 * `root_phys` is a CR3 value rather than a `struct vmm_space*` so this file
 * needs to know nothing about either arch's page-table types — the two VMMs
 * have different structures and the same CR3. */
void hal_tlb_shootdown(uintptr_t root_phys, uintptr_t va) {
    /* Local half first.  On the CPU making the edit we can afford to be
     * precise, and this is the ONLY work on a uniprocessor box. */
    if (root_phys == 0 || read_cr3_local() == root_phys) {
        if (va) __asm__ volatile ("invlpg (%0)" :: "r"(va) : "memory");
        else    flush_local();
    }

    int n = smp_ncpus();
    if (n <= 1) return;
    int self = this_cpu_id();

    /* Take a ticket on every peer, then chase them all.  Tickets are taken up
     * front so the IPIs overlap instead of serialising one CPU at a time. */
    uint32_t ticket[ACPI_MAX_CPUS];
    for (int i = 0; i < n && i < ACPI_MAX_CPUS; i++) {
        struct percpu* p = percpu_at(i);
        ticket[i] = 0;
        if (i == self || !p || !p->online) continue;
        ticket[i] = __atomic_add_fetch(&p->tlb_req, 1, __ATOMIC_ACQ_REL);
        lapic_send_ipi(p->apic_id, TLB_IPI_VECTOR);
    }

    for (int i = 0; i < n && i < ACPI_MAX_CPUS; i++) {
        struct percpu* p = percpu_at(i);
        if (!ticket[i] || !p) continue;
        while ((int32_t)(__atomic_load_n(&p->tlb_ack, __ATOMIC_ACQUIRE)
                         - ticket[i]) < 0) {
            x86_tlb_service_local();
            __asm__ volatile ("pause" ::: "memory");
        }
    }
}
