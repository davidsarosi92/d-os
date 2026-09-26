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

#include "board.h"   /* §M85 — the machine, discovered */
#include <stdint.h>

void uart_early_puts(const char* s);
void uart_early_puthex(uint64_t v);

/* Scheduler IRQ-exit hook (task.c, M21 Phase C).  Weak so the Phase A/B
 * builds link without the core scheduler; when present, it consults the
 * per-CPU need_resched flag + preempt_count and may context-switch. */
void schedule_check(void) __attribute__((weak));

/* ---- register banks (QEMU `virt`, GICv2) ------------------------------------ */
#define GICD_BASE   ((uintptr_t)g_board.gicd)      /* §M85: from the board */
#define GICC_BASE   ((uintptr_t)g_board.gicc)

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

/* ---- GICv3 (§M85 stage 2, 2026-09-26) ---------------------------------------
 *
 * What changes from v2, and why each piece is needed:
 *   - the CPU interface is SYSTEM REGISTERS (ICC_*_EL1), not an MMIO page —
 *     enabled by ICC_SRE_EL1.SRE; there is no GICC to map;
 *   - SGIs and PPIs live in a per-CPU REDISTRIBUTOR, found by walking the
 *     GICR frames for the one whose GICR_TYPER affinity is this CPU's MPIDR,
 *     and which must be WOKEN (GICR_WAKER) before it forwards anything;
 *   - SPIs are routed by AFFINITY (GICD_IROUTER), ITARGETSR is ignored once
 *     affinity routing (GICD_CTLR.ARE) is on;
 *   - interrupts must be GROUP 1 to reach EL1 (IGROUPR), and ICC_IGRPEN1_EL1
 *     enables the group at the CPU;
 *   - an SGI is sent by writing ICC_SGI1R_EL1, not GICD_SGIR.
 * QEMU `virt,gic-version=3` runs with a single security state (GICD_CTLR.DS),
 * which is the case written for; a machine with EL3 firmware has done the
 * secure half before we run. */
#define GICD_CTLR_RWP     (1u << 31)
#define GICD_IGROUPR      0x080
#define GICD_IROUTER      0x6000         /* 64-bit per SPI, from INTID 32      */
#define GICR_FRAME        0x20000        /* RD_base + SGI_base, 64 KiB each    */
#define GICR_WAKER        0x0014
#define GICR_TYPER        0x0008
#define GICR_SGI          0x10000
#define GICR_IGROUPR0     (GICR_SGI + 0x080)
#define GICR_ISENABLER0   (GICR_SGI + 0x100)
#define GICR_IPRIORITYR   (GICR_SGI + 0x400)

static int gic3(void) { return g_board.gic_version == 3; }

static inline uint64_t mmio_r64(uintptr_t a) { return *(volatile uint64_t*)a; }

/* This CPU's affinity, packed like GICR_TYPER[63:32]: Aff3.Aff2.Aff1.Aff0. */
static uint32_t my_aff(void) {
    uint64_t m;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(m));
    return (uint32_t)((m & 0xFFFFFF) | ((m >> 8) & 0xFF000000));
}

/* The redistributor frame of THIS CPU (cached per Aff0), or 0 if none matched
 * — which is a description that does not fit the machine, and says so. */
static uintptr_t rd_cache[64];
static uintptr_t gicr_this_cpu(void) {
    uint32_t aff = my_aff();
    if ((aff & 0xFF) < 64 && rd_cache[aff & 0xFF]) return rd_cache[aff & 0xFF];
    uintptr_t end = (uintptr_t)(g_board.gicr + (g_board.gicr_size ? g_board.gicr_size : 0x1000000));
    for (uintptr_t f = (uintptr_t)g_board.gicr; f < end; f += GICR_FRAME) {
        uint64_t typer = mmio_r64(f + GICR_TYPER);
        if ((uint32_t)(typer >> 32) == aff) {
            if ((aff & 0xFF) < 64) rd_cache[aff & 0xFF] = f;
            return f;
        }
        if (typer & (1u << 4)) break;               /* Last frame */
    }
    uart_early_puts("aarch64: GICv3 - no redistributor for this CPU\n");
    return 0;
}

static void gic3_rwp_wait(void) {
    for (int i = 0; i < 1000000 && (mmio_r32(GICD_BASE + GICD_CTLR) & GICD_CTLR_RWP); i++) { }
}

static void gic3_cpu_init(void) {
    uintptr_t rd = gicr_this_cpu();
    if (rd) {
        /* Wake the redistributor: clear ProcessorSleep, wait ChildrenAsleep. */
        mmio_w32(rd + GICR_WAKER, mmio_r32(rd + GICR_WAKER) & ~(1u << 1));
        for (int i = 0; i < 1000000 && (mmio_r32(rd + GICR_WAKER) & (1u << 2)); i++) { }
        mmio_w32(rd + GICR_IGROUPR0, 0xFFFFFFFFu);  /* SGIs + PPIs: group 1 */
        /* SGIs ENABLED, as on GICv2 — where they are permanently enabled and
         * the port never had to ask.  On v3 they are ordinary per-CPU enable
         * bits, and left clear the cross-CPU reschedule SGI is silently
         * dropped: a task woken for another CPU then waits for that CPU's next
         * timer tick instead.  Nothing fails; everything that wakes a remote
         * task gets ~10 ms slower — measured as `diskstorm` 740 ms on GICv2
         * against 22 512 ms on v3 before this line. */
        for (int i = 0; i < 16; i++) mmio_w8(rd + GICR_IPRIORITYR + i, 0x00);
        mmio_w32(rd + GICR_ISENABLER0, 0xFFFFu);
    }
    uint64_t sre;
    __asm__ volatile ("mrs %0, S3_0_C12_C12_5" : "=r"(sre));          /* ICC_SRE_EL1 */
    __asm__ volatile ("msr S3_0_C12_C12_5, %0\nisb" :: "r"(sre | 1));
    __asm__ volatile ("msr S3_0_C4_C6_0, %0" :: "r"((uint64_t)0xF0));  /* ICC_PMR_EL1 */
    __asm__ volatile ("msr S3_0_C12_C12_3, xzr");                     /* ICC_BPR1_EL1 */
    /* ICC_CTLR_EL1.EOImode = 0: one EOIR write both drops the priority and
     * deactivates.  Its reset value is IMPLEMENTATION DEFINED — with 1 the
     * dispatcher's EOI would leave every interrupt active for good. */
    __asm__ volatile ("msr S3_0_C12_C12_4, xzr");                     /* ICC_CTLR_EL1 */
    __asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" :: "r"((uint64_t)1)); /* IGRPEN1 */
}

/* Send SGI `sgi` to the CPU whose Aff0 is `cpu` (Aff1..3 = 0: this port's
 * linear topology, see smp.c). */
void gic_send_sgi(int cpu, uint32_t sgi) {
    if (gic3()) {
        uint64_t v = ((uint64_t)(sgi & 0xF) << 24) | (1u << (cpu & 15));
        __asm__ volatile ("msr S3_0_C12_C11_5, %0\nisb" :: "r"(v));   /* ICC_SGI1R_EL1 */
        return;
    }
    mmio_w32(GICD_BASE + 0xF00, ((1u << cpu) << 16) | (sgi & 0xF)); /* GICD_SGIR */
}

/* Unmask a single interrupt ID at the distributor: mid priority, and for SPIs
 * target CPU 0.  PPIs/SGIs (< 32) are banked per-CPU so no targeting. */
void gic_enable_irq(uint32_t intid) {
    if (gic3()) {
        if (intid < 32) {                               /* this CPU's SGI/PPI */
            uintptr_t rd = gicr_this_cpu();
            if (!rd) return;
            mmio_w8(rd + GICR_IPRIORITYR + intid, 0x00);
            mmio_w32(rd + GICR_ISENABLER0, 1u << intid);
        } else {
            mmio_w8(GICD_BASE + GICD_IPRIORITYR + intid, 0x00);
            mmio_w32(GICD_BASE + GICD_IGROUPR + (intid / 32) * 4,
                     mmio_r32(GICD_BASE + GICD_IGROUPR + (intid / 32) * 4) | (1u << (intid % 32)));
            *(volatile uint64_t*)(GICD_BASE + GICD_IROUTER + 8 * (intid - 32)) = 0; /* → Aff 0.0.0.0 */
            mmio_w32(GICD_BASE + GICD_ISENABLER + (intid / 32) * 4, 1u << (intid % 32));
        }
        return;
    }
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
    if (gic3()) { gic3_cpu_init(); return; }
    mmio_w32(GICC_BASE + GICC_PMR, 0xF0);
    mmio_w32(GICC_BASE + GICC_CTLR, 1);
}

/* Bring up the GICv2: the global distributor (once) + this CPU's interface.
 * After this, an enabled INTID that becomes pending is delivered as an IRQ
 * exception (once PSTATE.I is cleared). */
void gic_init(void) {
    if (gic3()) {
        mmio_w32(GICD_BASE + GICD_CTLR, 0);
        gic3_rwp_wait();
        gic3_cpu_init();
        /* ARE (bit 4) + EnableGrp1 (bit 1) + EnableGrp0 (bit 0). */
        mmio_w32(GICD_BASE + GICD_CTLR, (1u << 4) | (1u << 1) | 1u);
        gic3_rwp_wait();
        uart_early_puts("aarch64: GICv3 initialised (system-register CPU interface)\n");
        return;
    }
    /* Distributor off while we configure. */
    mmio_w32(GICD_BASE + GICD_CTLR, 0);

    /* This (boot) CPU's interface. */
    gic_cpu_init();

    /* Distributor on (global — done once, by the BSP). */
    mmio_w32(GICD_BASE + GICD_CTLR, 1);

    uart_early_puts("aarch64: GICv2 initialised (addresses: see `board`)\n");
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
    uint32_t iar;
    if (gic3()) {
        uint64_t v;
        __asm__ volatile ("mrs %0, S3_0_C12_C12_0" : "=r"(v));          /* ICC_IAR1_EL1 */
        iar = (uint32_t)v;
    } else {
        iar = mmio_r32(GICC_BASE + GICC_IAR);
    }
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
    if (gic3())
        __asm__ volatile ("msr S3_0_C12_C12_1, %0\nisb" :: "r"((uint64_t)iar)); /* ICC_EOIR1_EL1 */
    else
        mmio_w32(GICC_BASE + GICC_EOIR, iar);

    /* IRQ-exit preemption point: if a handler (the timer) requested a
     * reschedule, do it now — this may context-switch to another task and
     * only return here when we are eventually scheduled back. */
    if (schedule_check) schedule_check();
}
