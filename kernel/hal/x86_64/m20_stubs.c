/* =============================================================================
 * m20_stubs.c — last residual stubs for symbols still missing on x86_64.
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
 *
 * Only `xhci_poll` remains — the i386 xHCI driver assumes <4 GiB DMA
 * and isn't ported yet.  pit_irq still calls xhci_poll unconditionally.
 * When the xHCI driver is ported, delete this file (and remove it from
 * the Makefile).
 * ============================================================================= */

/* ---------------------------------------------------------------------------
 * USB host (M15) — xhci_poll is called from pit_irq.  We have no
 * xHCI driver on x86_64 yet (the i386 driver assumes MMIO + DMA in
 * the low 4 GiB; needs a 64-bit revisit).  No-op until then.
 * --------------------------------------------------------------------------- */

void xhci_poll(void) { /* no USB on x86_64 yet */ }
