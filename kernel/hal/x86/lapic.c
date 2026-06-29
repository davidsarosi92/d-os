/* =============================================================================
 * lapic.c — Local APIC driver (M18).
 *
 * Hardware: Intel xAPIC MMIO interface at the address ACPI reports
 * (typically 0xFEE00000).  Each CPU has its own LAPIC accessed via the
 * same MMIO window — the chipset directs the access to the local unit.
 *
 * What we use the LAPIC for at this stage:
 *   - End-of-Interrupt acknowledgement for IOAPIC-routed IRQs.
 *   - Inter-Processor Interrupts: the INIT + SIPI sequence that wakes
 *     an Application Processor out of reset state.
 *
 * Bring-up sequence on the BSP:
 *   1. Map MMIO base with cache-disable + write-through (Intel SDM Vol
 *      3 §10.4.1: the LAPIC registers must be mapped strongly-ordered).
 *   2. Set IA32_APIC_BASE MSR bit 11 (APIC global enable) just in case
 *      firmware left it off.
 *   3. Write SIVR (Spurious Interrupt Vector Register, offset 0xF0)
 *      with bit 8 set (APIC software enable) + a spurious vector
 *      (0xFF — caught by an IDT stub, no-op handler).
 *   4. Mask LINT0/LINT1/LVT-timer/LVT-error so no stray interrupts
 *      arrive while we wire IOAPIC.
 *
 * Reference: Intel SDM Vol 3, Chapter 10 ("Advanced Programmable
 * Interrupt Controller (APIC)").
 * ============================================================================= */

#include "lapic.h"
#include "vmm.h"
#include "hal.h"
#include "printf.h"
#include "timer.h"
#include <stdint.h>

/* LAPIC MMIO register offsets. */
#define LAPIC_REG_ID        0x020
#define LAPIC_REG_VERSION   0x030
#define LAPIC_REG_TPR       0x080
#define LAPIC_REG_EOI       0x0B0
#define LAPIC_REG_LDR       0x0D0
#define LAPIC_REG_DFR       0x0E0
#define LAPIC_REG_SIVR      0x0F0
#define LAPIC_REG_ICR_LO    0x300
#define LAPIC_REG_ICR_HI    0x310
#define LAPIC_REG_LVT_TIMER 0x320
#define LAPIC_REG_LVT_LINT0 0x350
#define LAPIC_REG_LVT_LINT1 0x360
#define LAPIC_REG_LVT_ERROR 0x370
#define LAPIC_REG_INITCNT   0x380
#define LAPIC_REG_CURRCNT   0x390
#define LAPIC_REG_DIVCONF   0x3E0

/* LVT timer mode bits (bits 17..18 of LVT_TIMER):
 *   00 = one-shot, 01 = periodic, 10 = TSC-deadline. */
#define LAPIC_LVT_PERIODIC  (1u << 17)

/* DIVIDE_CONFIG encoding (Intel SDM Vol 3 §10.5.4 Table 10-1):
 *   value  divisor
 *   0xB    1
 *   0x0    2
 *   0x1    4
 *   0x2    8
 *   0x3    16
 *   0x8    32
 *   0x9    64
 *   0xA    128
 * We pick /16 — enough resolution at GHz-class bus clocks, fits in
 * 32-bit count for milliseconds at QEMU's ~250 MHz APIC. */
#define LAPIC_DIVIDE_16     0x3u

/* SIVR bits. */
#define LAPIC_SIVR_ENABLE   (1u << 8)
#define LAPIC_SIVR_SPURIOUS 0xFF

/* LVT bits. */
#define LAPIC_LVT_MASKED    (1u << 16)

/* ICR delivery modes (bits 8..10). */
#define LAPIC_DM_FIXED      (0u << 8)
#define LAPIC_DM_INIT       (5u << 8)
#define LAPIC_DM_STARTUP    (6u << 8)

/* ICR level (bit 14): 1 = assert.  Used for INIT-deassert legacy logic
 * but modern firmware ignores it; we set 1 for safety. */
#define LAPIC_ICR_ASSERT    (1u << 14)

static volatile uint8_t* g_lapic_mmio = 0;

/* Raw MMIO accessors — `volatile uint32_t*` to keep the compiler from
 * folding consecutive reads/writes. */
static inline uint32_t lapic_r(uint32_t off) {
    return *(volatile uint32_t*)(g_lapic_mmio + off);
}
static inline void lapic_w(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(g_lapic_mmio + off) = v;
}

/* IA32_APIC_BASE MSR (0x1B): bit 11 = APIC global enable. */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    uint32_t lo = (uint32_t)v;
    uint32_t hi = (uint32_t)(v >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

int lapic_init_bsp(uintptr_t phys) {
    /* Map a single 4 KiB page covering the LAPIC's MMIO window with
     * cache-disabled + write-through so reads always go to the device
     * and never come back from a stale cache line.
     *
     * `phys` is uintptr_t so an x86_64 board reporting an MMIO base
     * above 4 GiB compiles without truncation; vmm_map's i386 impl
     * still rejects > 4 GiB (CR3 layout), which is the right failure
     * for that case. */
    uintptr_t aligned = phys & ~((uintptr_t)0xFFFu);
    if (vmm_map(aligned, aligned, VMM_WRITABLE | VMM_CACHE_DIS) != 0) {
        /* Possibly already mapped by the kernel's identity map — that's
         * fine; the cache-disable bit will lose, but on QEMU
         * 0xFEE00000 is above the identity range so vmm_map should
         * win.  On x86_64 the 1 GiB boot identity already covers it. */
    }
    g_lapic_mmio = (volatile uint8_t*)phys;

    /* Force IA32_APIC_BASE.APIC_EN = 1 — firmware usually leaves it on
     * but spec says we should re-set it after a reset. */
    uint64_t base = rdmsr(0x1B);
    wrmsr(0x1B, base | (1ULL << 11));

    /* Software-enable + spurious vector. */
    lapic_w(LAPIC_REG_SIVR, LAPIC_SIVR_ENABLE | LAPIC_SIVR_SPURIOUS);

    /* Mask everything we don't use yet — stray IRQ delivery is the
     * #1 cause of "system works for 200 ms and dies" bring-up bugs. */
    lapic_w(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_w(LAPIC_REG_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_w(LAPIC_REG_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_w(LAPIC_REG_LVT_ERROR, LAPIC_LVT_MASKED);

    /* Task priority 0 = accept any vector. */
    lapic_w(LAPIC_REG_TPR, 0);

    kprintf("lapic: BSP enabled at %p (id=%u)\n",
            (void*)phys, lapic_id());
    return 0;
}

void lapic_init_ap(void) {
    /* APs share the MMIO window the BSP mapped — just enable the
     * local unit and mask its LVT lines. */
    uint64_t base = rdmsr(0x1B);
    wrmsr(0x1B, base | (1ULL << 11));
    lapic_w(LAPIC_REG_SIVR, LAPIC_SIVR_ENABLE | LAPIC_SIVR_SPURIOUS);
    lapic_w(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_w(LAPIC_REG_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_w(LAPIC_REG_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_w(LAPIC_REG_LVT_ERROR, LAPIC_LVT_MASKED);
    lapic_w(LAPIC_REG_TPR, 0);
}

void lapic_eoi(void) {
    /* Write anything (Intel-recommended: 0) to acknowledge the
     * highest-priority in-service interrupt. */
    lapic_w(LAPIC_REG_EOI, 0);
}

uint8_t lapic_id(void) {
    /* APIC ID is in bits 24..31 of the ID register. */
    return (uint8_t)(lapic_r(LAPIC_REG_ID) >> 24);
}

/* ---------------------------------------------------------------------------
 * IPI helpers — used by the SMP bring-up code in smp.c.
 *
 * The Intel-mandated AP bring-up sequence (SDM Vol 3 §8.4):
 *   1. BSP sends INIT IPI to target → AP enters wait-for-SIPI.
 *   2. BSP waits ≥10 ms.
 *   3. BSP sends SIPI IPI with vector V (so AP starts at physical
 *      address V × 4 KiB in real mode).
 *   4. BSP waits ≥200 µs.
 *   5. BSP sends SIPI IPI again (firmware-quirk recommendation).
 *
 * Writing the high half of ICR (LAPIC_REG_ICR_HI) latches the
 * destination APIC ID in bits 24..31.  Writing the low half (ICR_LO)
 * actually fires the IPI — so we always set HI first.
 * --------------------------------------------------------------------------- */

static void ipi_wait_idle(void) {
    /* ICR.Delivery Status (bit 12) clears when the IPI is accepted. */
    while (lapic_r(LAPIC_REG_ICR_LO) & (1u << 12)) {
        __asm__ volatile ("pause");
    }
}

void lapic_send_init(uint8_t target_apic_id) {
    lapic_w(LAPIC_REG_ICR_HI, ((uint32_t)target_apic_id) << 24);
    lapic_w(LAPIC_REG_ICR_LO, LAPIC_DM_INIT | LAPIC_ICR_ASSERT);
    ipi_wait_idle();
}

void lapic_send_sipi(uint8_t target_apic_id, uint8_t vector) {
    lapic_w(LAPIC_REG_ICR_HI, ((uint32_t)target_apic_id) << 24);
    lapic_w(LAPIC_REG_ICR_LO, LAPIC_DM_STARTUP | LAPIC_ICR_ASSERT | vector);
    ipi_wait_idle();
}

/* M18.6.4 — generic fixed-delivery IPI sender.  Used for cross-CPU
 * preempt requests (vector 0x41) and any future "kick this CPU" path.
 *
 * Fixed delivery: target CPU's LAPIC vectors `vector` into the IDT
 * exactly like any other IRQ.  Caller is expected to have an IDT
 * gate installed for `vector` already (idt.c sets up 0x41 at boot).
 *
 * Avoid sending to self — Intel guarantees self-IPIs work but they
 * defeat the purpose here (we wanted a different CPU to reschedule).
 * Callers that want self-reschedule should `schedule_request()` and
 * let the next local tick honor it. */
void lapic_send_ipi(uint8_t target_apic_id, uint8_t vector) {
    /* Self-IPI is a no-op for our use case; cheap guard to avoid the
     * round-trip through LAPIC for a wakeup we don't need. */
    if (g_lapic_mmio && target_apic_id == lapic_id()) return;
    lapic_w(LAPIC_REG_ICR_HI, ((uint32_t)target_apic_id) << 24);
    lapic_w(LAPIC_REG_ICR_LO, LAPIC_DM_FIXED | LAPIC_ICR_ASSERT | (uint32_t)vector);
    ipi_wait_idle();
}

/* ---------------------------------------------------------------------------
 * LAPIC timer — per-CPU preempt source (M18.5).
 *
 * Calibration: program one-shot mode with the maximum count, sleep
 * N PIT-ms, sample the elapsed count, scale to the target frequency.
 * All LAPICs in a CPU package share the same bus clock so the
 * calibrated value is reusable across CPUs.
 * --------------------------------------------------------------------------- */

uint32_t lapic_timer_calibrate(uint32_t target_hz) {
    if (target_hz == 0) target_hz = 100;

    /* Configure divider first — Intel says divide config must be
     * stable before LVT timer programming. */
    lapic_w(LAPIC_REG_DIVCONF, LAPIC_DIVIDE_16);

    /* Mask the timer so the one-shot countdown doesn't fire an IRQ. */
    lapic_w(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);

    /* Start one-shot countdown from the max 32-bit value. */
    lapic_w(LAPIC_REG_INITCNT, 0xFFFFFFFFu);

    /* Calibration window — 50 PIT-ms.  Short enough to keep boot
     * snappy, long enough to wash out single-tick jitter. */
    const uint32_t window_ms = 50;
    uint64_t start = timer_ticks_ms();
    while (timer_ticks_ms() - start < window_ms) {
        __asm__ volatile ("pause");
    }

    /* Sample remaining count + freeze further countdown. */
    uint32_t remaining = lapic_r(LAPIC_REG_CURRCNT);
    lapic_w(LAPIC_REG_INITCNT, 0);

    uint32_t elapsed = 0xFFFFFFFFu - remaining;
    uint32_t ticks_per_ms = elapsed / window_ms;
    if (ticks_per_ms == 0) ticks_per_ms = 1;

    /* Count for one period of `target_hz`: ticks per (1000/target_hz) ms. */
    uint32_t count = ticks_per_ms * (1000u / target_hz);
    if (count == 0) count = 1;

    kprintf("lapic: timer calibrated — %u ticks/ms, count=%u for %u Hz\n",
            ticks_per_ms, count, target_hz);
    return count;
}

void lapic_timer_start_periodic(uint32_t count, uint8_t vector) {
    /* Divider must be set on every CPU that runs the timer — it's
     * a per-LAPIC register. */
    lapic_w(LAPIC_REG_DIVCONF, LAPIC_DIVIDE_16);

    /* LVT_TIMER: periodic mode + vector.  No mask. */
    lapic_w(LAPIC_REG_LVT_TIMER, LAPIC_LVT_PERIODIC | (uint32_t)vector);

    /* Writing INITCNT (re)arms the timer. */
    lapic_w(LAPIC_REG_INITCNT, count);
}

void lapic_timer_stop(void) {
    lapic_w(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_w(LAPIC_REG_INITCNT, 0);
}
