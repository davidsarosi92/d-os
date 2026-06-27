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
#include "printf.h"
#include "acpi.h"
#include "hal_api.h"
#include "multiboot.h"
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

void kernel_main(uint32_t mb_magic, uint32_t mb_info) {
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

    /* ACPI discovery — enables soft-off via `shutdown`. */
    acpi_init();

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

    /* Heap alloc / free / reuse round-trip. */
    {
        void* a = kmalloc(64);
        void* b = kmalloc(128);
        kfree(a);
        void* c = kmalloc(48);
        kprintf("kmalloc self-test: a=%p b=%p (after free) c=%p [reuse=%s]\n",
                a, b, c, (a == c) ? "yes" : "no");
        kfree(b);
        kfree(c);
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

        /* Enable interrupts BEFORE spawning the hog — otherwise the
         * first PIT tick that would preempt us never arrives. */
        hal_intr_enable();

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
    {
        extern void shell_task_entry(void);

        vc_init();
        struct vc* root_vc = vc_root();
        if (root_vc) {
            preempt_disable();
            struct task* shell0 = task_spawn("shell", shell_task_entry);
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

    /* IRQs are already on (sti above for the preempt test).  pid 0
     * (this thread) is now the idle task: every preemption picks the
     * shell tasks (or any other spawned worker); we just hlt to save
     * power until the next interrupt and then yield. */
    for (;;) {
        hal_cpu_halt();
        task_yield();
    }
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
