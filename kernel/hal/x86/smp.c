/* =============================================================================
 * smp.c — BSP-side SMP bring-up (M18).
 *
 * Walks the MADT CPU list, copies the AP trampoline to physical
 * 0x8000, fills the per-AP launch info at 0x9000, and runs the
 * INIT + SIPI + SIPI sequence the Intel SDM (Vol 3, §8.4) requires:
 *
 *   1. Send INIT IPI to target APIC ID.
 *   2. Wait ≥10 ms (we use 10).
 *   3. Send SIPI with vector V (target executes at physical V × 4 KiB).
 *   4. Wait ≥200 µs (we use 1 ms — coarser-grained timer).
 *   5. Send SIPI again (firmware-quirk recommendation).
 *
 * The AP, having reached the C entry, runs ap_main() (defined here)
 * which does its per-CPU init and marks itself online in the percpu
 * table.  The BSP polls that flag with a timeout to confirm bring-up.
 *
 * The AP then enters a halt loop — it has no scheduler hooks yet
 * (LAPIC timer / cross-CPU IPI for preemption are M18.5 work).  An
 * online AP is observable via `lscpu` but doesn't actually run
 * RUNNABLE tasks today; load-balancing the global runqueue across
 * cores is a follow-up.  See PLAN.md §M18 for the deferred items.
 * ============================================================================= */

#include "smp.h"
#include "acpi.h"
#include "lapic.h"
#include "gdt.h"
#include "idt.h"
#include "vmm.h"
#include "hal_api.h"
#include "percpu.h"
#include "task.h"
#include "timer.h"
#include "kmalloc.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

/* Trampoline blob — emitted by ld from objcopy on ap_trampoline.bin.
 * `objcopy --input-target=binary` produces three symbols named after
 * the input file: _start, _end, _size. */
extern uint8_t _binary_kernel_hal_x86_ap_trampoline_bin_start[];
extern uint8_t _binary_kernel_hal_x86_ap_trampoline_bin_end[];

/* Trampoline lands at this physical address; SIPI vector = base / 4 KiB. */
#define AP_BOOT_ADDR  0x8000
#define AP_INFO_ADDR  0x9000
#define AP_SIPI_VECTOR 0x08

/* The trampoline expects this layout at AP_INFO_ADDR. */
struct ap_info {
    uint32_t page_directory_phys;
    uint32_t stack_top;
    uint32_t c_entry;
    uint16_t gdtr_limit;
    uint32_t gdtr_base;
} __attribute__((packed));

/* Forward decl — the C entry the trampoline jumps to. */
static void ap_main(void);

/* ---------------------------------------------------------------------------
 * Tiny memcpy — the kernel doesn't link libc, so we roll our own.
 * --------------------------------------------------------------------------- */
static void smp_memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

/* ---------------------------------------------------------------------------
 * Trampoline copy — done once for the entire SMP bring-up; same
 * trampoline serves every AP since the per-AP info area is patched
 * separately for each launch.
 * --------------------------------------------------------------------------- */
static void copy_trampoline(void) {
    size_t len = (size_t)(_binary_kernel_hal_x86_ap_trampoline_bin_end
                        - _binary_kernel_hal_x86_ap_trampoline_bin_start);
    smp_memcpy((void*)AP_BOOT_ADDR,
               _binary_kernel_hal_x86_ap_trampoline_bin_start, len);
}

/* Calibrated count for the LAPIC timer at LAPIC_TIMER_HZ.  The BSP
 * fills this in once (via lapic_timer_calibrate) and every AP reads
 * the same value — LAPICs in one package share the bus clock. */
static uint32_t g_lapic_timer_count = 0;
#define LAPIC_TIMER_VECTOR 0x40

void smp_set_lapic_timer_count(uint32_t count) {
    g_lapic_timer_count = count;
}

/* ---------------------------------------------------------------------------
 * AP entry — first C function any AP runs.  Stack is already set up
 * (by the trampoline) and paging is on with the BSP's page directory.
 *
 * M18.5 bring-up sequence:
 *   1. Enable LAPIC + register self in percpu table.
 *   2. Load the shared IDT (IDTR is per-CPU; trampoline didn't lidt).
 *   3. Synthesize an idle task for the current context and join the
 *      global runqueue (`task_install_ap_idle`).
 *   4. Start LAPIC timer in periodic mode at the BSP-calibrated count.
 *   5. Enable interrupts and enter the idle loop.  On every LAPIC
 *      timer tick `schedule_check` runs and picks any non-idle
 *      runnable task off the global ring.
 * --------------------------------------------------------------------------- */
static void ap_main(void) {
    lapic_init_ap();
    percpu_init_ap();
    idt_load();
    /* SMP userland: point this CPU's TR at its own TSS so ring-3 → ring-0
     * traps (syscalls/IRQs from a user task scheduled here) land on a valid
     * per-CPU kernel stack.  After percpu_init_ap so this_cpu_id() is valid. */
    gdt_load_cpu_tss();

    /* Announce ourselves on serial — once per boot per AP. */
    kprintf("ap: cpu %d (apic_id=%u) online\n",
            this_cpu_id(), lapic_id());

    /* Become a real scheduler participant: synthesize an idle task
     * for the running context and start receiving LAPIC timer
     * preempt ticks. */
    task_install_ap_idle();
    if (g_lapic_timer_count) {
        lapic_timer_start_periodic(g_lapic_timer_count, LAPIC_TIMER_VECTOR);
    }

    /* Idle loop: enable IRQs, halt until the next one (LAPIC timer
     * tick or otherwise), then yield in case the scheduler wants
     * to hand us actual work.  Same shape as the BSP's pid-0 loop. */
    for (;;) {
        hal_intr_enable();
        hal_cpu_halt();
        task_yield();
    }
}

/* ---------------------------------------------------------------------------
 * smp_boot_aps — the public entry.
 * --------------------------------------------------------------------------- */
int smp_boot_aps(void) {
    int ncpus = smp_ncpus();
    if (ncpus <= 1) return 0;

    copy_trampoline();

    uint8_t bsp_id = lapic_id();
    int started = 0;

    for (int i = 0; i < ncpus; i++) {
        struct percpu* p = percpu_at(i);
        if (!p || p->apic_id == bsp_id) continue;

        /* Allocate a kernel stack for this AP. */
        void* stack = kmalloc(4096);
        if (!stack) {
            kprintf("smp: OOM allocating stack for AP %u\n", p->apic_id);
            continue;
        }

        /* Patch the per-AP info area.  Recompute the GDTR each iteration
         * because the AP reads it through bin-format absolute address. */
        struct ap_info* info = (struct ap_info*)AP_INFO_ADDR;
        info->page_directory_phys = vmm_kernel_pd_phys();
        info->stack_top           = (uint32_t)(uintptr_t)stack + 4096;
        info->c_entry             = (uint32_t)(uintptr_t)ap_main;
        uint8_t* gdtr_src = (uint8_t*)gdt_get_ptr_struct();
        info->gdtr_limit = *(uint16_t*)gdtr_src;
        info->gdtr_base  = *(uint32_t*)(gdtr_src + 2);

        /* INIT + SIPI + SIPI sequence. */
        lapic_send_init(p->apic_id);
        timer_msleep(10);
        lapic_send_sipi(p->apic_id, AP_SIPI_VECTOR);
        timer_msleep(1);
        lapic_send_sipi(p->apic_id, AP_SIPI_VECTOR);

        /* Wait up to 200 ms for the AP to come online. */
        uint64_t deadline = timer_ticks_ms() + 200;
        while (timer_ticks_ms() < deadline) {
            if (p->online) break;
            hal_cpu_pause();
        }
        if (p->online) {
            started++;
        } else {
            kprintf("smp: AP %u didn't come online within 200 ms\n",
                    p->apic_id);
        }
    }
    kprintf("smp: %d AP(s) started (of %d total CPU(s))\n",
            started, ncpus);
    return started;
}

/* ---------------------------------------------------------------------------
 * M18.6.4 — cross-CPU preempt IPI wrapper.
 *
 * Translates a logical CPU index into the target CPU's APIC ID and
 * fires LAPIC fixed-delivery IPI on vector 0x41.  Falls through for
 * self / invalid index — see smp.h for rationale.
 *
 * Vector 0x41 is reserved at IDT-init time in idt.c; its handler
 * (in isr_handler) calls schedule_check() on the receiving CPU.
 * --------------------------------------------------------------------------- */
#define PREEMPT_IPI_VECTOR  0x41

void smp_send_reschedule(int cpu_index) {
    if (cpu_index < 0 || cpu_index >= smp_ncpus()) return;
    struct percpu* p = percpu_at(cpu_index);
    if (!p || !p->online) return;
    if (cpu_index == this_cpu_id()) return;
    lapic_send_ipi(p->apic_id, PREEMPT_IPI_VECTOR);
}

