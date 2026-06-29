/* =============================================================================
 * smp.c — BSP-side SMP bring-up for x86_64 (M20.5 Phase B).
 *
 * Mostly a port of `kernel/hal/x86/smp.c` with two structural changes:
 *
 *   1. The per-AP info struct uses 64-bit fields (PML4 pointer, stack
 *      top, C entry, kernel GDTR base).  The trampoline reads them at
 *      well-known offsets — keep this in sync with ap_trampoline.s.
 *
 *   2. The trampoline blob lives at kernel/hal/x86_64/ap_trampoline.bin
 *      and the `objcopy` symbol names include the full path, so the
 *      extern declarations point at the x86_64 blob (not the i386 one).
 *
 * Bring-up sequence (Intel SDM Vol 3 §8.4 — same as i386):
 *
 *   1. INIT IPI to target → AP enters wait-for-SIPI.
 *   2. Wait ≥10 ms.
 *   3. SIPI with vector V (target executes at physical V × 4 KiB).
 *   4. Wait ≥200 µs (we use 1 ms — coarser-grained timer).
 *   5. SIPI again (firmware-quirk recommendation).
 *
 * The AP, having reached `ap_main`, does per-CPU init (LAPIC enable,
 * percpu table registration, IDT load, idle task install, LAPIC timer
 * arm) and joins the global runqueue.
 *
 * Only one AP at a time goes through the AP_INFO_ADDR struct — the BSP
 * launches them sequentially and waits for the `online` flag before
 * moving on.
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

/* Trampoline blob — emitted by objcopy on ap_trampoline.bin.
 * `objcopy --input-target=binary` produces three symbols named after
 * the input file with '/' and '.' replaced by '_'. */
extern uint8_t _binary_kernel_hal_x86_64_ap_trampoline_bin_start[];
extern uint8_t _binary_kernel_hal_x86_64_ap_trampoline_bin_end[];

/* Trampoline lands at this physical address; SIPI vector = base / 4 KiB. */
#define AP_BOOT_ADDR    0x8000
#define AP_INFO_ADDR    0x9000
#define AP_SIPI_VECTOR  0x08

/* Per-AP info layout — the trampoline reads from these fixed offsets.
 * `packed` so the compiler doesn't pad; the trampoline picks up the
 * GDTR base at an 8-byte-misaligned offset, which is slow but
 * correct on x86. */
struct ap_info {
    uint64_t pml4_phys;             /* offset  0 — CR3 value           */
    uint64_t stack_top;             /* offset  8 — per-AP kernel stack */
    uint64_t c_entry;               /* offset 16 — &ap_main            */
    uint16_t kernel_gdtr_limit;     /* offset 24                       */
    uint64_t kernel_gdtr_base;      /* offset 26 — unaligned 8 bytes   */
} __attribute__((packed));

/* Forward decl — the C entry the trampoline jumps to. */
static void ap_main(void);

/* The trampoline-side GDTR mirror — same packed layout as the one in
 * gdt.c, redeclared locally so we don't have to expose the internals. */
struct gdtr_packed {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Calibrated count for the LAPIC timer.  The BSP fills this in once
 * (via lapic_timer_calibrate) and every AP reads the same value —
 * LAPICs in one package share the bus clock. */
static uint32_t g_lapic_timer_count = 0;
#define LAPIC_TIMER_VECTOR 0x40

void smp_set_lapic_timer_count(uint32_t count) {
    g_lapic_timer_count = count;
}

/* ---------------------------------------------------------------------------
 * Tiny memcpy — kernel has no libc.
 * --------------------------------------------------------------------------- */
static void smp_memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void copy_trampoline(void) {
    size_t len = (size_t)(_binary_kernel_hal_x86_64_ap_trampoline_bin_end
                        - _binary_kernel_hal_x86_64_ap_trampoline_bin_start);
    smp_memcpy((void*)AP_BOOT_ADDR,
               _binary_kernel_hal_x86_64_ap_trampoline_bin_start, len);
}

/* ---------------------------------------------------------------------------
 * AP entry — first C function any AP runs.  Stack already set up by
 * the trampoline, paging on with the BSP's PML4, CS=GDT_KERNEL_CS in
 * the kernel GDT.
 *
 * Same bring-up sequence as i386 (every step is portable):
 *   1. Enable LAPIC + register self in percpu table.
 *   2. lidt the shared IDT.
 *   3. Synthesize an idle task and join the global runqueue.
 *   4. Start LAPIC timer in periodic mode at the BSP-calibrated count.
 *   5. Enable interrupts, enter idle loop.
 * --------------------------------------------------------------------------- */
static void ap_main(void) {
    lapic_init_ap();
    percpu_init_ap();
    idt_load();

    kprintf("ap: cpu %d (apic_id=%u) online\n",
            this_cpu_id(), lapic_id());

    task_install_ap_idle();
    if (g_lapic_timer_count) {
        lapic_timer_start_periodic(g_lapic_timer_count, LAPIC_TIMER_VECTOR);
    }

    for (;;) {
        hal_intr_enable();
        hal_cpu_halt();
        task_yield();
    }
}

/* ---------------------------------------------------------------------------
 * smp_boot_aps — the public entry called from kernel_main.
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

        /* Per-AP kernel stack.  kmalloc returns 4 KiB-aligned for a
         * 4 KiB request (the slab path tops out at 2 KiB, so this
         * goes through page_alloc), and stack_top = base + 4096 is
         * 16-byte aligned — fits the System V ABI. */
        void* stack = kmalloc(4096);
        if (!stack) {
            kprintf("smp: OOM allocating stack for AP %u\n", p->apic_id);
            continue;
        }

        struct ap_info* info = (struct ap_info*)AP_INFO_ADDR;
        info->pml4_phys = (uint64_t)vmm_kernel_pd_phys();
        info->stack_top = (uint64_t)((uintptr_t)stack + 4096);
        info->c_entry   = (uint64_t)(uintptr_t)&ap_main;

        struct gdtr_packed* gdtr = (struct gdtr_packed*)gdt_get_ptr_struct();
        info->kernel_gdtr_limit = gdtr->limit;
        info->kernel_gdtr_base  = gdtr->base;

        /* INIT + SIPI + SIPI sequence — lapic_send_init/sipi are
         * shared between i386 and x86_64 (lapic.c compiles for both). */
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
 * M18.6.4 — cross-CPU preempt IPI wrapper.  Same as the i386 version
 * (lapic_send_ipi is in kernel/hal/x86/lapic.c, shared across archs).
 * --------------------------------------------------------------------------- */
#define PREEMPT_IPI_VECTOR  0x41

void smp_send_reschedule(int cpu_index) {
    if (cpu_index < 0 || cpu_index >= smp_ncpus()) return;
    struct percpu* p = percpu_at(cpu_index);
    if (!p || !p->online) return;
    if (cpu_index == this_cpu_id()) return;
    lapic_send_ipi(p->apic_id, PREEMPT_IPI_VECTOR);
}
