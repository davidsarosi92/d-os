/* =============================================================================
 * linker-aarch64.ld — kernel link script for the ARM64 (AArch64) port (M21).
 *
 * Unlike the x86 ports (which are loaded by GRUB at 1 MiB physical after a
 * multiboot header), the AArch64 image is handed straight to QEMU's `-M virt`
 * loader via `-kernel`.  That loader:
 *   - reads the ELF program headers and copies each PT_LOAD segment to its
 *     physical address,
 *   - jumps to the ELF entry point (`_start`) with the MMU OFF, caches off,
 *     and (on default `virt`) at EL1.
 *
 * The `virt` machine places RAM at physical 0x40000000.  We link the kernel
 * a little above that base (0x40080000) to leave the low 512 KiB for QEMU's
 * own boot shim / device-tree blob, matching the de-facto convention for
 * hand-written AArch64 kernels on this board.
 *
 * There is NO multiboot section here — boot is via the raw ELF entry.  The
 * driver / module / GUI registry KEEP() blocks are identical in spirit to the
 * x86 scripts (the registry mechanism is arch-agnostic); they are carried so
 * that a later M21 phase can link the full arch-independent core without
 * touching this file.
 *
 * Symbols exported for the boot assembler:
 *   __bss_start / __bss_end — zeroed by _start before the first C call.
 *   __stack_top             — top of the initial kernel stack (grows down).
 * ============================================================================= */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

/* QEMU `virt` RAM base is 0x40000000; leave the low 512 KiB free. */
LOAD_ADDR = 0x40080000;

SECTIONS
{
    . = LOAD_ADDR;
    PROVIDE(kernel_start = .);

    /* .text.boot is force-placed first so the ELF entry (_start) is the very
     * first byte of the image — makes the entry address trivially the load
     * address, which some minimal loaders assume. */
    .text : ALIGN(4K)
    {
        KEEP(*(.text.boot))
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata)
        *(.rodata.*)
    }

    /* Registry sections — populated by MODULE()/DRIVER()/GUI_APP()/... .
     * 8-byte stride to match the 64-bit struct/pointer natural alignment. */
    .modules : ALIGN(8)
    {
        __start_modules = .;
        KEEP(*(modules))
        __stop_modules = .;
    }

    .drivers : ALIGN(8)
    {
        __start_drivers = .;
        KEEP(*(drivers))
        __stop_drivers = .;
    }

    .gui_apps : ALIGN(8)
    {
        __start_gui_apps = .;
        KEEP(*(gui_apps))
        __stop_gui_apps = .;
    }

    .desktop_shells : ALIGN(8)
    {
        __start_desktop_shells = .;
        KEEP(*(desktop_shells))
        __stop_desktop_shells = .;
    }

    .shell_providers : ALIGN(8)
    {
        __start_shell_providers = .;
        KEEP(*(shell_providers))
        __stop_shell_providers = .;
    }

    .data : ALIGN(4K)
    {
        *(.data)
        *(.data.*)
    }

    /* .bss — zeroed by the boot assembler.  The initial kernel stack lives
     * at the tail of BSS so it is inside the mapped image and zero-init'd. */
    .bss : ALIGN(4K)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)

        . = ALIGN(16);
        __stack_bottom = .;
        . += 0x10000;              /* 64 KiB initial kernel/idle stack */
        __stack_top = .;

        . = ALIGN(4K);
        __bss_end = .;
    }

    PROVIDE(kernel_end = .);
}
