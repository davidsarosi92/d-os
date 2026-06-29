/* =============================================================================
 * m20_stubs.c — UP-only stubs for symbols deferred to later milestones
 *               on x86_64.
 *
 * d-os's i386 port has these subsystems fully wired:
 *   - LAPIC + IOAPIC (M18 / M18.5)        — per-CPU APIC + IOAPIC routing
 *   - SMP bring-up (M18)                  — AP trampoline, INIT+SIPI
 *   - Ring-3 + int 0x80 syscalls (M6)     — TSS, usermode wrap
 *   - xHCI USB host (M15)                 — USB HID keyboards
 *
 * For the M20 "x86_64 UP boot to shell" DoD we need NONE of those at
 * runtime, but kernel core/drivers reference their symbols
 * unconditionally (irq dispatch checks `f->int_no == 0x80`, pit_irq
 * polls xhci, etc.).  Linking would fail without something providing
 * those names.
 *
 * Strategy: provide minimum-viable stubs here, behind UP/no-APIC
 * semantics.  As later milestones bring real x86_64 impls (M20.5 for
 * SMP/APIC, Phase 7 for SYSCALL/SYSRET, separate milestone for USB),
 * delete the corresponding stubs from this file.  When this file
 * shrinks to empty, delete it.
 *
 * Every stub here returns the "feature unavailable" answer it would
 * give on a hypothetical APIC-less single-core system: lapic_id() = 0
 * (single CPU), smp_ncpus() = 1, no IRQs routed via APIC, no SYSCALL,
 * no USB.  kernel.c's APIC-init block is gated under `#if
 * defined(__i386__)` so the LAPIC/IOAPIC stubs are never CALLED on
 * x86_64 — they're just there for the linker.  Same for ring-3 and
 * USB callers.
 * ============================================================================= */

#include <stdint.h>

struct int_frame;

/* ---------------------------------------------------------------------------
 * LAPIC stubs (M18 / M18.5).  Real impl ships in M20.5 (x86_64 SMP).
 * --------------------------------------------------------------------------- */

int      lapic_init_bsp(uint32_t phys)                              { (void)phys; return -1; }
void     lapic_init_ap(void)                                        { /* no-op */ }
void     lapic_eoi(void)                                            { /* no-op */ }
uint8_t  lapic_id(void)                                             { return 0; }
void     lapic_send_init(uint8_t target_apic_id)                    { (void)target_apic_id; }
void     lapic_send_sipi(uint8_t target_apic_id, uint8_t vector)    { (void)target_apic_id; (void)vector; }
uint32_t lapic_timer_calibrate(uint32_t target_hz)                  { (void)target_hz; return 0; }
void     lapic_timer_start_periodic(uint32_t count, uint8_t vector) { (void)count; (void)vector; }
void     lapic_timer_stop(void)                                     { /* no-op */ }

/* ---------------------------------------------------------------------------
 * IOAPIC stubs.
 * --------------------------------------------------------------------------- */

int  ioapic_init(uint32_t phys, uint32_t gsi_base)            { (void)phys; (void)gsi_base; return -1; }
void ioapic_route_isa(int irq, uint8_t vector, uint8_t apic_id) { (void)irq; (void)vector; (void)apic_id; }

/* ---------------------------------------------------------------------------
 * SMP stubs.  smp_ncpus is provided by kernel/core/percpu.c (which
 * we include in the build) — it returns 1 on a UP boot regardless of
 * arch, so we don't stub it here.
 * --------------------------------------------------------------------------- */

int  smp_boot_aps(void)                          { return 0; }
void smp_set_lapic_timer_count(uint32_t count)   { (void)count; }

/* ---------------------------------------------------------------------------
 * Ring-3 / syscall stubs.  Real impl in Phase 7 (SYSCALL/SYSRET).
 *
 * If anything calls these on x86_64 today, hard-stop so the misuse
 * is visible on the serial log instead of silently misbehaving.
 * --------------------------------------------------------------------------- */

void syscall_dispatch(struct int_frame* f) {
    (void)f;
    for (;;) __asm__ volatile ("cli; hlt");
}

void enter_user_mode_wrap(uint64_t eip, uint64_t esp) {
    (void)eip; (void)esp;
    for (;;) __asm__ volatile ("cli; hlt");
}

/* ---------------------------------------------------------------------------
 * USB host (M15) — xhci_poll is called from pit_irq.  We have no
 * xHCI driver on x86_64 yet (the i386 driver assumes MMIO + DMA in
 * the low 4 GiB; needs a 64-bit revisit).  No-op until then.
 * --------------------------------------------------------------------------- */

void xhci_poll(void) { /* no USB on x86_64 yet */ }
