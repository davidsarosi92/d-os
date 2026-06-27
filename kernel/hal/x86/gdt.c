/* =============================================================================
 * gdt.c — build and load our own Global Descriptor Table.
 *
 * On entry, GRUB gives us a flat GDT of its own making.  That works, but we
 * don't control what's in it, we can't extend it (no user segments, no
 * TSS), and a theoretical bug in GRUB's GDT could bite us later.  So the
 * first real act of arch setup in d-os is to replace GRUB's GDT with ours.
 *
 * -------------------- GDT entry format (Intel SDM Vol 3, §3.4.5) ------------
 *
 *   byte 0:  limit[7:0]
 *   byte 1:  limit[15:8]
 *   byte 2:  base[7:0]
 *   byte 3:  base[15:8]
 *   byte 4:  base[23:16]
 *   byte 5:  access byte       = | P | DPL(2) | S | Type(4) |
 *   byte 6:  flags + limit     = | G | D/B | L | AVL | limit[19:16] |
 *   byte 7:  base[31:24]
 *
 * For a flat kernel segment we want:
 *   base   = 0
 *   limit  = 0xFFFFF   (with G=1, that's 0xFFFFF * 4 KiB = 4 GiB)
 *   access = 0x9A  (code)    or  0x92  (data)
 *   flags  = 0xC   (G=1, D/B=1 for 32-bit, L=0, AVL=0)
 *
 * --------------------- Access byte decoding --------------------------------
 *   P   = 1 — segment is present
 *   DPL = 0 — ring 0 (kernel)
 *   S   = 1 — this is a code/data descriptor, not a system descriptor
 *   Type — 4 bits depending on whether it's code or data:
 *     code: 1010  = executable, readable, non-conforming
 *     data: 0010  = read+write, expand-up
 *
 *   0x9A = 1001 1010  — kernel code (P=1, DPL=0, S=1, Type=1010)
 *   0x92 = 1001 0010  — kernel data (P=1, DPL=0, S=1, Type=0010)
 *
 * ------------------- Reloading segment registers ---------------------------
 *
 * `lgdt` loads the GDT register with our table pointer but does NOT update
 * the shadow descriptors inside CS/DS/ES/FS/GS/SS — those still reference
 * GRUB's GDT entries.  To force a refresh we:
 *
 *   - load DS/ES/FS/GS/SS with the new data selector (plain `mov`)
 *   - far-jump to a next-instruction label with the new code selector,
 *     which reloads CS as a side effect
 *
 * After that our new descriptors are in effect everywhere and GRUB's old
 * GDT is out of the picture.
 * =========================================================================== */

#include "gdt.h"
#include "printf.h"
#include "tss.h"
#include <stdint.h>

/* One GDT entry, packed because the CPU reads it byte-for-byte. */
struct gdt_entry {
    uint16_t limit_low;             /* limit[15:0] */
    uint16_t base_low;              /* base[15:0] */
    uint8_t  base_mid;              /* base[23:16] */
    uint8_t  access;                /* P | DPL | S | Type */
    uint8_t  flags_limit_high;      /* flags[3:0] << 4 | limit[19:16] */
    uint8_t  base_high;             /* base[31:24] */
} __attribute__((packed));

/* The 6-byte pointer structure `lgdt` reads:  limit first, then base. */
struct gdt_ptr {
    uint16_t limit;                 /* size of the GDT in bytes, minus 1 */
    uint32_t base;                  /* linear address of the GDT */
} __attribute__((packed));

/* 6 entries: null, kernel CS, kernel DS, user CS, user DS, TSS */
#define GDT_ENTRIES 6
static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gdtr;

/* Fill in one entry.  `flags` is the 4-bit nibble that goes into the high
 * half of byte 6; `access` is the full 8-bit access byte. */
static void set_entry(int i, uint32_t base, uint32_t limit,
                      uint8_t access, uint8_t flags) {
    gdt[i].limit_low        = (uint16_t)(limit & 0xFFFF);
    gdt[i].base_low         = (uint16_t)(base  & 0xFFFF);
    gdt[i].base_mid         = (uint8_t) ((base  >> 16) & 0xFF);
    gdt[i].access           = access;
    gdt[i].flags_limit_high = (uint8_t)(((flags & 0xF) << 4) | ((limit >> 16) & 0xF));
    gdt[i].base_high        = (uint8_t) ((base  >> 24) & 0xFF);
}

/* Load the GDT and reload every segment register.  After this returns,
 * CS = GDT_KERNEL_CS and DS/ES/FS/GS/SS = GDT_KERNEL_DS.
 *
 * The `memory` clobber tells the compiler not to move memory accesses
 * across the barrier — important because the act of reloading segments
 * could otherwise be reordered with surrounding loads/stores. */
static void gdt_load(void) {
    __asm__ volatile (
        "lgdt %0\n\t"
        "mov %1, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "ljmp %2, $1f\n\t"          /* far jump reloads CS */
        "1:"
        :
        : "m"(gdtr),
          "i"(GDT_KERNEL_DS),
          "i"(GDT_KERNEL_CS)
        : "ax", "memory"
    );
}

void gdt_init(void) {
    /* Entry 0: mandatory null descriptor.  Loading a non-null selector into
     * a segment register while entry 0 is not all zeros is a CPU fault,
     * so we zero-fill it explicitly. */
    set_entry(0, 0, 0, 0, 0);

    /* Entry 1: kernel code — flat 4 GiB, ring 0, readable + executable. */
    set_entry(1, 0, 0xFFFFF, 0x9A, 0xC);

    /* Entry 2: kernel data — flat 4 GiB, ring 0, read + write. */
    set_entry(2, 0, 0xFFFFF, 0x92, 0xC);

    /* Entry 3: user code — flat 4 GiB, ring 3, readable + executable.
     * Access byte: P=1, DPL=3, S=1, Type=1010 → 0xFA. */
    set_entry(3, 0, 0xFFFFF, 0xFA, 0xC);

    /* Entry 4: user data — flat 4 GiB, ring 3, read + write.
     * Access byte: P=1, DPL=3, S=1, Type=0010 → 0xF2. */
    set_entry(4, 0, 0xFFFFF, 0xF2, 0xC);

    /* Entry 5: TSS descriptor — base + limit point at our static TSS in
     * tss.c.  Access type 0x9 = "32-bit available TSS" with S=0 (system).
     * Full access byte: P=1, DPL=0, S=0, Type=1001 → 0x89.  Flags 0x0
     * (no granularity, single TSS).  `tss_get_addr()` returns the
     * struct's linear address; `tss_get_limit` returns sizeof(tss)-1. */
    set_entry(5, tss_get_addr(), tss_get_limit(), 0x89, 0x0);

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint32_t)(uintptr_t)&gdt[0];

    gdt_load();

    /* Load TR (task register) so the CPU starts using our TSS for ring
     * transitions.  Must come AFTER lgdt because TR loading reads the
     * GDT to validate the descriptor. */
    __asm__ volatile ("ltr %%ax" : : "a"((uint16_t)GDT_TSS));

    kprintf("GDT: %d entries @ %p (incl. user CS/DS + TSS)\n",
            GDT_ENTRIES, (void*)gdtr.base);
}
