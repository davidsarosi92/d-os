/* =============================================================================
 * gic.c — ARM Generic Interrupt Controller v2 driver for the AArch64 port
 * (M21 Phase B).
 *
 * The GIC is ARM's answer to the x86 APIC: it is the thing that routes device
 * interrupts to the CPU.  A GICv2 has two register banks:
 *
 *   - **Distributor (GICD)** — global: enables/disables individual interrupt
 *     IDs, sets their priority + CPU targets, and edge/level config.  On the
 *     QEMU `virt` board it sits at MMIO 0x0800_0000.
 *   - **CPU interface (GICC)** — per-CPU: the priority mask, and the
 *     acknowledge (IAR) / end-of-interrupt (EOIR) handshake registers.  On
 *     `virt` it is at 0x0801_0000.
 *
 * Interrupt ID ranges (GICv2):
 *   0..15    SGIs  (software-generated / IPIs)   — banked per-CPU
 *   16..31   PPIs  (private peripheral, e.g. the per-CPU timer) — banked
 *   32..1019 SPIs  (shared peripheral, e.g. the UART, virtio) — global
 *   1020..1023 special (1023 = spurious)
 *
 * This provides the ARM half of the "IRQ install API": gic_enable_irq() +
 * gic_register_handler() replace the x86 irq_install()/IOAPIC routing.  The
 * strong `aarch64_irq_dispatch` here overrides the weak stub in exceptions.c,
 * so an IRQ taken through the EL1 vector table lands in the ack→dispatch→EOI
 * handshake below.
 *
 * We run everything in GIC Group 0 (the simplest single-security-state setup
 * QEMU accepts), targeting CPU 0 for SPIs.  SMP (SGIs/IPIs + per-CPU init) is
 * a later phase.
 * ============================================================================= */

#include <stdint.h>

void uart_early_puts(const char* s);
void uart_early_puthex(uint64_t v);

/* Scheduler IRQ-exit hook (task.c, M21 Phase C).  Weak so the Phase A/B
 * builds link without the core scheduler; when present, it consults the
 * per-CPU need_resched flag + preempt_count and may context-switch. */
void schedule_check(void) __attribute__((weak));

/* ---- register banks (QEMU `virt`, GICv2) ------------------------------------ */
#define GICD_BASE   0x08000000UL
#define GICC_BASE   0x08010000UL

/* Distributor registers (byte offsets from GICD_BASE). */
#define GICD_CTLR         0x000   /* Distributor control (enable).            */
#define GICD_TYPER        0x004   /* Controller type (line count).            */
#define GICD_ISENABLER    0x100   /* Set-enable, 1 bit / INTID.               */
#define GICD_ICENABLER    0x180   /* Clear-enable, 1 bit / INTID.             */
#define GICD_IPRIORITYR   0x400   /* Priority, 1 byte / INTID.                */
#define GICD_ITARGETSR    0x800   /* CPU targets, 1 byte / INTID (SPI only).  */
#define GICD_ICFGR        0xC00   /* Edge/level config, 2 bits / INTID.       */

/* CPU-interface registers (byte offsets from GICC_BASE). */
#define GICC_CTLR         0x000   /* CPU interface control (enable).          */
#define GICC_PMR          0x004   /* Priority mask.                           */
#define GICC_IAR          0x00C   /* Interrupt acknowledge (read → INTID).    */
#define GICC_EOIR         0x010   /* End of interrupt (write INTID).          */

#define GIC_SPURIOUS      1023    /* IAR returns this when nothing pending.   */

static inline void     mmio_w32(uintptr_t a, uint32_t v) { *(volatile uint32_t*)a = v; }
static inline uint32_t mmio_r32(uintptr_t a)             { return *(volatile uint32_t*)a; }
static inline void     mmio_w8 (uintptr_t a, uint8_t v)  { *(volatile uint8_t*)a  = v; }

/* Per-INTID handler table.  256 is comfortably past the PPIs + the low SPIs
 * we will ever wire on `virt` (timer PPI 30, UART SPI 33, virtio 48+). */
#define GIC_MAX_INTID 256
typedef void (*irq_handler_t)(uint32_t intid);
static irq_handler_t handlers[GIC_MAX_INTID];

/* Register a C handler for an interrupt ID.  Does NOT enable it — call
 * gic_enable_irq() for that (mirrors the x86 install-then-unmask split). */
void gic_register_handler(uint32_t intid, irq_handler_t fn) {
    if (intid < GIC_MAX_INTID) handlers[intid] = fn;
}

/* Unmask a single interrupt ID at the distributor: mid priority, and for SPIs
 * target CPU 0.  PPIs/SGIs (< 32) are banked per-CPU so no targeting. */
void gic_enable_irq(uint32_t intid) {
    mmio_w8(GICD_BASE + GICD_IPRIORITYR + intid, 0x00);   /* highest priority */
    if (intid >= 32) {
        mmio_w8(GICD_BASE + GICD_ITARGETSR + intid, 0x01); /* → CPU 0        */
    }
    mmio_w32(GICD_BASE + GICD_ISENABLER + (intid / 32) * 4, 1u << (intid % 32));
}

/* Per-CPU CPU-interface bring-up.  GICC (and the banked SGI/PPI registers) are
 * private to each core, so EVERY CPU — the BSP and each PSCI-started secondary
 * — must run this to start receiving interrupts.  PMR = 0xF0 lets every
 * priority through; GICC_CTLR bit 0 enables signalling to this core. */
void gic_cpu_init(void) {
    mmio_w32(GICC_BASE + GICC_PMR, 0xF0);
    mmio_w32(GICC_BASE + GICC_CTLR, 1);
}

/* Bring up the GICv2: the global distributor (once) + this CPU's interface.
 * After this, an enabled INTID that becomes pending is delivered as an IRQ
 * exception (once PSTATE.I is cleared). */
void gic_init(void) {
    /* Distributor off while we configure. */
    mmio_w32(GICD_BASE + GICD_CTLR, 0);

    /* This (boot) CPU's interface. */
    gic_cpu_init();

    /* Distributor on (global — done once, by the BSP). */
    mmio_w32(GICD_BASE + GICD_CTLR, 1);

    uart_early_puts("aarch64: GICv2 initialised (GICD @0x08000000, GICC @0x08010000)\n");
}

/* -----------------------------------------------------------------------------
 * IRQ entry point — strong override of the weak stub in exceptions.c.  Called
 * from the EL1 IRQ vector with a trapframe already on the stack.
 *
 * The GICv2 handshake: read GICC_IAR to acknowledge (this both tells us the
 * INTID and raises the running priority), dispatch, then write the SAME value
 * back to GICC_EOIR to drop the priority and allow the next one.
 * ----------------------------------------------------------------------------- */
void aarch64_irq_dispatch(void) {
    uint32_t iar   = mmio_r32(GICC_BASE + GICC_IAR);
    uint32_t intid = iar & 0x3FF;

    if (intid >= 1020) {
        /* Spurious / special — no EOI needed (and none defined). */
        return;
    }

    if (intid < GIC_MAX_INTID && handlers[intid]) {
        handlers[intid](intid);
    } else {
        uart_early_puts("aarch64: unhandled IRQ intid=");
        uart_early_puthex(intid);
        uart_early_puts("\n");
    }

    /* End of interrupt — must write back the full IAR value BEFORE any
     * reschedule, so the timer keeps firing on whoever runs next. */
    mmio_w32(GICC_BASE + GICC_EOIR, iar);

    /* IRQ-exit preemption point: if a handler (the timer) requested a
     * reschedule, do it now — this may context-switch to another task and
     * only return here when we are eventually scheduled back. */
    if (schedule_check) schedule_check();
}
