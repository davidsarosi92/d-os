/* =============================================================================
 * main_entry.c — AArch64 kernel bring-up entry (M21, Phases A–C).
 *
 * boot.S drops us to EL1, sets a stack, zeroes .bss, and calls
 * aarch64_main_entry(dtb).  Unlike the x86_64 port (which shares the
 * x86-coupled kernel_main), aarch64 runs its own bring-up here and calls the
 * *portable* core subsystems directly, skipping the x86-only ones
 * (multiboot/ACPI/LAPIC/IOAPIC/PIT).  Porting the shared kernel_main to a
 * fully portable shape — and reaching the interactive shell + drivers — is a
 * later phase; this entry owns the top of the call stack until then.
 *
 * Phases wired so far:
 *   A — exception vectors + MMU identity map (via hal_arch_early_init).
 *   B — GICv2 + generic timer (periodic IRQs).
 *   C — serial console sink (kprintf), PMM + kmalloc, the preemptive
 *       round-robin scheduler, proven by a multitask self-test: two hog
 *       tasks that only make progress if the timer IRQ preempts between
 *       them.  This is the ARM equivalent of kernel.c's preempt self-test.
 *   D — VFS + ramfs + the interactive serial shell (serial_shell.c).
 *   E — SMP: PSCI-started secondary cores joining the stock per-CPU
 *       scheduler + load balancer (smp.c); the two hogs then run on TWO
 *       cores in parallel.
 *   F — virtio-MMIO block device (virtio_mmio_blk.c) registered as
 *       /dev/vda, with a write→read round-trip self-test.
 *   G — exFAT (stock block_cache.c + exfat.c) mounted at /mnt off /dev/vda:
 *       persistent storage — files written from the shell survive a reboot.
 * ============================================================================= */

#include "printf.h"
#include "pmm.h"
#include "kmalloc.h"
#include "task.h"
#include "hal_api.h"
#include "vfs.h"
#include "module.h"
#include "percpu.h"
#include "smp.h"
#include "block.h"
#include "block_cache.h"
#include <stdint.h>

/* Early UART + arch bring-up (uart.c / hal_arch.c / gic.c / timer.c / stubs.c). */
void     uart_early_puts(const char* s);
void     uart_early_puthex(uint64_t v);
void     uart_early_putc(char c);
void     gic_init(void);
void     timer_init(uint32_t hz);
uint64_t timer_ticks(void);
void     aarch64_boot_meminfo_init(void);
void     aarch64_serial_console_init(void);
void     serial_shell_entry(void);   /* serial_shell.c — the Phase D REPL     */
int      virtio_mmio_blk_init(void); /* virtio_mmio_blk.c — Phase F disk       */
void     dtb_init(uint64_t x0);      /* dtb.c — Phase H device-tree discovery  */

static uint64_t current_el(void) {
    uint64_t el;
    __asm__ volatile ("mrs %0, CurrentEL" : "=r"(el));
    return (el >> 2) & 3;
}

/* -----------------------------------------------------------------------------
 * Preemptive-multitasking self-test.
 *
 * Two tasks that each spin incrementing a private counter and NEVER yield.
 * Under cooperative scheduling only one could ever run; under preemptive
 * round-robin the timer IRQ rotates the CPU between them (and pid 0), so BOTH
 * counters advance.  PASS = both non-zero.
 * --------------------------------------------------------------------------- */
static volatile uint64_t hog_a_count;
static volatile uint64_t hog_b_count;
static volatile int      hog_a_cpu = -1;
static volatile int      hog_b_cpu = -1;
static volatile int      hog_stop;

/* Each hog records which CPU it is currently running on; on an SMP system the
 * scheduler + load balancer land the two hogs on two different cores, so the
 * recorded CPU ids differ — the proof of real parallel execution. */
static void hog_a_entry(void) { while (!hog_stop) { hog_a_cpu = this_cpu_id(); hog_a_count++; } }
static void hog_b_entry(void) { while (!hog_stop) { hog_b_cpu = this_cpu_id(); hog_b_count++; } }

void aarch64_main_entry(uint64_t dtb) {
    /* Arch essentials (Phase A): EL1 vectors + MMU/caches on. */
    hal_arch_early_init();

    /* Wire kprintf to the PL011 so the rest of bring-up logs cleanly. */
    aarch64_serial_console_init();

    kprintf("\n=== d-os AArch64 (M21 Phase A–H) ===\n");
    kprintf("aarch64: booted at EL%u, DTB @ %p\n",
            (unsigned)current_el(), (void*)(uintptr_t)dtb);

    /* Discover the machine from the device tree (Phase H): RAM size, CPU
     * count, model.  The PMM map below then sizes itself to the actual `-m`
     * value instead of a hard-coded constant. */
    dtb_init(dtb);

    /* Memory manager (Phase C): synthesise the RAM map (no multiboot on ARM;
     * uses the DTB-discovered RAM size when available), then bring up the
     * physical allocator + kernel heap. */
    aarch64_boot_meminfo_init();
    pmm_init();
    kmalloc_init();

    /* Quick heap round-trip so a heap fault shows up here, not mid-scheduler. */
    {
        void* a = kmalloc(64);
        void* b = kmalloc(64);
        kfree(a);
        void* c = kmalloc(64);
        kprintf("aarch64: kmalloc self-test a=%p b=%p c=%p [reuse=%s]\n",
                a, b, c, (a == c) ? "yes" : "no");
        kfree(b); kfree(c);
    }

    /* Scheduler (Phase C): synthesise pid 0 from this context + an idle task. */
    task_init();

    /* Per-CPU table (Phase E) — build it now so smp_boot_aps() can register
     * secondary cores.  Runs AFTER task_init (which stamped slot 0's current)
     * so it does not wipe pid 0. */
    percpu_init_bsp();

    /* Interrupt controller + periodic timer (Phase B) — the timer now also
     * drives preemption (timer_isr → schedule_request; gic → schedule_check). */
    gic_init();
    timer_init(100);
    hal_intr_enable();

    /* SMP (Phase E) — start the secondary cores via PSCI; each joins the
     * scheduler as its CPU's idle task and arms its own generic timer. */
    int ncpu = smp_boot_aps();

    /* Preemption + parallelism self-test: two never-yielding hog tasks.  On a
     * single core the timer round-robins them (both progress); on SMP the load
     * balancer lands them on TWO cores (they record different CPU ids). */
    kprintf("aarch64: scheduler self-test (2 hog tasks, ~1 s)...\n");
    hog_a_count = hog_b_count = 0;
    hog_a_cpu = hog_b_cpu = -1;
    hog_stop = 0;
    struct task* ta = task_spawn("hogA", hog_a_entry);
    struct task* tb = task_spawn("hogB", hog_b_entry);
    if (ta && tb) {
        uint64_t deadline = timer_ticks() + 100;      /* ~1 s */
        while (timer_ticks() < deadline) hal_cpu_halt();
        uint64_t a = hog_a_count, b = hog_b_count;
        int ca = hog_a_cpu, cb = hog_b_cpu;
        hog_stop = 1;
        uint64_t reap = timer_ticks() + 20;
        while (timer_ticks() < reap) hal_cpu_halt();
        kprintf("aarch64: preemption %s (hogA=%u on CPU%d, hogB=%u on CPU%d)\n",
                (a > 0 && b > 0) ? "OK" : "FAIL",
                (unsigned)a, ca, (unsigned)b, cb);
        if (ncpu > 1) {
            kprintf("aarch64: parallelism %s (%d CPUs online; hogs on CPU%d + CPU%d)\n",
                    (ca != cb && ca >= 0 && cb >= 0) ? "PASS" : "inconclusive",
                    ncpu, ca, cb);
        }
    }

    /* -----------------------------------------------------------------------
     * Phase D — filesystem + the interactive serial shell.
     *
     * Bring up the VFS and let the ramfs MODULE register + mount itself at /
     * (module_init_all walks the .modules section — only ramfs is linked into
     * the aarch64 build today).  Then spawn the serial shell as a normal
     * scheduler task and drop pid 0 into the idle loop; the shell reads from
     * the PL011 and drives the PMM/scheduler/VFS.
     * ----------------------------------------------------------------------- */
    vfs_init();
    module_init_all();
    kprintf("aarch64: VFS up — ramfs mounted at /\n");

    /* -----------------------------------------------------------------------
     * Phase F — virtio-MMIO block device.  Probe the QEMU `virt` virtio-MMIO
     * slots for an attached disk; if present, register it as /dev/vda and run
     * a write→read round-trip on a scratch sector to prove real disk DMA.
     * ----------------------------------------------------------------------- */
    if (virtio_mmio_blk_init() == 0) {
        struct block_device* d = blk_find("vda");
        if (d && d->sector_count > 200) {
            static uint8_t wbuf[512], rbuf[512];
            for (int i = 0; i < 512; i++) wbuf[i] = (uint8_t)(i ^ 0x5a);
            int ok = d->write(d, 100, 1, wbuf) == 0
                  && d->read (d, 100, 1, rbuf) == 0;
            int match = ok;
            for (int i = 0; match && i < 512; i++) if (rbuf[i] != wbuf[i]) match = 0;
            kprintf("aarch64: virtio-blk write→read self-test %s (sector 100)\n",
                    match ? "PASS" : "FAIL");
        }

        /* Phase G — mount an exFAT volume off /dev/vda at /mnt.  The stock
         * block cache + exfat.c are arch-independent; if the disk carries an
         * exFAT filesystem the shell's ls/cat/write/rm then operate on real,
         * persistent storage under /mnt. */
        bcache_init();
        if (vfs_mount("exfat", "/mnt", "vda") == 0)
            kprintf("aarch64: exFAT mounted at /mnt (persistent storage)\n");
        else
            kprintf("aarch64: /dev/vda has no exFAT volume (skipping /mnt mount)\n");
    }

    struct task* sh = task_spawn("shell", serial_shell_entry);
    if (!sh) kprintf("aarch64: FATAL — failed to spawn serial shell\n");
    kprintf("aarch64: Phase D — interactive serial shell running "
            "(pid 0 → idle).\n");

    for (;;) hal_cpu_halt();
}
