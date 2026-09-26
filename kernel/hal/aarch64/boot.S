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
#include "el2_drop.h"

.global _start

_start:
    /* Save the DTB pointer QEMU passed in x0 — x19 is callee-saved and we
     * don't touch it before the C call, so it survives to the bl below. */
    mov     x19, x0

    /* Leave EL2 if we were started there (el2_drop.h — shared with the
     * secondary-CPU entry, which firmware starts at the same EL). */
    DROP_TO_EL1 .Lin_el1

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
