/* =============================================================================
 * pl031_drv.c — the ARM PL031 real-time clock, written against drvrt (§M78).
 *
 * WHY THIS EXISTS, AND IT IS NOT "TO DRIVE THIS DEVICE".
 *
 * §M33 built driver placement — a driver running in ring 3, supervised,
 * restarted, its device confined — and measured it on both x86 architectures.
 * §M76 opened the seven portable `SYS_DRV_*` doorways on aarch64, and left an
 * item stated honestly: *reachable is not proven.*
 *
 * Testing it turned up something better than a pass or a fail.  §M33's only
 * client is QEMU's `edu`, and on `-M virt` it is PRESENT and cannot initialise:
 *
 *     edu: no DMA buffer at 28 address bits (-3) — nothing free that low
 *
 * `edu` addresses DMA with 28 bits — 256 MiB — and this machine's RAM starts at
 * 1 GiB.  There is no address below 2^28 at all, so no allocation policy can
 * satisfy it.  **That is a fact about the DEVICE, not about the placement
 * machinery**, and the two must not be conflated.
 *
 * So the thing that was actually missing is a client whose requirements this
 * architecture can meet.  THIS DRIVER IS MMIO-ONLY AND DELIBERATELY SO: no DMA,
 * no interrupt, no port I/O — the three things that are either impossible here
 * (ports) or constrained by the machine's memory map (DMA).  What is left is
 * exactly the question the open item asks: *does a driver placed in ring 3 on
 * ARM get its device's registers and nothing else?*
 *
 * IT IS A REAL DEVICE AND NOT A SYNTHETIC ONE.  The PL031 is on every `virt`
 * board, the desktop's taskbar clock reads it, and a wrong answer is visible on
 * screen rather than only in a test's verdict — which is what §M33 wanted when
 * it wrote down that "a synthetic client cannot answer whether the interface is
 * pleasant for a COMPLICATED driver".  This is not complicated; it is the
 * smallest real one, which makes it the right FIRST one.
 *
 * -----------------------------------------------------------------------------
 * WHAT IT DOES NOT REPLACE.
 *
 * `hal/aarch64/pl031_rtc.c` still implements `rtc_read()` for the boot path and
 * the taskbar, reading the same register through the identity map.  This is a
 * SECOND reader of one device, and that is safe for one reason worth stating:
 * the PL031's data register is READ-ONLY and free-running, so two readers
 * cannot interfere — there is no state to race over.  A device with a command
 * register would need one of them to give way.
 * ============================================================================= */

#include "drvrt.h"
/* The ring-3 shim (user/drvrt_user.c) supplies `kprintf`, `task_should_stop`
 * and `task_msleep` under the SAME names the kernel does — which is the whole
 * of drvrt's claim, and why this file has no #ifdef inside its body. */
#ifdef DRV_USERSPACE
void kprintf(const char* fmt, ...);
int  task_should_stop(void);
void task_msleep(unsigned ms);
#else
#include "task.h"
#endif
#include <stdint.h>
#include <stddef.h>

#ifndef DRV_USERSPACE
#include "driver.h"
#include "hal_api.h"
#include "printf.h"
#include "config.h"
#endif

/* QEMU `virt`: PL031 at 0x0901_0000.  One 4 KiB page covers every register. */
#define PL031_PHYS   0x09010000ULL
#define PL031_LEN    0x1000
#define PL031_DR     0x000        /* data register — seconds since the epoch  */
#define PL031_CR     0x00C        /* control register — bit 0 = enabled       */
#define PL031_PID0   0xFE0        /* peripheral ID, 0x31 for a PL031          */

static struct drv_rt    rt;
static drv_handle       h_mmio = -1;
static volatile uint8_t* regs;

static inline uint32_t pl_rd(uint32_t off) {
    return *(volatile uint32_t*)(regs + off);
}

/* ---------------------------------------------------------------------------
 * The body.  Identical in both backends — which is the whole claim drvrt makes,
 * and the reason this file has no `#ifdef` inside it.
 * ------------------------------------------------------------------------- */
static int pl031_bringup(void) {
    drv_rt_init(&rt, "pl031");

    /* ASK FOR THE WINDOW BEFORE MAPPING IT, and the asymmetry between the two
     * backends is the point rather than a wrinkle.
     *
     * A PLACED driver cannot know where its device is: it has no bus access
     * and must not have any, so the kernel TELLS it — from the manifest, which
     * for a platform device declares the address outright (§M78).  An
     * in-kernel driver may use the board constant, because it is the kernel.
     *
     * So the window is asked for first and the constant is the FALLBACK, not
     * the other way round.  Written this way so the placed path is the one
     * exercised by default: a driver that used its constant first would work
     * in ring 3 by accident and stop the moment the manifest disagreed with
     * it — which is exactly the disagreement the manifest exists to catch. */
    uint64_t win = PL031_PHYS, wlen = PL031_LEN;
    if (drv_device_window(&rt, 0, &win, &wlen) != 0 || !win) {
#ifndef DRV_USERSPACE
        /* In the kernel: the BOARD's answer (§M85), not `virt`'s constant. */
        if (hal_platform_window("pl031", &win, &wlen) != 0) return -1;
#else
        win = PL031_PHYS; wlen = PL031_LEN;
#endif
    }
    h_mmio = drv_mmio_request(&rt, win, wlen < PL031_LEN ? PL031_LEN : wlen,
                              "PL031 registers");
    if (h_mmio < 0) {
        kprintf("pl031: no MMIO window — refused\n");
        return -1;
    }
    regs = (volatile uint8_t*)drv_mmio_ptr(h_mmio);
    if (!regs) { kprintf("pl031: MMIO handle has no pointer\n"); return -1; }

    /* IDENTIFY THE DEVICE BEFORE BELIEVING THE ADDRESS.  A placed driver is
     * handed a window by the kernel and cannot check the platform for itself;
     * if the window is wrong, every later read is a plausible number from
     * somewhere else.  The PL031's peripheral ID says what it is. */
    uint32_t pid = pl_rd(PL031_PID0) & 0xFF;
    if (pid != 0x31) {
        kprintf("pl031: peripheral ID is not 0x31 — this is not a PL031\n");
        return -1;
    }

    /* The clock is free-running on QEMU and CR bit 0 reads back set; this is a
     * report, not a configuration step — there is nothing to program. */
    kprintf(pl_rd(PL031_CR) & 1 ? "pl031: up, clock enabled\n"
                                : "pl031: up, clock reports DISABLED\n");
    return 0;
}

/* Seconds since the epoch, straight from the device.  Exposed so the placement
 * test can compare what the PLACED driver reads with what the kernel's own
 * reader sees — two independent paths to one register, which is what makes the
 * test falsifiable rather than a driver agreeing with itself. */
uint32_t pl031_drv_seconds(void) {
    return regs ? pl_rd(PL031_DR) : 0;
}

#ifdef DRV_USERSPACE
/* ---------------------------------------------------------------------------
 * The ring-3 half.  §M33's placed drivers are ordinary programs: bring the
 * device up, then serve until the kernel asks us to stop.
 *
 * `drv_should_stop` is the cooperative stop that crosses the process boundary
 * (DRV_ESTOP) — §M33 found that a placed driver whose stop was an
 * honest-looking stub was effectively unkillable.
 * ------------------------------------------------------------------------- */
int main(void) {
    if (pl031_bringup() != 0) return 1;

    uint32_t last = 0;
    for (;;) {
        if (task_should_stop()) break;
        uint32_t now = pl031_drv_seconds();
        /* One line a minute, so a log shows the device is being READ from ring
         * 3 rather than merely opened there — the difference between a
         * placement that works and one that got as far as its first grant. */
        if (now / 60 != last / 60) {
            last = now;
            kprintf("pl031: tick %d (reading DR from ring 3)\n", (int)now);
        }
        task_msleep(500);
    }
    drv_release_all(&rt);
    return 0;
}
#else
/* ---------------------------------------------------------------------------
 * The in-kernel half.
 * ------------------------------------------------------------------------- */
static int pl031_probe(void* ctx) {
    (void)ctx;
    /* §M85 — only if THIS machine describes one.  "This board always has
     * one" was true of `virt` only; on sbsa-ref the driver read 0x0901_0fe0,
     * found no device and took an external abort (contained by §M33). */
    uint64_t b, l;
    if (hal_platform_window("pl031", &b, &l) != 0) return -1;
    /* This board always has one, and a probe that reads the register before the
     * window is granted would be reading through the identity map — which is
     * exactly the assumption a placed driver may not make.  So the probe says
     * "this platform has a PL031" and the IDENTITY CHECK lives in bring-up,
     * where the window is real in both backends. */
    return 0;
}

static int pl031_init(void* ctx) {
    (void)ctx;
    return pl031_bringup();
}

static int pl031_shutdown(void* ctx) {
    (void)ctx;
    /* A driver with no shutdown hook is REFUSED at load (§M67): code that can
     * never be stopped is a module that can never be removed. */
    drv_release_all(&rt);
    regs = NULL;
    h_mmio = -1;
    return 0;
}

static const struct driver_ops pl031_ops = {
    .probe = pl031_probe, .init = pl031_init, .shutdown = pl031_shutdown,
};

/* DOMAIN_KERNEL | DOMAIN_USER and NO DRVF_DMA — the declaration is the point.
 *
 * §M33's rule: `.domains` is a CAPABILITY OF THE CODE and config chooses among
 * the declared set without widening it.  This driver really can run in either,
 * because it asks for nothing this architecture cannot grant — and because it
 * takes no DMA, `drv domain` can report `isolation full` for it without an
 * IOMMU being involved at all.  That is the first time that answer is available
 * on this architecture. */
DRIVER_EX(pl031, "rtc", &pl031_ops, NULL, DOMAIN_KERNEL | DOMAIN_USER, 0);
#endif
