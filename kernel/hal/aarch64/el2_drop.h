/* =============================================================================
 * el2_drop.h — leave EL2 for EL1, the same way on EVERY entry path (§M85).
 *
 * Included (through the C preprocessor) by boot.S AND smp_entry.S.  It used to
 * live in boot.S only, which was enough while the only way to reach EL2 was the
 * boot CPU under `virtualization=on`.  Firmware changes that: PSCI CPU_ON
 * starts each SECONDARY core at the EL the firmware runs its callers at — EL2
 * under UEFI — and smp_entry.S assumed EL1.  The secondary then configured
 * EL1's registers while EXECUTING at EL2 with its MMU off, took the zone lock
 * with non-cacheable accesses the boot CPU could not see, and the boot CPU
 * waited on that lock forever (`!! SPINLOCK STUCK` in buddy_alloc_in_zone).
 * One macro, so a third entry path inherits it instead of being remembered.
 *
 * Clobbers x0, x1.  Falls through at EL1; from EL2 it `eret`s to \target.
 * ============================================================================= */
.macro DROP_TO_EL1 target
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    \target
    /* HCR_EL2.RW = 1 → EL1 executes in AArch64 (not AArch32). */
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0

    /* Give EL1 access to the architected timer without trapping to EL2:
     *   CNTHCTL_EL2.EL1PCTEN (bit 0) + EL1PCEN (bit 1) = 1,
     *   CNTVOFF_EL2 = 0 so the virtual counter equals the physical one. */
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr

    /* §M85 stage 2 — a GICv3 system-register interface is reachable from EL1
     * only if EL2 says so: ICC_SRE_EL2.Enable (bit 3) lets EL1 use
     * ICC_SRE_EL1, and SRE (bit 0) selects the register interface.  Without
     * it EL1's ICC_* accesses trap to EL2 — which, once we have left it, has
     * no handler.  UEFI firmware (sbsa-ref) enters us at EL2, so this is the
     * path that board takes.  Guarded by ID_AA64PFR0_EL1.GIC (bits 27:24):
     * a CPU without the interface would UNDEF on the msr. */
    mrs     x0, id_aa64pfr0_el1
    ubfx    x0, x0, #24, #4
    cbz     x0, 71f
    mrs     x0, S3_4_C12_C9_5              /* ICC_SRE_EL2 */
    mov     x1, #0x9                       /* Enable | SRE — 0b1001 is not a
                                            * valid logical immediate, so it
                                            * goes through a register */
    orr     x0, x0, x1
    msr     S3_4_C12_C9_5, x0
    isb
    msr     S3_4_C12_C11_0, xzr            /* ICH_HCR_EL2: no virtual CPU IF */
71:

    /* A sane SCTLR_EL1 with the MMU still OFF.  0x30d00800 sets the
     * architecturally-RES1 bits and leaves caches + MMU disabled; mmu.c
     * later flips M/C/I on. */
    ldr     x0, =0x30d00800
    msr     sctlr_el1, x0

    /* SPSR_EL2 for the eret: DAIF all masked (D,A,I,F = bits 9..6) and
     * target mode EL1h (0b0101) → 0x3c5.  ELR_EL2 = where to resume. */
    mov     x0, #0x3c5
    msr     spsr_el2, x0
    adr     x0, \target
    msr     elr_el2, x0
    eret
.endm
