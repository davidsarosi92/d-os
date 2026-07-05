/* =============================================================================
 * kernel.c — the C entry point for d-os.
 *
 * Control lands here from `_start` (kernel/hal/x86/boot.s) with a stack
 * already set up.  `_start` passes us the two values GRUB handed it on
 * multiboot1 entry:
 *
 *   magic   — should equal 0x2BADB002 if the loader actually is multiboot1.
 *   info    — physical address of the multiboot_info structure.
 *
 * Boot sequence and rationale:
 *
 *   1. serial_module_init  — earliest possible: gives us a debug log on
 *                            COM1.  Idempotent (the module framework
 *                            re-runs it later as a no-op).
 *   2. gdt_init / idt_init — arch essentials.  These can't be modules
 *                            because subsequent C code depends on
 *                            interrupts being safely deliverable.
 *   3. mboot_init          — cache the loader's info pointer.
 *   4. pmm_init / vmm_init / kmalloc_init — memory management stack.
 *   5. module_init_all     — fires every registered MODULE() including
 *                            the keyboard, the framebuffer terminal,
 *                            the VGA fallback terminal.  After this the
 *                            console sink registry is alive and kprintf
 *                            output reaches the user.
 *   6. banner + diagnostics — first lines the user sees.
 *   7. acpi_init           — soft-off support.
 *   8. self-tests          — vmm + kmalloc round-trips.
 *   9. sti                 — let IRQs through; keyboard buffer fills.
 *  10. shell_run           — never returns.
 *
 * Adding new drivers requires NO change here — they self-register
 * through the MODULE() macro and module_init_all picks them up.
 * ============================================================================= */

#include "console.h"
#include "shell.h"
#include "shell_provider.h"
#include "printf.h"
#include "acpi.h"
#include "hal_api.h"
#include "multiboot.h"
#include "lapic.h"
#include "ioapic.h"
#include "idt.h"
#include "percpu.h"
#include "smp.h"
#include "pmm.h"
#include "vmm.h"
#include "kmalloc.h"
#include "serial.h"
#include "module.h"
#include "driver.h"
#include "vfs.h"
#include "devfs.h"
#include "procfs.h"
#include "block_cache.h"
#include "block.h"
#include "config.h"
#include "task.h"
#include "timer.h"
#include "vc.h"
#include "lock.h"
#include "keymap.h"
#include <stdint.h>

#define MULTIBOOT1_BOOTLOADER_MAGIC 0x2BADB002

void kernel_main(uint32_t mb_magic, uintptr_t mb_info) {
    /* Earliest debug output.  Sets up the UART and registers the serial
     * console sink — every kprintf below this line lands on COM1. */
    serial_module_init();

    /* Arch essentials behind the HAL (M17).  On x86 this runs TSS,
     * GDT, IDT init in that order; other ports plug in their own
     * `hal_arch_early_init` implementation.  Either way, IRQs may be
     * safely installed after this call returns. */
    hal_arch_early_init();

    /* Capture the multiboot info pointer — many later subsystems (PMM,
     * FB, meminfo) read the mmap and framebuffer fields out of it. */
    if (mb_magic != MULTIBOOT1_BOOTLOADER_MAGIC) {
        kprintf("warning: unexpected multiboot magic %x\n", mb_magic);
    } else {
        kprintf("multiboot info @ %p\n", (void*)mb_info);
    }
    mboot_init(mb_magic, mb_info);

    /* Memory management — physical bitmap, paging, then the kernel heap. */
    pmm_init();
    vmm_init();
    kmalloc_init();

    /* VFS root must exist before any fs module's `register_fs` /
     * `mount` runs in the registry loop below. */
    vfs_init();

    /* Run every registered driver module.  The keyboard, framebuffer
     * terminal, and VGA fallback all register via MODULE(); they come
     * up here without kernel_main having to mention any of them by name. */
    module_init_all();

    /* DRIVER() registry — newer scaffold (M8) with probe/init/shutdown
     * lifecycle.  Coexists with MODULE(); see driver.h for rationale. */
    driver_init_all();

    /* devfs (M9) — flushes any queued driver registrations and registers
     * /dev/null + /dev/zero built-ins.  Must run after the FS is up
     * (ramfs created /dev) and after module/driver init (so all
     * registrations are queued). */
    devfs_init();

    /* procfs (M10) — synthetic /proc files exposing kernel state. */
    procfs_init();

    /* Block cache (M12) — pmm-backed buffer pool sitting between fs
     * code and dev->read/write.  Init here so any fs mounted at boot
     * (none today, but exFAT will be) sees a live cache. */
    bcache_init();

    /* From this point user-visible output flows: the FB sink (or VGA
     * fallback) is active, the serial sink is also active. */
    kprintf("d-os booted.\n");

    /* Configuration store: defaults + overlay from /etc/d-os.conf if
     * present.  Must run after the fs is mounted (module_init_all). */
    config_init();

    /* Keyboard layout (M16) — reads `keyboard.layout` from config and
     * activates one of the built-in layouts (us, hu).  Must run after
     * config_init.  PS/2 and USB drivers consult the active layout via
     * keymap_translate, so no further wiring is needed. */
    keymap_init();

    /* Bring up the task table.  Synthesizes pid 0 = "kernel" from the
     * current execution context so subsequent `task_spawn` calls have
     * something to schedule against. */
    task_init();

    /* ACPI discovery — enables soft-off via `shutdown`, and on the
     * same pass enumerates LAPIC + IOAPIC topology for SMP (M18). */
    acpi_init();

    /* APIC bring-up (M18, x86_64 enabled by M20.5 Phase A).  If ACPI
     * gave us a MADT with a LAPIC + at least one IOAPIC, switch IRQ
     * routing from the 8259 to APIC.  `idt_use_apic` re-routes the
     * already-installed PIT (IRQ0) and PS/2 keyboard (IRQ1) handlers
     * and masks both 8259s.
     *
     * Shared between i386 and x86_64 — lapic.c and ioapic.c compile
     * for both archs (pure MMIO + MSR, no port I/O).  x86_64 SMP AP
     * trampoline + SYSCALL/SYSRET remain in M20.5 Phase B/C. */
    if (acpi_lapic_phys() && acpi_ioapic_phys() && acpi_ncpus() > 0) {
        lapic_init_bsp(acpi_lapic_phys());
        ioapic_init(acpi_ioapic_phys(), acpi_ioapic_gsi_base());
        idt_use_apic(lapic_id());
        /* Per-CPU table init must come AFTER LAPIC bring-up: it reads
         * this_cpu's LAPIC ID to pin the BSP at the right slot. */
        percpu_init_bsp();
    } else {
        kprintf("apic: not available, staying on 8259\n");
    }

    /* IRQs on.  PIT is already firing through IOAPIC at this point
     * (idt_use_apic re-routed it).  We need IRQs for both the LAPIC
     * timer calibration (uses PIT-based timer_msleep) and the AP
     * bring-up SIPI sequence below. */
    hal_intr_enable();

    /* LAPIC timer (M18.5) — per-CPU preempt source.  Calibrate once
     * on the BSP and stash the count so every AP can program its own
     * timer with the same value (LAPICs in one package share the bus
     * clock).  100 Hz target tick = 10 ms quantum upper bound.
     *
     * Active on both i386 and x86_64 since M20.5 Phase A.  On x86_64
     * `smp_boot_aps` is still a stub (returns 0) — Phase B lands the
     * 16→32→64-bit AP trampoline.  Until then x86_64 stays single-CPU
     * even with `-smp N`. */
    if (acpi_lapic_phys()) {
        uint32_t lapic_count = lapic_timer_calibrate(100);
        smp_set_lapic_timer_count(lapic_count);
        lapic_timer_start_periodic(lapic_count, 0x40);
    }

    /* SMP application-processor bring-up — INIT + SIPI + SIPI sequence
     * per Intel SDM Vol 3 §8.4.  No-op on UP (smp_ncpus() == 1).
     * Each AP, in ap_main, loads the IDT, joins the runqueue as an
     * idle task, and arms its own LAPIC timer. */
    if (smp_ncpus() > 1) {
        smp_boot_aps();
    }

    /* Two-level paging path round-trip. */
    {
        uint32_t phys = pmm_alloc_frame();
        uint32_t virt = 0xE0000000u;
        if (phys && vmm_map(virt, phys, VMM_WRITABLE) == 0) {
            volatile uint32_t* p = (volatile uint32_t*)virt;
            *p = 0xDEADBEEFu;
            kprintf("vmm self-test: virt %p -> phys %p, readback=%x\n",
                    (void*)virt, (void*)phys, *p);
            vmm_unmap(virt);
            pmm_free_frame(phys);
        }
    }

    /* Heap alloc / free / reuse round-trip + microbench (M19).
     *
     * The reuse check is the M1-era smoke test: alloc → free → re-alloc
     * the same size should hand back the same address (slab pops the
     * just-freed slot off the magazine LIFO).
     *
     * The microbench does N small allocs + frees and reports wall time
     * + per-op timing.  Sets a baseline so future allocator changes
     * can be compared.  N is bounded to keep boot fast (~few ms). */
    {
        void* a = kmalloc(64);
        void* b = kmalloc(128);
        kfree(a);
        void* c = kmalloc(48);
        kprintf("kmalloc self-test: a=%p b=%p (after free) c=%p [reuse=%s]\n",
                a, b, c, (a == c) ? "yes" : "no");
        kfree(b);
        kfree(c);

        const int N = 10000;
        uint64_t t0 = timer_ticks_ms();
        for (int i = 0; i < N; i++) {
            void* p = kmalloc(64);
            kfree(p);
        }
        uint64_t t1 = timer_ticks_ms();
        kprintf("kmalloc microbench: %d × {alloc(64)+free} in %u ms\n",
                N, (unsigned)(t1 - t0));
    }

    /* exFAT round-trip self-test.
     *
     * Boot N : if /dev/vda hosts an exFAT volume, mount it at /mnt.
     *          Then look for a marker file we may have written on a
     *          previous boot.  Missing -> create + write; present ->
     *          read back.  Across two boots this proves mount + read +
     *          write + persistence end-to-end on the serial log alone. */
    {
        if (vfs_mount("exfat", "/mnt", "vda") == 0) {
            struct file* f = vfs_open("/mnt/dos-marker.txt", VFS_RDONLY);
            if (f) {
                char b[64];
                ssize_t got = vfs_read(f, b, sizeof b - 1);
                if (got > 0) {
                    b[got] = 0;
                    kprintf("exfat self-test: marker present, content=\"%s\"\n", b);
                } else {
                    kprintf("exfat self-test: marker present but empty\n");
                }
                vfs_close(f);
            } else {
                kprintf("exfat self-test: marker missing — creating\n");
                f = vfs_open("/mnt/dos-marker.txt", VFS_WRONLY | VFS_CREATE);
                if (f) {
                    const char msg[] = "wrote-from-dos";
                    vfs_write(f, msg, sizeof msg - 1);
                    vfs_close(f);
                    kprintf("exfat self-test: wrote marker (reboot to verify)\n");
                } else {
                    kprintf("exfat self-test: open-for-create failed\n");
                }
            }
        }
        /* No `else` — if mount fails (no exFAT volume / no disk) we say
         * nothing.  blktest already shouts about a missing /dev/vda. */
    }

    /* Block cache self-test — only runs if a block device is present
     * (the virtio-blk driver registers `vda` when QEMU has -drive).
     * Touches sector 2 (sectors 0 and 1 are owned by future MBR and
     * the blktest pattern respectively) so it doesn't trample anything
     * meaningful.  First access is a miss; second is a hit on the same
     * slot, demonstrating the cache is live. */
    {
        struct block_device* dev = blk_find("vda");
        if (dev) {
            struct bcache_buf* x = bcache_get(dev, 2);
            struct bcache_buf* y = bcache_get(dev, 2);
            if (x && y && x == y) {
                struct bcache_stats s;
                bcache_get_stats(&s);
                kprintf("bcache self-test: hit=%u miss=%u (same slot reused)\n",
                        (unsigned)s.hits, (unsigned)s.misses);
            } else {
                kprintf("bcache self-test: skipped (get failed)\n");
            }
            if (x) bcache_release(x);
            if (y) bcache_release(y);
        }
    }

    /* Preemption self-test (M13).  Spawn a tight-loop task that never
     * yields, then sleep on `hlt` for ~500 ms in the kernel thread.
     * Under cooperative scheduling we'd never wake (the hog would have
     * eaten the CPU on the first IRQ-driven retry).  Under preemption
     * the PIT IRQ pulls us back every quantum.  Success criterion:
     * the kprintf below ever runs AND the hog made some progress. */
    {
        extern volatile uint32_t preempt_test_counter;
        extern volatile int      preempt_test_stop;

        /* Provide a tiny entry — its body is the whole test. */
        extern void preempt_test_entry(void);

        preempt_test_counter = 0;
        preempt_test_stop    = 0;

        /* IRQs are already on (enabled above before SMP bring-up). */

        struct task* hog = task_spawn("preempt-test", preempt_test_entry);
        if (hog) {
            uint64_t deadline = timer_ticks_ms() + 500;
            while (timer_ticks_ms() < deadline) hal_cpu_halt();

            uint32_t ticks = preempt_test_counter;
            preempt_test_stop = 1;
            /* Let the hog observe stop_flag and exit cleanly. */
            uint64_t reap_deadline = timer_ticks_ms() + 100;
            while (timer_ticks_ms() < reap_deadline) hal_cpu_halt();

            if (ticks > 0) {
                kprintf("preempt self-test: PASS — kernel ran while hog tight-looped (hog ticks=%u)\n",
                        (unsigned)ticks);
            } else {
                kprintf("preempt self-test: FAIL — hog never got a slice\n");
            }
        } else {
            kprintf("preempt self-test: skipped (spawn failed)\n");
        }
    }

    /* Parallel-execution self-test (M18.5) — on SMP, two CPU-bound
     * tasks should run in PARALLEL on two different cores.  With
     * per-AP LAPIC timer + AP-side scheduling now active, the
     * scheduler should land hog1 on one CPU and hog2 on another.
     *
     * PASS criterion: both counters are non-zero.  Run BEFORE the
     * shell spawn so kernel_main is the only non-idle RUNNABLE task
     * besides the hogs themselves — keeps the test cheap to schedule
     * under TCG even on -smp 4. */
    if (smp_ncpus() > 1) {
        extern volatile uint32_t par_test_counter1;
        extern volatile uint32_t par_test_counter2;
        extern volatile int      par_test_stop;
        extern void par_test_entry1(void);
        extern void par_test_entry2(void);

        par_test_counter1 = 0;
        par_test_counter2 = 0;
        par_test_stop     = 0;

        struct task* h1 = task_spawn("par-hog1", par_test_entry1);
        struct task* h2 = task_spawn("par-hog2", par_test_entry2);
        if (h1 && h2) {
            uint64_t deadline = timer_ticks_ms() + 500;
            while (timer_ticks_ms() < deadline) hal_cpu_halt();

            uint32_t t1 = par_test_counter1;
            uint32_t t2 = par_test_counter2;
            par_test_stop = 1;
            /* Let them observe stop_flag and exit. */
            uint64_t reap = timer_ticks_ms() + 100;
            while (timer_ticks_ms() < reap) hal_cpu_halt();

            uint64_t total = (uint64_t)t1 + (uint64_t)t2;
            int both_progressed = (t1 > 0 && t2 > 0);
            kprintf("parallel self-test: hog1=%u hog2=%u total=%u\n",
                    (unsigned)t1, (unsigned)t2, (unsigned)total);
            if (both_progressed) {
                kprintf("parallel self-test: PASS — both hogs made progress on %d-CPU system\n",
                        smp_ncpus());
            } else {
                kprintf("parallel self-test: FAIL — one hog starved\n");
            }
        } else {
            kprintf("parallel self-test: skipped (spawn failed)\n");
        }
    }

    /* -----------------------------------------------------------------
     * M14 — bring up the VC subsystem and spawn the first shell on the
     * root pane.  This kernel_main thread (pid 0) then becomes the
     * idle task: just hlt + yield forever.
     *
     * Order is important:
     *   1. vc_init     — paints over the boot log, installs the per-task
     *                    kprintf hook, makes vc_root() valid.
     *   2. preempt_disable + spawn + bind + enable — so the new task's
     *                    very first kprintf already routes into its VC.
     * ----------------------------------------------------------------- */
    /* M27 — spawn the init/reaper task before the shell so every task
     * that follows (the shell and anything it launches, GUI windows, …)
     * has a universal reaper: exited kernel threads no longer leak as
     * zombies, and orphans re-parent to init instead of dangling. */
    task_start_init();

    {
        /* S.1: the boot shell is whatever provider shell.provider
         * selects (config may already be loaded from /etc at this
         * point); default is the full "d-os" shell. */
        vc_init();
        struct vc* root_vc = vc_root();
        if (root_vc) {
            preempt_disable();
            struct task* shell0 =
                task_spawn("shell", shell_provider_active()->entry);
            if (shell0) {
                task_set_out_console(shell0, root_vc);
                root_vc->task = shell0;
            }
            preempt_enable();
            if (!shell0) {
                kprintf("FATAL: failed to spawn shell on root VC\n");
            }
        } else {
            kprintf("FATAL: no root VC — no framebuffer?\n");
        }
    }

    /* Boot complete.  kernel_main was just a worker thread for the
     * init sequence and self-tests; the dedicated BSP idle task (set
     * up in task_init) takes over now via task_exit. */
    task_exit();
}

/* Symbol re-export: kernel_main spawns the shell with this entry,
 * defined in shell.c.  Declared `extern void` above. */

/* -------------------------------------------------------------------- */
/* Preemption self-test shared state.                                    */
/*                                                                      */
/* Defined here (not as locals inside kernel_main) so the spawned task's */
/* entry function can see them across the kernel/task boundary.         */
/* -------------------------------------------------------------------- */

volatile uint32_t preempt_test_counter = 0;
volatile int      preempt_test_stop    = 0;

void preempt_test_entry(void) {
    /* Pure tight loop — no yield, no hlt, nothing IRQ-aware.  If the
     * scheduler is cooperative, this monopolizes the CPU forever.  If
     * preemption is working, the PIT IRQ will pull us out every quantum
     * and let the kernel main thread make progress. */
    while (!preempt_test_stop) {
        preempt_test_counter++;
    }
}

/* M18.5 parallel self-test — two independent hog tasks.  Each
 * increments its own counter; if APs are scheduling real work
 * (LAPIC timer + per-CPU current pointer + cross-CPU pick), both
 * counters should be roughly equal because the scheduler will
 * keep one hog on each available core via the "task_running_
 * elsewhere" check. */
volatile uint32_t par_test_counter1 = 0;
volatile uint32_t par_test_counter2 = 0;
volatile int      par_test_stop     = 0;

void par_test_entry1(void) {
    while (!par_test_stop) par_test_counter1++;
}
void par_test_entry2(void) {
    while (!par_test_stop) par_test_counter2++;
}
