/* =============================================================================
 * boot.S — AArch64 (ARM64) reset entry for the M21 port.
 *
 * QEMU `-M virt -kernel d-os.elf` copies our PT_LOAD segments to physical
 * RAM (base 0x40000000) and jumps here with:
 *   - MMU OFF, D-cache/I-cache effectively cold,
 *   - the CPU at the highest implemented "normal" exception level for the
 *     machine.  Default `virt` (no `virtualization=on`, no `secure=on`)
 *     enters at EL1; `virtualization=on` enters at EL2.  We handle both:
 *     if we wake in EL2 we configure a minimal EL2 and `eret` down to EL1h.
 *   - x0 = physical address of the device-tree blob (QEMU convention).
 *     We stash it for later phases (DTB parsing is a future milestone).
 *
 * Responsibilities (kept to the bare minimum a first C call needs):
 *   1. Drop to EL1 if we came up in EL2.
 *   2. Point SP_EL1 at the linker-provided initial stack.
 *   3. Zero the .bss (C globals + the stack region live there).
 *   4. Call aarch64_main_entry(dtb).
 *
 * There is no port I/O and no descriptor tables on ARM; the exception
 * vector table + MMU are installed later from C (exceptions.c / mmu.c),
 * not here, so this file stays tiny and easy to reason about.
 *
 * References:
 *   Arm ARM (DDI 0487) D1 "The AArch64 System Level Programming Model"
 *     — CurrentEL, SPSR_ELx, HCR_EL2.RW, the eret-to-lower-EL sequence.
 * ============================================================================= */

.section .text.boot
.global _start

_start:
    /* Save the DTB pointer QEMU passed in x0 — x19 is callee-saved and we
     * don't touch it before the C call, so it survives to the bl below. */
    mov     x19, x0

    /* -----------------------------------------------------------------------
     * Which EL did we boot into?  CurrentEL holds the level in bits [3:2].
     * ----------------------------------------------------------------------- */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.eq    .Lin_el2
    /* EL1 (or EL0, which QEMU never does) — nothing to drop through. */
    b       .Lin_el1

.Lin_el2:
    /* -----------------------------------------------------------------------
     * We are in EL2.  Configure the minimum so that an `eret` lands the
     * kernel in EL1 running AArch64, then perform that eret.
     * ----------------------------------------------------------------------- */

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

    /* A sane SCTLR_EL1 with the MMU still OFF.  0x30d00800 sets the
     * architecturally-RES1 bits and leaves caches + MMU disabled; mmu.c
     * later flips M/C/I on. */
    ldr     x0, =0x30d00800
    msr     sctlr_el1, x0

    /* SPSR_EL2 for the eret: DAIF all masked (D,A,I,F = bits 9..6) and
     * target mode EL1h (0b0101) → 0x3c5.  ELR_EL2 = where to resume. */
    mov     x0, #0x3c5
    msr     spsr_el2, x0
    adr     x0, .Lin_el1
    msr     elr_el2, x0
    eret

.Lin_el1:
    /* -----------------------------------------------------------------------
     * Common EL1 bring-up.
     * ----------------------------------------------------------------------- */

    /* Initial stack.  __stack_top is 16-byte aligned by the linker; SP on
     * AArch64 must be 16-byte aligned at every public boundary. */
    ldr     x0, =__stack_top
    mov     sp, x0

    /* Zero .bss: [__bss_start, __bss_end) in 8-byte strides.  The linker
     * guarantees both symbols are 8-byte aligned and end >= start. */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
.Lbss_zero:
    cmp     x0, x1
    b.hs    .Lbss_done
    str     xzr, [x0], #8
    b       .Lbss_zero
.Lbss_done:

    /* Hand the DTB pointer to C and go.  aarch64_main_entry never returns. */
    mov     x0, x19
    bl      aarch64_main_entry

    /* Belt-and-braces: park the CPU if C ever returns. */
.Lhang:
    wfe
    b       .Lhang
