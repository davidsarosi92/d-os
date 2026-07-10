/* =============================================================================
 * tss.c — single static Task State Segment for the kernel (x86_64).
 *
 * The 64-bit TSS is laid out very differently from the 32-bit one:
 *
 *   offset  field         size
 *   0x00    reserved       4 B
 *   0x04    RSP0           8 B  ← kernel stack pointer for ring 3 → ring 0
 *   0x0C    RSP1           8 B
 *   0x14    RSP2           8 B
 *   0x1C    reserved       8 B
 *   0x24    IST1           8 B  ← per-vector kernel stack (optional)
 *   ...     IST2..IST7     8 B each
 *   0x5C    reserved       8 B
 *   0x64    reserved      16 b
 *   0x66    iomap_base    16 b
 *   total                104 B
 *
 * Most fields are 8-byte values starting at 4-byte boundaries — the
 * struct MUST be packed or the compiler will pad to natural alignment
 * and the CPU will read garbage.
 *
 * Why no ESP1/ESP2 array like i386: long mode dropped hardware task
 * switching entirely.  The CPU only ever reads RSP0/1/2 (for the
 * three privilege levels it actually supports) and IST1..7 (for IDT
 * entries that opt into a per-vector stack via their IST field).
 *
 * Reference: Intel SDM Vol 3 §7.7 (Task Management in 64-bit Mode).
 * ============================================================================= */

#include "tss.h"
#include "gdt.h"
#include "hal_api.h"
#include <stdint.h>

struct tss64 {
    uint32_t reserved0;             /* 0x00 */
    uint64_t rsp0;                  /* 0x04 — kernel stack pointer */
    uint64_t rsp1;                  /* 0x0C */
    uint64_t rsp2;                  /* 0x14 */
    uint64_t reserved1;             /* 0x1C */
    uint64_t ist1;                  /* 0x24 */
    uint64_t ist2;                  /* 0x2C */
    uint64_t ist3;                  /* 0x34 */
    uint64_t ist4;                  /* 0x3C */
    uint64_t ist5;                  /* 0x44 */
    uint64_t ist6;                  /* 0x4C */
    uint64_t ist7;                  /* 0x54 */
    uint64_t reserved2;             /* 0x5C */
    uint16_t reserved3;             /* 0x64 */
    uint16_t iomap_base;            /* 0x66 */
} __attribute__((packed));

static struct tss64 tss;

/* Dedicated kernel stack used for ring-3 → ring-0 transitions, mirror
 * of the i386 syscall_stack.  4 KiB is plenty for a syscall handler;
 * deeper paths (nested IRQs etc.) use the regular kernel stack via
 * IST entries instead.  Aligned to 16 bytes to satisfy System V's
 * stack-alignment requirement on call. */
#define KSTACK_SIZE 4096
static uint8_t syscall_stack[KSTACK_SIZE] __attribute__((aligned(16)));

void tss_init(void) {
    /* Zero everything — the per-CPU init code below sets the slots we
     * actually use. */
    uint8_t* b = (uint8_t*)&tss;
    for (uint32_t i = 0; i < sizeof(tss); i++) b[i] = 0;

    tss.rsp0 = (uint64_t)(uintptr_t)(syscall_stack + KSTACK_SIZE);

    /* iomap_base == sizeof(tss) means "no I/O permission bitmap" —
     * any ring-3 IN/OUT traps with #GP (which is what we want; user
     * mode has no business doing port I/O). */
    tss.iomap_base = sizeof(tss);
}

void tss_set_kernel_stack(uintptr_t sp) {
    tss.rsp0 = (uint64_t)sp;
}

/* Tier B — per-task ring-3→ring-0 stack.  `top != 0` = an independent user
 * task's own kernel-stack top; `top == 0` restores the fixed syscall stack
 * (kernel threads + the excursion-model self-tests). */
void hal_set_kernel_stack(uintptr_t top) {
    tss.rsp0 = top ? (uint64_t)top
                   : (uint64_t)(uintptr_t)(syscall_stack + KSTACK_SIZE);
}

uintptr_t tss_get_addr(void)  { return (uintptr_t)&tss; }
uint32_t  tss_get_limit(void) { return sizeof(tss) - 1; }
