/* =============================================================================
 * gdt.c — build and load the 64-bit Global Descriptor Table.
 *
 * boot.s already loaded a minimal GDT (null + code64 + data) just to
 * get us into long mode.  Here we replace it with a fuller one that
 * adds user-mode code/data (DPL=3) and a TSS descriptor — same shape
 * as the i386 GDT but with the long-mode-specific encoding tweaks:
 *
 *   1. Long-mode code descriptors set L=1 (bit 53) and clear D=0
 *      (bit 54).  Setting both is invalid.
 *   2. The TSS descriptor in a long-mode GDT is 16 bytes (not 8) so
 *      it can carry a full 64-bit base address.  It occupies two
 *      consecutive GDT entry slots.  The first 8 bytes use the
 *      familiar SDM format (with type 0x9 = "available 64-bit TSS");
 *      the second 8 bytes hold the high 32 bits of base + a few
 *      reserved fields.
 *   3. base/limit are largely ignored for code/data segments in long
 *      mode (the CPU treats them as flat 0..2^64).  We set the
 *      i386-style limit=0xFFFFF + G=1 anyway because that's what
 *      every other long-mode OS does and it'd be confusing not to.
 *
 * Reference: AMD64 APM Vol 2 §4.8 (Long-Mode Segment Descriptors).
 * ============================================================================= */

#include "gdt.h"
#include "printf.h"
#include "tss.h"
#include <stdint.h>

/* Standard 8-byte descriptor (null, code, data, user code, user data).
 * Field layout is identical to i386 in the encoding sense; the
 * interpretation just differs (long-mode CPU ignores base/limit for
 * code/data but checks L+D for code). */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_high;
    uint8_t  base_high;
} __attribute__((packed));

/* Long-mode TSS descriptor: 16 bytes spanning two GDT slots.  The low
 * 8 bytes mirror the regular descriptor format (with type=0x9, S=0);
 * the high 8 bytes hold the upper 32 bits of base + reserved zeros. */
struct gdt_tss_entry {
    /* low half — looks like a standard descriptor */
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;             /* 0x89 = P=1, DPL=0, S=0, type=0x9 (avail TSS) */
    uint8_t  flags_limit_high;
    uint8_t  base_high;          /* base bits 24..31 */
    /* high half — upper base + reserved */
    uint32_t base_upper;         /* base bits 32..63 */
    uint32_t reserved;           /* must be zero */
} __attribute__((packed));

/* GDTR pointer for 64-bit lgdt: 2-byte limit + 8-byte base.  Note the
 * difference from i386's 6-byte form. */
struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Slot layout:
 *   0   null
 *   1   kernel code64           (selector 0x08)
 *   2   kernel data             (selector 0x10)
 *   3   user code64             (selector 0x18 | 3 = 0x1B)
 *   4   user data               (selector 0x20 | 3 = 0x23)
 *   5   TSS descriptor lo half  (selector 0x28)
 *   6   TSS descriptor hi half  (consumed by the CPU as part of slot 5)
 */
#define GDT_ENTRIES 7
static union {
    struct gdt_entry e[GDT_ENTRIES];
    uint8_t          raw[GDT_ENTRIES * 8];
} gdt;
static struct gdt_ptr gdtr;

void* gdt_get_ptr_struct(void) { return &gdtr; }

static void set_entry(int i, uint32_t base, uint32_t limit,
                      uint8_t access, uint8_t flags) {
    gdt.e[i].limit_low        = (uint16_t)(limit & 0xFFFF);
    gdt.e[i].base_low         = (uint16_t)(base  & 0xFFFF);
    gdt.e[i].base_mid         = (uint8_t) ((base  >> 16) & 0xFF);
    gdt.e[i].access           = access;
    gdt.e[i].flags_limit_high = (uint8_t)(((flags & 0xF) << 4) | ((limit >> 16) & 0xF));
    gdt.e[i].base_high        = (uint8_t) ((base  >> 24) & 0xFF);
}

/* Install the 16-byte TSS descriptor at slots `i` and `i+1`. */
static void set_tss_entry(int i, uintptr_t base, uint32_t limit) {
    struct gdt_tss_entry* t = (struct gdt_tss_entry*)&gdt.raw[i * 8];
    t->limit_low        = (uint16_t)(limit & 0xFFFF);
    t->base_low         = (uint16_t)(base  & 0xFFFF);
    t->base_mid         = (uint8_t) ((base  >> 16) & 0xFF);
    t->access           = 0x89;                            /* P=1, DPL=0, S=0, type=0x9 */
    t->flags_limit_high = (uint8_t) ((limit >> 16) & 0xF); /* G=0 (single TSS) */
    t->base_high        = (uint8_t) ((base  >> 24) & 0xFF);
    t->base_upper       = (uint32_t)(base >> 32);
    t->reserved         = 0;
}

/* lgdt + reload data segment registers.  Note: we DO NOT do a far-jmp
 * to reload CS here, because boot.s already loaded the 64-bit code
 * descriptor we want and the new descriptor at slot 1 is encoded
 * identically.  If a future change moved the kernel code descriptor
 * to a different slot, a far-jmp via `pushq $sel; pushq $1f; lretq`
 * would be needed. */
static void gdt_load(void) {
    __asm__ volatile (
        "lgdt %0\n\t"
        "movw %1, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        :
        : "m"(gdtr),
          "i"(GDT_KERNEL_DS)
        : "ax", "memory"
    );
}

void gdt_init(void) {
    /* Entry 0: null. */
    set_entry(0, 0, 0, 0, 0);

    /* Entry 1: kernel code64.
     * Access: 0x9A = P=1, DPL=0, S=1, type=0xA (executable, readable).
     * Flags:  0xA  = G=1, D=0, L=1 (long-mode code).  D MUST be 0 with L=1. */
    set_entry(1, 0, 0xFFFFF, 0x9A, 0xA);

    /* Entry 2: kernel data.
     * Access: 0x92 = P=1, DPL=0, S=1, type=0x2 (read+write).
     * Flags:  0xC  = G=1, D/B=1 (data segments still honor D in long mode for
     *               stack ops).  Long mode mostly ignores data segs anyway. */
    set_entry(2, 0, 0xFFFFF, 0x92, 0xC);

    /* Entry 3: user code64.  Access 0xFA = same as kernel code but DPL=3.
     * Flags 0xA = G=1, L=1, D=0 (same constraints as kernel code64). */
    set_entry(3, 0, 0xFFFFF, 0xFA, 0xA);

    /* Entry 4: user data.  Access 0xF2 = same as kernel data but DPL=3. */
    set_entry(4, 0, 0xFFFFF, 0xF2, 0xC);

    /* Entries 5+6: 16-byte TSS descriptor. */
    set_tss_entry(5, tss_get_addr(), tss_get_limit());

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)(uintptr_t)&gdt.raw[0];

    gdt_load();

    /* Load TR with the TSS selector so the CPU starts using our TSS
     * for ring transitions.  Selector value GDT_TSS (0x28) is the
     * same as i386 by coincidence — both put the TSS in slot 5. */
    __asm__ volatile ("ltr %%ax" : : "a"((uint16_t)GDT_TSS));

    kprintf("GDT: %d entries @ %p (incl. user CS/DS + 64-bit TSS)\n",
            GDT_ENTRIES, (void*)gdtr.base);
}
