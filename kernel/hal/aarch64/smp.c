/* =============================================================================
 * smp.c — AArch64 SMP bring-up via PSCI (M21 Phase E).
 *
 * The ARM torture test of the "SMP-ready on UP" abstraction: bring up the
 * QEMU `virt` board's secondary cores and make them full scheduler
 * participants, reusing the stock per-CPU runqueue + load balancer + the
 * percpu.c table — the same core code the x86 SMP port drives, now on a third
 * architecture with a completely different mechanism.
 *
 * How a secondary starts, ARM-style (no INIT-SIPI-SIPI, no trampoline in low
 * memory): the firmware/hypervisor exposes **PSCI** (Power State Coordination
 * Interface).  We call `PSCI_CPU_ON` (an HVC to QEMU's emulated PSCI), passing
 * the target core's MPIDR + a physical entry point; QEMU releases that vCPU at
 * our entry with the MMU off, at EL1.  The entry trampoline (smp_entry.S) sets
 * a per-CPU stack and calls smp_secondary_main() here.
 *
 * Topology: on QEMU `virt` the vCPU MPIDR Aff0 field is the linear core index
 * (0..N-1) for the sizes we use, so `lapic_id()` (the percpu.c hook) returns
 * MPIDR.Aff0 and the ACPI-topology getters describe a linear N-CPU machine —
 * that lets percpu.c's stock apic_id→index mapping work unchanged.
 * ============================================================================= */

#include "printf.h"
#include "percpu.h"
#include "task.h"
#include "hal_api.h"
void hal_fpu_enable_this_cpu(void);   /* fpu.c (A2) */
#include "board.h"   /* §M85 — the machine, discovered */
#include <stdint.h>

/* Number of CPUs the kernel is built to manage.  MUST match the QEMU `-smp`
 * value the run script passes: the stock scheduler's task-placement picks any
 * CPU in [0, smp_ncpus()), so a slot with no online core behind it would strand
 * a task.  Keeping this == the vCPU count means every slot has a running core.
 * Raise both together (here + run_qemu.sh -smp) to scale up. */
#define AARCH64_MAX_CPUS 2

/* From mmu.c / gic.c / exceptions.c / timer.c. */
void gic_cpu_init(void);
void gic_send_sgi(int cpu, uint32_t sgi);
void gic_register_handler(uint32_t intid, void (*fn)(uint32_t));
void timer_init(uint32_t hz);

/* The reschedule SGI carries no payload — the actual reschedule happens in the
 * GIC IRQ-exit path (schedule_check runs after every EOI).  This handler just
 * exists so the dispatcher doesn't log it as "unhandled". */
static volatile uint32_t g_sgi_sent, g_sgi_recv;
static void resched_sgi_handler(uint32_t intid) {
    (void)intid;
    __atomic_add_fetch(&g_sgi_recv, 1, __ATOMIC_RELAXED);
}

/* Assembly trampoline (smp_entry.S) + the per-CPU stack-top table it reads. */
extern void smp_secondary_entry(void);
uint64_t ap_sp[AARCH64_MAX_CPUS];
static uint8_t ap_stacks[AARCH64_MAX_CPUS][16384] __attribute__((aligned(16)));

/* ---- percpu.c topology hooks (replaces the x86 LAPIC/ACPI ones) ------------- */

/* percpu.c calls lapic_id() to identify the running CPU; on ARM that is the
 * MPIDR Aff0 field. */
uint8_t lapic_id(void) {
    uint64_t mpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint8_t)(mpidr & 0xff);
}

int     acpi_ncpus(void)            { return AARCH64_MAX_CPUS; }
uint8_t acpi_cpu_apic_id(int i)     { return (uint8_t)i; }   /* linear Aff0    */
/* §M19.5.3 — the device tree's numa-node-id for this CPU (0 when it gives
 * none).  Slots are linear in Aff0 and in the tree's cpu@ order on the boards
 * this runs on (virt, sbsa-ref). */
int dtb_cpu_node(int i);
int     acpi_cpu_node(int slot)     { int n = dtb_cpu_node(slot); return n < 0 ? 0 : n; }

/* ---- cross-CPU reschedule (PSCI has no IPI; use a GIC SGI) ------------------ */

/* GICv2 distributor software-generated-interrupt register + our reschedule
 * SGI id.  Sending SGI `n` to CPU `c` sets that core's need_resched via the
 * SGI handler; used by the scheduler to kick a remote CPU. */
#define RESCHED_SGI 0

void smp_send_reschedule(int cpu_index) {
    if (cpu_index < 0 || cpu_index >= AARCH64_MAX_CPUS) return;
    /* §M85 — gic.c knows which GIC this is (v2: GICD_SGIR; v3: ICC_SGI1R). */
    __atomic_add_fetch(&g_sgi_sent, 1, __ATOMIC_RELAXED);
    gic_send_sgi(cpu_index, RESCHED_SGI);
}
/* `sgistat` (hidden) — reschedule SGIs sent against SGIs received: "the kick
 * never arrives" and "the kick arrives and is ignored" are different bugs. */
#include "shellcmd.h"
#include "printf.h"
static void cmd_sgistat(const char* a) {
    (void)a;
    kprintf("sgistat: reschedule SGIs sent %u, received %u\n", g_sgi_sent, g_sgi_recv);
}
SHELL_CMD(sgistat) = { "sgistat", "", 0, SHELL_G_TEST, cmd_sgistat, SHELL_P_ADMIN };

/* x86 leftover — the generic timer needs no shared calibration constant. */
void smp_set_lapic_timer_count(uint32_t c) { (void)c; }

/* ---- PSCI ------------------------------------------------------------------ */

#define PSCI_CPU_ON_AARCH64 0xC4000003u

static long psci_cpu_on(uint64_t target_mpidr, uint64_t entry, uint64_t ctx) {
    /* The conduit (HVC on `virt`, SMC under EL3 firmware) is the board's. */
    return psci_call(PSCI_CPU_ON_AARCH64, target_mpidr, entry, ctx);
}

/* ---- secondary CPU C entry ------------------------------------------------- */

static volatile int cpus_online = 1;    /* BSP counts as 1 */

/* Called (with the MMU still off) from smp_entry.S on each secondary core. */
void smp_secondary_main(uint64_t cpu) {
    /* The MMU is already on: smp_entry.S turned it on before jumping to the
     * image's virtual address (§M85), so every access from here is coherent. */

    /* Install the (shared) exception vector table for this core. */
    extern char vector_table[];
    __asm__ volatile ("msr vbar_el1, %0\nisb" :: "r"(vector_table) : "memory");

    /* This core's GIC CPU interface + banked timer PPI. */
    gic_cpu_init();
    /* A2 — CPACR_EL1.FPEN is per-CPU: enable FP/SIMD on THIS core, or a
     * musl binary's NEON memset traps here while working on the other. */
    hal_fpu_enable_this_cpu();


    /* Join the per-CPU table + become this CPU's idle task, then arm this
     * core's generic timer so its own tick drives preemption. */
    percpu_init_ap();
    task_install_ap_idle();
    timer_init(100);

    __sync_fetch_and_add(&cpus_online, 1);
    kprintf("aarch64: secondary CPU %u online\n", (unsigned)cpu);

    /* Enter the scheduler idle loop: run work the load balancer migrates here,
     * otherwise halt until the next interrupt. */
    hal_intr_enable();
    for (;;) {
        hal_cpu_halt();
        task_yield();
    }
}

/* ---- BSP-side bring-up ----------------------------------------------------- */

/* Start every secondary core (1..N-1) via PSCI.  Returns the number of CPUs
 * now online (including the BSP). */
int smp_boot_aps(void) {
    /* PSCI starts the core with its MMU off, so the entry is PHYSICAL (§M85:
     * the image runs at a virtual address unrelated to where it was loaded). */
    uint64_t entry = kptr_phys(&smp_secondary_entry);

    /* Handler for the cross-CPU reschedule SGI (registry is global, so one
     * registration covers every core). */
    gic_register_handler(RESCHED_SGI, resched_sgi_handler);

    for (int cpu = 1; cpu < AARCH64_MAX_CPUS; cpu++) {
        ap_sp[cpu] = (uint64_t)(uintptr_t)&ap_stacks[cpu][sizeof ap_stacks[cpu]];
        /* The secondary reads ap_sp[] and the image offset with its MMU (and
         * so its caches) OFF: clean both to the point of coherency, or it may
         * read memory the stores have not reached yet.  (QEMU's TCG has no
         * caches and would never show this; hardware does.) */
        __asm__ volatile ("dc cvac, %0" :: "r"(&ap_sp[cpu]) : "memory");
        __asm__ volatile ("dc cvac, %0" :: "r"(&aarch64_kimage_voffset) : "memory");
        __asm__ volatile ("dsb sy" ::: "memory");
        /* Target MPIDR = linear Aff0 = cpu index (QEMU `virt` topology). */
        long rc = psci_cpu_on((uint64_t)cpu, entry, 0);
        if (rc != 0) {
            kprintf("aarch64: PSCI CPU_ON(cpu=%d) failed rc=%ld "
                    "(fewer vCPUs than %d?)\n", cpu, rc, AARCH64_MAX_CPUS);
        }
    }

    /* Give the secondaries a moment to reach smp_secondary_main + count in. */
    for (volatile int spin = 0; spin < 20000000; spin++) { }

    kprintf("aarch64: SMP — %d CPU(s) online\n", cpus_online);
    return cpus_online;
}
