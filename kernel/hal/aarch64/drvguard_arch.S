/* =============================================================================
 * drvguard_arch.S — §M33 Tier 0, the aarch64 half.  See the i386 file for the
 * design.
 *
 * regs[] layout:
 *   [0..9] x19..x28   [10] x29 (FP)   [11] x30 (LR = resume)   [12] SP
 *
 * NOTE THE SIZE: this arch needs thirteen slots where x86 needs six, because
 * the AAPCS callee-saved set is x19..x28 plus FP and LR.  `struct drvguard_ctx`
 * sizes `regs[]` for the largest of the three — a shared struct whose array is
 * sized for one arch and read by another is the §M56 `epoll_event` trap, and it
 * is cheaper to over-allocate ten words than to make the struct arch-conditional.
 *
 * The landing pad takes the regs pointer in X0.
 * ============================================================================= */

.text
.global drvguard_save
.global drvguard_land

/* int drvguard_save(uintptr_t* regs)  — regs in x0 */
drvguard_save:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]     /* FP and the return address            */
    mov     x1, sp
    str     x1, [x0, #96]           /* SP: this is a leaf, so it is already
                                     * the caller's — nothing was pushed.   */
    mov     w0, #0
    ret

/* void drvguard_land(void) — entered from the fault handler with regs in x0.
 * `eret` restores PC from ELR but leaves SP_EL1 as the handler left it, so the
 * stack switch happens here, exactly as it does on i386. */
drvguard_land:
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x1, [x0, #96]
    mov     sp, x1
    mov     w0, #1
    ret                             /* to x30 = drvguard_save's caller */
