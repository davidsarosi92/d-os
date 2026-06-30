/* =============================================================================
 * m20_stubs.c — empty after M20.6.2/3.
 *
 * History (each phase shrinks this file):
 *   - M20 Phase 5: stubbed lapic_*, ioapic_*, smp_*, syscall_dispatch,
 *     enter_user_mode_wrap, xhci_poll so kernel_main can link.
 *   - M20.5 Phase A: lapic_*, ioapic_* dropped (shared from
 *     kernel/hal/x86/ now).
 *   - M20.5 Phase B: smp_boot_aps + smp_set_lapic_timer_count dropped
 *     (real impl in kernel/hal/x86_64/smp.c).
 *   - M20.5 Phase C: syscall_dispatch + enter_user_mode_wrap dropped
 *     (real impl in kernel/hal/x86_64/syscall.c + usermode.s).
 *   - M20.6.2/3: xhci_poll dropped (the i386 xHCI driver now compiles
 *     for x86_64 — see Makefile + DOCS.md §8 change log).  Same audit
 *     applied to virtio_blk; both DMAs stay <4 GiB until M19.5.1
 *     populates HIGHMEM.
 *
 * The file is kept as a placeholder so future ports have a single
 * known location to graveyard arch-bring-up stubs.  Delete when the
 * project gains a sustained period without bring-up stubs.
 * ============================================================================= */

/* Intentionally empty. */
