/* =============================================================================
 * linker-x86_64.ld — kernel link script for the x86_64 (long mode) port.
 *
 * Differs from linker-i386.ld in only two material ways:
 *   1. OUTPUT_FORMAT/ARCH say ELF64.
 *   2. Section alignment uses 4 KiB pages on both (no PSE pages here —
 *      the early page tables in boot.s map identity 4 KiB by default).
 *
 * Load address is still 1 MiB physical, identity-mapped during early
 * boot.  Higher-half kernel relocation (link at
 * 0xFFFFFFFF80000000) is deferred — M20 Phase 4+ once paging + mcmodel
 * switch is in place.
 *
 * The .modules / .drivers KEEP() pattern is identical to i386 — the
 * driver and module registry mechanism is arch-agnostic.
 * ============================================================================= */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 1M;
    PROVIDE(kernel_start = .);

    /* Multiboot2 header.  Must live within the first 32 KiB of the
     * final image per the MB2 spec (vs. MB1's 8 KiB) — the section
     * placement here at the very start guarantees that. */
    .multiboot BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
    }

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.text)
        *(.text.*)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
        *(.rodata.*)
    }

    /* Module registry — every MODULE() macro instantiation drops a
     * `struct module_def` here.  Same KEEP() discipline as i386. */
    .modules : ALIGN(8)
    {
        __start_modules = .;
        KEEP(*(modules))
        __stop_modules = .;
    }

    /* Driver registry — sibling of .modules, populated by DRIVER().
     * Alignment is 8 to match the natural pointer/struct stride on
     * 64-bit (vs. 4 on i386). */
    .drivers : ALIGN(8)
    {
        __start_drivers = .;
        KEEP(*(drivers))
        __stop_drivers = .;
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
        *(.data.*)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    PROVIDE(kernel_end = .);
}
