/* =============================================================================
 * smp_entry.S — AArch64 secondary-core entry trampoline (M21 Phase E).
 *
 * PSCI CPU_ON releases a secondary vCPU here (physical entry point) with the
 * MMU OFF, caches cold, at EL1.  There is no shared low-memory trampoline like
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

.section .text
.global smp_secondary_entry

smp_secondary_entry:
    /* MMU-off SCTLR with the architecturally-RES1 bits set (same value the
     * BSP uses in boot.S before enabling the MMU). */
    ldr     x0, =0x30d00800
    msr     sctlr_el1, x0
    isb

    /* cpu index = MPIDR_EL1.Aff0 (linear on QEMU `virt`). */
    mrs     x9, mpidr_el1
    and     x9, x9, #0xff

    /* sp = ap_sp[cpu]  (ap_sp is a global uint64_t[] filled by the BSP). */
    ldr     x10, =ap_sp
    lsl     x11, x9, #3
    ldr     x12, [x10, x11]
    mov     sp, x12

    /* smp_secondary_main(cpu). */
    mov     x0, x9
    bl      smp_secondary_main

1:  wfe
    b       1b
