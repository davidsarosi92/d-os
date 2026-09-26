/* =============================================================================
 * smp_entry.S — AArch64 secondary-core entry trampoline (M21 Phase E).
 *
 * PSCI CPU_ON releases a secondary vCPU here (physical entry point) with the
 * MMU OFF, caches cold — at EL1 on raw `virt`, at EL2 under firmware.  There is no shared low-memory trampoline like
 * x86's AP_BOOT_ADDR — the entry is just our linked address, reachable because
 * the kernel is identity-mapped.
 *
 * Job (kept minimal; the rest is C in smp_secondary_main):
 *   1. Load a sane MMU-off SCTLR_EL1 (RES1 bits set, MMU/caches off).
 *   2. Derive this core's index from MPIDR_EL1 Aff0 and load its private
 *      stack top from the ap_sp[] table the BSP filled before CPU_ON.
 *   3. Call smp_secondary_main(cpu) — which turns the MMU on and joins the
 *      scheduler.  It never returns; park just in case.
 * ============================================================================= */

#include "el2_drop.h"

.section .text
.global smp_secondary_entry

smp_secondary_entry:
    /* Firmware may start a secondary at EL2 (see el2_drop.h). */
    DROP_TO_EL1 .Lap_el1
.Lap_el1:
    /* MMU-off SCTLR with the architecturally-RES1 bits set. */
    ldr     x0, =0x30d00800
    msr     sctlr_el1, x0
    isb

    /* cpu index = MPIDR_EL1.Aff0 (linear on QEMU `virt`). */
    mrs     x20, mpidr_el1
    and     x20, x20, #0xff

    /* §M85 — this runs at the image's PHYSICAL address with the MMU off, so
     * the stack pointer the BSP published (a virtual address) is converted
     * with the image offset first; both are read PC-relatively, and smp.c
     * cleaned them to the point of coherency before CPU_ON, because this core
     * reads them uncached. */
    adrp    x10, ap_sp
    add     x10, x10, :lo12:ap_sp
    ldr     x12, [x10, x20, lsl #3]
    adrp    x11, aarch64_kimage_voffset
    ldr     x11, [x11, :lo12:aarch64_kimage_voffset]
    sub     x12, x12, x11
    mov     sp, x12

    /* MMU on (identity map + kernel half), then up to the virtual address. */
    bl      aarch64_mmu_enable_this_cpu
    ldr     x0, =.Lap_high
    br      x0
.Lap_high:
    ldr     x10, =ap_sp
    ldr     x12, [x10, x20, lsl #3]
    mov     sp, x12
    bl      aarch64_mmu_drop_idmap

    /* smp_secondary_main(cpu). */
    mov     x0, x20
    bl      smp_secondary_main

1:  wfe
    b       1b
