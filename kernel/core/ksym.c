/* =============================================================================
 * ksym.c — the exported symbol table, and the lookup a module loader uses.
 *
 * See ksym.h for why this is a hand-written list rather than a scrape of the
 * linked kernel.  The short version: this file is a PROMISE.  Everything named
 * here is something a module out in the world may be calling, so removing an
 * entry is a breaking change and should feel like one.
 *
 * WHAT GOES IN.  Only what a driver actually needs, and it is worth noticing
 * how short the list is: the AC97 driver — a real one, DMA and interrupts and
 * a class registration — needs about twenty names.  That is the argument for
 * curating the list by hand instead of exporting the tree's whole global
 * namespace: the surface a module can reach is small, and it stays small
 * because somebody has to type each line.
 *
 * WHAT STAYS OUT.  Anything whose contract is "call this only from inside the
 * subsystem", anything holding a lock discipline a module cannot see, and
 * anything that would let a module reach past the class registry it is supposed
 * to be using.  Those are not security boundaries — §M33 is where that argument
 * lives, and one address space makes this advisory — but an export list that
 * omits them at least does not INVITE the mistake.
 * ============================================================================= */

#include "ksym.h"
#include "printf.h"
#include <stddef.h>

static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void* ksym_lookup(const char* name) {
    if (!name) return NULL;
    for (const struct ksym* s = __start_ksyms; s < __stop_ksyms; s++)
        if (s->name && streq(s->name, name)) return s->addr;
    return NULL;
}

int ksym_count(void) {
    return (int)(__stop_ksyms - __start_ksyms);
}

/* Substring match, not prefix: `ksyms audio` should find `audio_register` and
 * also `snd_audio_thing` if one ever exists.  A filter that only matches the
 * start is one the user has to guess the shape of. */
static int contains(const char* hay, const char* needle) {
    if (!needle || !*needle) return 1;
    for (const char* h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

void ksym_list(const char* filter) {
    int shown = 0, total = ksym_count();
    for (const struct ksym* s = __start_ksyms; s < __stop_ksyms; s++) {
        if (!s->name) continue;
        if (!contains(s->name, filter)) continue;
        kprintf("  %p  %s\n", s->addr, s->name);
        shown++;
    }
    if (filter && *filter) kprintf("%d of %d exported symbol(s)\n", shown, total);
    else                   kprintf("%d exported symbol(s)\n", total);
}

/* =============================================================================
 * THE EXPORTS.
 *
 * Grouped by what a driver is doing when it reaches for them.  Each group's
 * comment says what a module is allowed to assume, because an address without
 * a contract is how the first module gets it wrong.
 * ============================================================================= */

#include "kmalloc.h"
#include "pmm.h"
#include "hal_api.h"
#include "hal.h"
#include "klog.h"
#include "driver.h"
#include "audio.h"
#include "task.h"
#include "timer.h"
#include "config.h"
#include "vmm.h"

/* ---- talking to the user ------------------------------------------------- */
EXPORT_SYMBOL(kprintf);
EXPORT_SYMBOL(klog);

/* ---- memory.  A module frees what it allocates; nothing here reclaims on
 * unload, which is exactly why §M66's shutdown hook is mandatory for a module
 * (a driver with no way to withdraw its registrations is REFUSED, and the same
 * reasoning covers the memory it is holding). ------------------------------ */
EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kcalloc);
EXPORT_SYMBOL(kfree);
EXPORT_SYMBOL(pmm_alloc_frame_dma32);
EXPORT_SYMBOL(pmm_alloc_contiguous_dma32);
EXPORT_SYMBOL(pmm_free_frame);

/* ---- interrupts and buses.
 *
 * ARCH-CONDITIONAL, AND THE ASYMMETRY IS REAL RATHER THAN AN OVERSIGHT: x86
 * registers a handler with `irq_install(line, fn)` through the IDT, aarch64
 * with `gic_register_handler(intid, fn)` + an explicit enable.  There is no
 * portable IRQ registration in this tree to export instead, and inventing one
 * here — a shim that exists only so this list could be arch-free — would be a
 * new interface with exactly one caller.
 *
 * The practical consequence is small: a driver for a PCI card is x86-only for
 * PCI's own reasons long before it is x86-only for this.
 *
 * `irq_install` does NOT chain (§M23 stage 6), so a module taking a line that
 * is already claimed is a collision, and it must say so rather than assume it
 * shares. ------------------------------------------------------------------ */
#if defined(__i386__) || defined(__x86_64__)
#include "idt.h"
#include "pci.h"
EXPORT_SYMBOL(irq_install);

/* Port I/O is a real call here, not an inline — so a module that touches a
 * legacy device needs these by name. */
EXPORT_SYMBOL(inb);
EXPORT_SYMBOL(inw);
EXPORT_SYMBOL(outb);
EXPORT_SYMBOL(outw);
EXPORT_SYMBOL(outl);

/* PCI: enumeration + the BAR assignment §M66 added for hot-plugged devices,
 * which arrive with no BARs because firmware only programs the ones present at
 * boot.  A module driving a hot-added card needs it for the same reason a
 * built-in driver does. */
EXPORT_SYMBOL(pci_find_device);
EXPORT_SYMBOL(pci_read16);
EXPORT_SYMBOL(pci_write16);
EXPORT_SYMBOL(pci_bar_io_base);
EXPORT_SYMBOL(pci_assign_bars);

/* Mapping an MMIO BAR.  IN THE x86 BLOCK, not next to the portable exports:
 * aarch64 has no `vmm_map` of this shape — its kernel runs on a 1 GiB-block
 * identity map and MMIO windows are attached by a different call.  Listing it
 * unconditionally FAILED THE LINK on that arch, which is the interface saying
 * out loud that this is not a portable name. */
EXPORT_SYMBOL(vmm_map);
#endif

/* ---- waiting.  A driver that spins instead of sleeping costs a CPU for the
 * duration — the mistake §M23 stage 2 found in this tree's own AC97 driver,
 * where a three-second sound burned three seconds of a core. --------------- */
EXPORT_SYMBOL(task_msleep);
EXPORT_SYMBOL(timer_ticks_ms);
EXPORT_SYMBOL(timer_now_ns);
EXPORT_SYMBOL(hal_cpu_pause);

/* ---- configuration.  A module reads its keys the same way built-in code
 * does, so `drivers.rescan_ms` and a module's own key are the same kind of
 * thing to the Control Panel. ---------------------------------------------- */
EXPORT_SYMBOL(config_get);
EXPORT_SYMBOL(config_get_long);

/* ---- the driver registry itself.  A module normally does not call these —
 * the loader attaches its descriptor — but a module that manages several
 * devices needs `driver_find` and the fault report. ------------------------ */
EXPORT_SYMBOL(driver_attach);
EXPORT_SYMBOL(driver_find);
EXPORT_SYMBOL(driver_fault);

/* ---- the audio class.  This is the registry an audio module publishes into;
 * the refcount pair is what makes its device removable while somebody might be
 * inside a call (§M66). ---------------------------------------------------- */
EXPORT_SYMBOL(audio_register);
EXPORT_SYMBOL(audio_unregister);

/* ---- the network class.  `net_rx` is how a driver hands a received frame UP;
 * it runs with the stack lock held, which is why §M55's rule that the RX path
 * may never resolve an ARP entry applies to a module exactly as it does to a
 * built-in driver. ---------------------------------------------------------- */
#include "net.h"
EXPORT_SYMBOL(net_register);
EXPORT_SYMBOL(net_unregister);
EXPORT_SYMBOL(net_rx);

/* =============================================================================
 * THE COMPILER'S RUNTIME — the part of an export table nobody designs for.
 *
 * The first module built against this list failed to load on ONE symbol, and it
 * was not a kernel function: `__udivdi3`.  A 64-bit division on i386 is not an
 * instruction, so GCC emits a call into libgcc, and libgcc is linked into the
 * kernel rather than into the module.  Nothing in the driver's source mentions
 * it; it appears because the driver divides a `uint64_t`.
 *
 * That is worth writing down because of how it would look to somebody porting a
 * driver: the code compiles, the module builds, and `insmod` refuses it naming
 * a symbol that occurs nowhere in the source.  Exporting these makes the
 * question not arise — and it is what Linux does with the same helpers for the
 * same reason.
 *
 * i386 ONLY.  x86_64 and aarch64 divide and shift 64-bit values in hardware, so
 * there is no call to export.  Listing them anyway would fail the LINK on those
 * arches (libgcc has no such object to pull in), which is a tidy demonstration
 * that this really is arch-specific rather than a portability oversight.
 * ============================================================================= */
#if defined(__i386__)
extern long long          __divdi3(long long, long long);
extern long long          __moddi3(long long, long long);
extern unsigned long long __udivdi3(unsigned long long, unsigned long long);
extern unsigned long long __umoddi3(unsigned long long, unsigned long long);
extern long long          __ashldi3(long long, int);
extern long long          __ashrdi3(long long, int);
extern unsigned long long __lshrdi3(unsigned long long, int);

EXPORT_SYMBOL(__divdi3);
EXPORT_SYMBOL(__moddi3);
EXPORT_SYMBOL(__udivdi3);
EXPORT_SYMBOL(__umoddi3);
EXPORT_SYMBOL(__ashldi3);
EXPORT_SYMBOL(__ashrdi3);
EXPORT_SYMBOL(__lshrdi3);
#endif
