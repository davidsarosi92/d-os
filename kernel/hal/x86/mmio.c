/* =============================================================================
 * mmio.c — hal_mmio_map on x86 (both arches) — see hal_api.h.
 *
 * x86 reaches device registers through an IDENTITY mapping: the window is
 * mapped uncached in 4 MiB (i386 PSE) / 2 MiB-backed chunks at its own
 * physical address, exactly what xhci/ahci/e1000e did by hand before this
 * call existed.  The pointer is the physical address.  (Moving x86_64's
 * device space and kernel to the upper half is the follow-up; callers do not
 * change when it happens.)
 * ============================================================================= */
#include "hal_api.h"
#include "vmm.h"
#include <stdint.h>
#include <stddef.h>

volatile void* hal_mmio_map(uint64_t phys, size_t len) {
    if (!phys) return NULL;
    uintptr_t start = (uintptr_t)phys & ~(uintptr_t)0x3FFFFF;
    uintptr_t end   = (uintptr_t)(phys + (len ? len : 1));
    for (uintptr_t a = start; a < end; a += 0x400000)
        vmm_map_4mib(a, a, VMM_WRITABLE | VMM_CACHE_DIS);   /* already there: harmless */
    return (volatile void*)(uintptr_t)phys;
}
