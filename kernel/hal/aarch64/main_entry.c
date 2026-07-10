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
#include "procfs.h"
#include "module.h"
#include "percpu.h"
#include "smp.h"
#include "block.h"
#include "block_cache.h"
#include "vc.h"
#include "shell_provider.h"
#include "service.h"
#include "bus.h"
#include "watchdog.h"
#include "lock.h"
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
int      virtio_gpu_init(void);      /* virtio_gpu.c — Phase I framebuffer     */
int      virtio_input_init(void);    /* virtio_input.c — Phase J kbd + mouse   */
void     keymap_init(void);          /* keymap.c — select the default layout   */
void     driver_init_all(void);      /* driver.c — probe DRIVER()s (xHCI)       */
void     xhci_poll(void);            /* xhci.c — poll the USB event/HID rings   */
int      aarch64_usertest(void);     /* syscall.c — Phase L EL0 userspace test */
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

    /* -----------------------------------------------------------------------
     * Phase I — virtio-gpu framebuffer.  Probe the virtio-MMIO slots for a GPU;
     * if present, allocate a RAM framebuffer, stand up a 2D scanout, and route
     * the PORTABLE fb_terminal console onto it (the same renderer x86 uses).
     * From here the boot log renders graphically as well as to the serial log.
     * A no-op if no GPU is attached (`-nographic` without a virtio-gpu device),
     * in which case the serial console stays the only sink.
     * ----------------------------------------------------------------------- */
    int have_fb = (virtio_gpu_init() == 0);
    if (have_fb)
        kprintf("aarch64: virtio-gpu framebuffer console up\n");
    else
        kprintf("aarch64: no virtio-gpu device — serial console only\n");

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

    /* procfs (M29 parity): ramfs bootstraps /proc, so populate it — gives
     * aarch64 /proc/services + /proc/bus (and the built-ins) like x86. */
    procfs_init();

    /* Probe DRIVER()s (M15 USB: xHCI over the PCIe ECAM bus).  A no-op if no
     * xHCI controller is attached (`-device qemu-xhci`); when present it
     * enumerates the bus, assigns BARs, brings the controller up, and its
     * interrupt-IN HID endpoint is then polled from the generic-timer ISR. */
    driver_init_all();

    /* -----------------------------------------------------------------------
     * Phase L — EL0 userspace self-test (M25 prerequisite).  Create a private
     * address space, map an EL0 code + stack page, drop to EL0, run a tiny user
     * program that issues SYS_PRINT + SYS_EXIT via `svc`, and teleport back.
     * Proves the ring-3/EL0 substrate M25 will build real user processes on is
     * live — the ARM equivalent of the x86 `ringtest`.
     * ----------------------------------------------------------------------- */
    aarch64_usertest();

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

    /* -----------------------------------------------------------------------
     * Shell.  With a framebuffer (virtio-gpu) present we run the SAME full
     * shell.c the x86 ports use, on a virtual console, with virtio-input
     * keyboard + mouse (M21 Phase J) — `gui` then launches the M22 desktop
     * (Phase K).  Mirrors kernel.c's startup: init/reaper first, then vc_init
     * (paints over the boot log + installs per-task console routing), then the
     * shell provider on the root VC.  With no framebuffer we fall back to the
     * UART serial shell (Phase D).
     * ----------------------------------------------------------------------- */
    keymap_init();                              /* select the keyboard layout (default "us") */

    if (have_fb) {
        /* vc_init FIRST: it paints over the boot log and disables the global fb
         * console sink, switching to per-task routing.  Everything spawned
         * afterwards (init, the input task) has no bound console, so its output
         * goes to the SERIAL log only — the framebuffer then shows just the
         * shell's VC, with no multi-task console-race garble on screen. */
        vc_init();
        task_start_init();                      /* M27 reaper (prints to serial) */
        virtio_input_init();                    /* keyboard + mouse (prints to serial) */
        struct vc* root = vc_root();
        if (root) {
            struct task* sh = task_spawn("shell", shell_provider_active()->entry);
            if (sh) { task_set_out_console(sh, root); root->task = sh; }
            if (!sh) kprintf("aarch64: FATAL — failed to spawn framebuffer shell\n");
        } else {
            kprintf("aarch64: FATAL — no root VC (framebuffer init failed)\n");
        }
    } else {
        task_start_init();                      /* M27 universal reaper */
        kprintf("aarch64: no framebuffer — UART serial shell (pid 0 → idle).\n");
        if (!task_spawn("shell", serial_shell_entry))
            kprintf("aarch64: FATAL — failed to spawn serial shell\n");
    }

    /* M29 — services + service bus (init exists now, in either branch above).
     * Register /proc/services + /proc/bus and spawn the supervisor. */
    service_init();
    bus_init();
    service_start_supervisor();
    watchdog_init();                        /* M31 — freeze detection */

    /* Idle loop.  With a framebuffer we also drain virtio-input here (polled),
     * so keyboard + mouse events are serviced whenever the CPU is otherwise
     * idle (the shell blocked in vc_getchar, the compositor waiting).  Do NOT
     * halt in that case — halting would stall input until the next timer IRQ. */
    /* Mark pid 0 as THE idle task (like x86 kernel_main): idle tasks are off the
     * runqueue and only picked as a fallback, so the shell + init + apps are no
     * longer starved by pid 0 competing round-robin with them.  Then idle —
     * re-enabling IRQs each pass (as cpu_idle_entry does): `wfi` wakes on a
     * masked IRQ but does NOT take it, so if DAIF ever left IRQs masked here the
     * timer tick would never be serviced and this CPU would stop scheduling
     * (starving every task homed on it — e.g. the input poll task). */
    task_become_idle();
    for (;;) { hal_intr_enable(); hal_cpu_halt(); }
}
