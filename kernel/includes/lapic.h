/* =============================================================================
 * lapic.h — Local APIC interface.
 *
 * One LAPIC per CPU; the BSP's LAPIC is the same hardware accessed
 * through the same MMIO window (each CPU sees ITS own LAPIC at that
 * address — the chipset routes the read/write to the local unit).
 *
 * M18 first cut uses it for:
 *   - IRQ delivery (after IOAPIC takes over from the 8259)
 *   - EOI acknowledgement (`lapic_eoi`)
 *   - Inter-Processor Interrupts for AP bring-up (INIT + SIPI)
 *
 * IRQ handlers reach `lapic_eoi` instead of the legacy `pic_eoi` once
 * IOAPIC routing is active.  Vector layout stays compatible with the
 * IDT we already have: IRQ N → vector 0x20+N.
 * ============================================================================= */

#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

/* Map the LAPIC's MMIO window at `phys` and enable the local unit
 * (SIVR.APIC_EN = 1).  Must be called once on the BSP before IOAPIC
 * routing is activated.  Idempotent across CPUs — APs call the
 * "enable-only" variant once they're up. */
int  lapic_init_bsp(uint32_t phys);

/* Per-CPU APIC-enable for an AP (assumes lapic_init_bsp already
 * mapped the MMIO window).  Sets SIVR.APIC_EN, masks LVT lines. */
void lapic_init_ap(void);

/* Acknowledge the current IRQ to the LAPIC.  Replaces pic_eoi for
 * vectors delivered via IOAPIC. */
void lapic_eoi(void);

/* This CPU's APIC ID — read from LAPIC ID register (offset 0x20). */
uint8_t lapic_id(void);

/* Send INIT IPI to the target APIC ID (used by AP bring-up). */
void lapic_send_init(uint8_t target_apic_id);

/* Send Startup IPI carrying a vector that encodes the trampoline
 * physical address: trampoline must be at (vector << 12).  Used twice
 * per AP per the Intel-recommended SIPI sequence. */
void lapic_send_sipi(uint8_t target_apic_id, uint8_t vector);

/* ---------------------------------------------------------------------------
 * LAPIC timer (M18.5).
 *
 * Each CPU has its own LAPIC timer; we use it as the per-CPU preempt
 * tick source so every core's scheduler can fire independently of the
 * (single, BSP-bound) PIT.
 *
 * The bus clock the timer counts from is unknown a-priori, so we
 * calibrate once on the BSP against the PIT to derive a "count for
 * target frequency" value that every CPU reuses (LAPICs in a single
 * package share the same bus clock).
 * --------------------------------------------------------------------------- */

/* Calibrate the LAPIC timer against the PIT.  Must run after the PIT
 * is already firing IRQ0 (i.e. after module_init_all + sti).  Returns
 * an opaque "count" value to feed into `lapic_timer_start_periodic`
 * for an interrupt rate of `target_hz`. */
uint32_t lapic_timer_calibrate(uint32_t target_hz);

/* Start the local LAPIC's timer in periodic mode at the calibrated
 * count, raising `vector` on every fire.  Each CPU calls this for
 * itself — periodic firing on this core only. */
void lapic_timer_start_periodic(uint32_t count, uint8_t vector);

/* Mask the local LAPIC timer (so it stops delivering its vector).
 * Used during shutdown or when stopping the per-CPU scheduler. */
void lapic_timer_stop(void);

#endif
