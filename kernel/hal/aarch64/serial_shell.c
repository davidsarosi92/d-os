/* =============================================================================
 * serial_shell.c — interactive PL011 serial shell for the AArch64 port
 * (M21 Phase D).
 *
 * The x86 shell (kernel/core/shell.c) reads its input from a framebuffer-
 * backed VC (vc_getchar) and its command set is welded to subsystems that are
 * themselves x86-specific or not-yet-ported on ARM (the GUI compositor, the
 * ring-3 usermode path, vmm.c, the block/USB drivers).  Reaching THAT shell
 * verbatim needs the framebuffer + VC + driver ports — several later phases.
 *
 * So Phase D brings up a genuine interactive REPL over the UART instead: it
 * runs as an ordinary scheduler task, reads lines from the PL011 (polling +
 * task_yield, so the timer keeps preempting underneath), and drives the
 * PORTABLE kernel services already up on ARM — the PMM, the scheduler, and
 * the VFS/ramfs — with a core command set (help, meminfo, ps, uptime, ls,
 * cat, mkdir, write, rm, echo, clear).  This proves an interactive shell +
 * a real in-memory filesystem on ARM64; growing it into the full shell.c is
 * gated on the framebuffer/driver ports.
 * ============================================================================= */

#include "printf.h"
#include "shellcmd.h"   /* §M70 — one command registry, both shells */
#include "net.h"
#include "config.h"       /* §M63 stage 0 — setconf/getconf/saveconf */
#include "console_plate.h"
#include "wallpaper.h"
#include "shortcut.h"
#include "settings.h"
#include "clipboard.h"
#include "gui.h"
#include "splash.h"
#include "watchdog.h"   /* §M60 — one implementation, both shells */
#include "pmm.h"
#include "task.h"
#include "percpu.h"    /* §M57 — smp_ncpus() for the runqueue audit + storm */
#include "timer.h"
#include "audio.h"
#include "driver.h"
#include "iommu.h"
#include "modload.h"   /* §M67 — insmod / rmmod / lsmod */
#include "ksym.h"      /* §M67 — ksyms */
#include "syscall.h"   /* §M53 stage 3 — timerfd + setitimer self-tests */
#include "ktimer.h"
#include "pkg.h"
#include "vfs.h"
#include "block.h"
#include <stdint.h>
#include <stddef.h>

int      uart_early_getchar(void);   /* uart.c — non-blocking RX             */
void     uart_early_putc(char c);
uint64_t timer_ticks_ms(void);       /* timer.c                              */




/* ---- line editor ----------------------------------------------------------- */
#define LINE_MAX 128

static void read_line(char* buf, int cap) {
    int n = 0;
    for (;;) {
        int c = uart_early_getchar();
        /* §M75.2 — HALT, don't spin.  `task_yield()` leaves this task RUNNABLE,
         * so a REPL waiting for a keystroke stayed on a core for ever: §M75's
         * chart measured **50 % of a 2-CPU ARM box** with nothing running but
         * this prompt.  Unlike the x86 GUI loops — whose 50 % turned out to be
         * halted time miscounted as busy — this one was REAL work: yielding in
         * a tight loop is a spin with better manners.
         *
         * `task_halt_idle` halts until the next interrupt, which on this arch
         * is the 100 Hz tick, so a keystroke is noticed within 10 ms.  That is
         * invisible on a serial console and is why the same change would NOT
         * be right for the compositor, where §M22.7 measured exactly this trade
         * as visible cursor lag.  The real fix is a PL011 RX interrupt; this is
         * the honest interim, and it costs latency rather than liveness. */
        if (c < 0) { task_halt_idle(); continue; }

        if (c == '\r' || c == '\n') {
            kprintf("\n");
            buf[n] = 0;
            return;
        }
        if (c == 0x7f || c == 0x08) {              /* DEL / Backspace        */
            if (n > 0) { n--; kprintf("\b \b"); }
            continue;
        }
        if (c >= 32 && c < 127 && n < cap - 1) {
            buf[n++] = (char)c;
            uart_early_putc((char)c);              /* echo                   */
        }
    }
}

/* ---- commands -------------------------------------------------------------- */



/* Phase L — the EL0/userspace self-test (syscall.c), the ARM analogue of the
 * x86 shell's `ringtest`.  Runs a tiny program at EL0 that SYS_PRINTs + SYS_EXITs. */
int aarch64_usertest(void);
static void cmd_usertest(void) { aarch64_usertest(); }

/* §A1 — the POSIX self-tests.  These live here rather than in shell.c because
 * the ARM serial console runs THIS minimal REPL; the full shell.c only comes up
 * on a VC once the framebuffer + virtio-input path is running, which a headless
 * boot has no way to drive.  PLAN_AARCH64's definition of done for A1 is
 * "passes over the serial shell" for exactly that reason. */
extern const unsigned char _binary_user_forktest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_forktest_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_pipetest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_pipetest_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_sigtest_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_sigtest_elf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_epollmusl_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_epollmusl_muslelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_muslhello_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_muslhello_muslelf_end[]   __attribute__((weak));

int proc_exec_elf(const unsigned char* image, unsigned long len);




























/* Parse a small non-negative decimal; returns 0 for empty/invalid. */
static uint64_t parse_u64(const char* s) {
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); s++; }
    return v;
}

/* kprintf() supports no field width, so emit two-digit hex by hand. */
static void put_hex8(uint8_t v) {
    static const char d[] = "0123456789abcdef";
    uart_early_putc(d[(v >> 4) & 0xf]);
    uart_early_putc(d[v & 0xf]);
}

/* Read one sector from /dev/vda and hexdump the first 64 bytes — proof the
 * virtio-mmio block driver does real disk I/O (Phase F). */
static void cmd_blk(const char* args) {
    struct block_device* dev = blk_find("vda");
    if (!dev) { kprintf("blk: no /dev/vda (attach a virtio-blk-device)\n"); return; }

    uint64_t lba = parse_u64(args);
    static uint8_t sec[512];
    if (blk_read(dev, lba, 1, sec) != 0) { kprintf("blk: read LBA %u failed\n", (unsigned)lba); return; }

    kprintf("vda LBA %u (%u sectors total), first 64 bytes:\n",
            (unsigned)lba, (unsigned)dev->sector_count);
    for (int row = 0; row < 4; row++) {
        uart_early_putc(' '); uart_early_putc(' ');
        put_hex8((uint8_t)(row * 16)); uart_early_putc(':');
        for (int i = 0; i < 16; i++) { uart_early_putc(' '); put_hex8(sec[row * 16 + i]); }
        uart_early_putc(' '); uart_early_putc(' ');
        for (int i = 0; i < 16; i++) {
            uint8_t c = sec[row * 16 + i];
            uart_early_putc((c >= 32 && c < 127) ? (char)c : '.');
        }
        uart_early_putc('\n');
    }
}

/* ---- REPL ------------------------------------------------------------------ */

void serial_shell_entry(void) {
    kprintf("\nWelcome to d-os on AArch64.  Type 'help'.\n");

    char line[LINE_MAX];
    for (;;) {
        kprintf("d-os> ");
        read_line(line, LINE_MAX);

        /* §M70 — ONE registry, both shells.
         *
         * This used to be sixty hand-written `else if` arms mirroring the ones
         * in shell.c, and the mirroring was the whole problem: a command
         * written into shell.c existed on x86 and simply did not exist here
         * until somebody remembered to add a second arm.  §M24's rule ("the
         * implementation lives in its own .c, both shells call one copy") was
         * followed for a handful of files and quietly broken by everything
         * else, because writing into shell.c was always the cheaper move.
         *
         * Now a SHELL_CMD() registration is on every architecture that links
         * the file it lives in, and there is nothing to keep in step. */
        if (!shell_cmd_dispatch(line))
            kprintf("unknown command (try 'help')\n");
    }
}

/* --- §M70: the two commands that really are this architecture's ------------
 * `usertest` drives the EL0 excursion through aarch64's own usermode.S, and
 * `blk` probes the virtio-MMIO block device this arch reaches differently from
 * either x86 target.  Everything else the ARM shell answers comes from the
 * shared registry, which is the point — these two are registered HERE, in an
 * arch file, because they are the only ones that would be wrong anywhere else. */
static void ar_usertest(const char* a) { (void)a; cmd_usertest(); }

SHELL_CMD(usertest) = { "usertest", "", "run a program at EL0 and come back",
                        SHELL_G_TEST, ar_usertest, SHELL_P_ADMIN };
SHELL_CMD(blk)      = { "blk", "[dev]", "virtio-MMIO block device probe",
                        SHELL_G_DEV, cmd_blk, SHELL_P_ADMIN };
