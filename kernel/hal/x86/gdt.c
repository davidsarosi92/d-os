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
#include "acpi.h"          /* ACPI_MAX_CPUS */
#include "percpu.h"        /* this_cpu_id  */
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

/* Fixed entries: null, kernel CS, kernel DS, user CS, user DS (0..4), then one
 * TSS descriptor PER CPU at GDT_TSS_BASE.. (SMP — each CPU loads TR with its
 * own; selector for CPU c = (GDT_TSS_BASE + c) << 3), then one user-TLS
 * descriptor PER CPU at GDT_TLS_BASE.. (M35 — a ring-3 %gs segment whose base
 * the scheduler swaps to the running thread's TLS pointer; selector for CPU c
 * = (GDT_TLS_BASE + c) << 3 | 3). */
#define GDT_TSS_BASE  5
#define GDT_TLS_BASE  (GDT_TSS_BASE + ACPI_MAX_CPUS)
#define GDT_ENTRIES   (GDT_TLS_BASE + ACPI_MAX_CPUS)
static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gdtr;

/* AP-boot helper (M18): expose the GDTR pointer so the trampoline
 * can lgdt the same table the BSP uses.  Returns a pointer to the
 * static gdtr struct in `.bss`; caller treats it as opaque, just
 * memcpy's it into the trampoline's data area. */
void* gdt_get_ptr_struct(void) { return &gdtr; }

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

    /* Entries GDT_TSS_BASE..: one 32-bit-available-TSS descriptor per CPU
     * (access 0x89 = P=1, DPL=0, S=0, Type=1001; flags 0x0).  Each points at
     * that CPU's own TSS in tss.c so a ring-3 → ring-0 trap on any core lands
     * on its own kernel stack. */
    for (int c = 0; c < tss_max_cpus(); c++)
        set_entry(GDT_TSS_BASE + c, (uint32_t)tss_get_addr_cpu(c),
                  tss_get_limit(), 0x89, 0x0);

    /* Per-CPU user-TLS descriptors: a flat 4 GiB ring-3 data segment
     * (access 0xF2 = P=1, DPL=3, S=1, Type=0010, same as user DS) whose base
     * starts at 0 and is rewritten per-thread by hal_set_tls_base. */
    for (int c = 0; c < tss_max_cpus(); c++)
        set_entry(GDT_TLS_BASE + c, 0, 0xFFFFF, 0xF2, 0xC);

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint32_t)(uintptr_t)&gdt[0];

    gdt_load();

    /* Load TR for the running CPU (the BSP, logical 0 at this point).  Must
     * come AFTER lgdt because TR loading reads the GDT to validate. */
    gdt_load_cpu_tss();

    kprintf("GDT: %d entries @ %p (%d per-CPU TSS)\n",
            GDT_ENTRIES, (void*)gdtr.base, tss_max_cpus());
}

/* Load this CPU's task register with its own per-CPU TSS descriptor.  Called
 * by the BSP from gdt_init and by each AP from ap_main (after percpu is up so
 * this_cpu_id() is valid).  The GDT itself is shared, so the AP just needs to
 * point TR at its slot. */
void gdt_load_cpu_tss(void) {
    uint16_t sel = (uint16_t)((GDT_TSS_BASE + this_cpu_id()) << 3);
    __asm__ volatile ("ltr %0" : : "r"(sel));
}

/* M35 TLS — the ring-3 selector a thread on THIS CPU loads into %gs to reach
 * its thread-local block (RPL 3).  Returned by sys_set_tls. */
uint16_t gdt_tls_selector(void) {
    return (uint16_t)(((GDT_TLS_BASE + this_cpu_id()) << 3) | 3);
}

/* M35 TLS — rewrite THIS CPU's user-TLS descriptor base.  The CPU caches a
 * descriptor in the hidden part of %gs, so the new base takes effect only when
 * %gs is next (re)loaded — which happens naturally on the return-to-ring-3 path
 * (isr_common pops %gs before iret).  Called by the scheduler on switch-in and
 * by sys_set_tls. */
void hal_set_tls_base(uintptr_t base) {
    set_entry(GDT_TLS_BASE + this_cpu_id(), (uint32_t)base, 0xFFFFF, 0xF2, 0xC);
}
