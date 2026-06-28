/* =============================================================================
 * tss.c — single static Task State Segment for the kernel.
 *
 * The 386 TSS is a 104-byte structure with one field that matters today:
 * `esp0`, the kernel ESP the CPU swaps in when a ring-3 → ring-0
 * transition happens.  We allocate a dedicated 4 KiB buffer to serve as
 * that stack so syscall / IRQ entry from ring 3 doesn't trample the
 * kernel's main stack used by `enter_user_mode_wrap`.
 *
 * `ss0` is the kernel data selector.  All other TSS fields are zeroed
 * — we don't use the legacy hardware-task-switching mechanism that
 * would read them.
 *
 * Reference: Intel SDM Vol 3, §7.2 "Task Management Data Structures",
 * Figure 7-2 (32-bit TSS layout).
 * ============================================================================= */

#include "tss.h"
#include "gdt.h"
#include <stdint.h>

/* Full 32-bit TSS layout per the SDM.  `__attribute__((packed))` is
 * critical; the CPU reads each field at its exact offset. */
struct tss32 {
    uint32_t prev_link;     /* 0x00 */
    uint32_t esp0;          /* 0x04 — kernel stack pointer */
    uint32_t ss0;           /* 0x08 — kernel stack segment */
    uint32_t esp1;          /* 0x0C */
    uint32_t ss1;           /* 0x10 */
    uint32_t esp2;          /* 0x14 */
    uint32_t ss2;           /* 0x18 */
    uint32_t cr3;           /* 0x1C */
    uint32_t eip;           /* 0x20 */
    uint32_t eflags;        /* 0x24 */
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp;
    uint32_t esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;           /* 0x60 */
    uint16_t trap;          /* 0x64 */
    uint16_t iomap_base;    /* 0x66 */
} __attribute__((packed));

static struct tss32 tss;

/* Dedicated stack used when CPU switches from ring 3 to ring 0.  Living
 * in .bss so it costs nothing in the kernel image but is zero-filled at
 * load time.  4 KiB is enough for a syscall handler frame. */
#define KSTACK_SIZE 4096
static uint8_t syscall_stack[KSTACK_SIZE] __attribute__((aligned(16)));

void tss_init(void) {
    /* Zero everything — we re-set the fields that matter below. */
    uint8_t* b = (uint8_t*)&tss;
    for (uint32_t i = 0; i < sizeof(tss); i++) b[i] = 0;

    tss.ss0  = GDT_KERNEL_DS;
    tss.esp0 = (uint32_t)(uintptr_t)(syscall_stack + KSTACK_SIZE);

    /* iomap_base set to size-of-TSS effectively means "no I/O bitmap"
     * — every IN/OUT in ring 3 traps with #GP unless EFLAGS.IOPL grants
     * access (it doesn't, by default). */
    tss.iomap_base = sizeof(tss);
}

void tss_set_kernel_stack(uintptr_t esp) {
    tss.esp0 = (uint32_t)esp;
}

uintptr_t tss_get_addr(void)  { return (uintptr_t)&tss; }
uint32_t  tss_get_limit(void) { return sizeof(tss) - 1; }
