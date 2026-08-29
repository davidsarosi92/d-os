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
    /* §M67 — kernel symbol table.  EXPORT_SYMBOL() entries: the names a
     * LOADABLE MODULE is allowed to resolve against.  KEEP() for the usual
     * reason (nothing references these by name), and note that unlike every
     * other registry here this one is a PUBLISHED CONTRACT — an entry that
     * disappears breaks a module built against it, so removing a line from
     * ksym.c is a breaking change rather than a cleanup. */
    .ksyms : ALIGN(8)
    {
        __start_ksyms = .;
        KEEP(*(ksyms))
        __stop_ksyms = .;
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

    /* Config watch registry (§M63 stage 0) — CONFIG_WATCH() entries.  A
     * subsystem that read a key at boot registers here so a later change
     * (the persistent store being overlaid once the disk mounts, or a
     * settings panel writing the key) reaches it instead of taking effect
     * only at the NEXT boot. */
    .config_watches : ALIGN(8)
    {
        __start_config_watches = .;
        KEEP(*(config_watches))
        __stop_config_watches = .;
    }

    /* Item-view registry (§M63/§M64) — ITEM_VIEW() layouts (grid, list, …).
     * The desktop and the control panel pick one BY NAME from config, so a
     * new layout is a new file rather than an edit to either consumer. */
    .item_views : ALIGN(8)
    {
        __start_item_views = .;
        KEEP(*(item_views))
        __stop_item_views = .;
    }

    /* Control-panel registries (§M63): SETTINGS_PANEL() pages and
     * CONFIG_KEY() descriptors.  A setting ships next to the code it
     * configures; controlpanel.c names none of them. */
    .settings_panels : ALIGN(8)
    {
        __start_settings_panels = .;
        KEEP(*(settings_panels))
        __stop_settings_panels = .;
    }

    /* §M65 — widget-class registry: WIDGET_CLASS() entries, looked up BY
       NAME so a class can be named in data (and later across a boundary). */
    .ui_classes : ALIGN(8)
    {
        __start_ui_classes = .;
        KEEP(*(ui_classes))
        __stop_ui_classes = .;
    }

    .config_keys : ALIGN(8)
    {
        __start_config_keys = .;
        KEEP(*(config_keys))
        __stop_config_keys = .;
    }

    /* Which PCI IDs a driver claims (hwdev.h) — declared NEXT TO THE DRIVER so
       the device manager never accumulates knowledge of the whole tree. */
    .driver_matches : ALIGN(8)
    {
        __start_driver_matches = .;
        KEEP(*(driver_matches))
        __stop_driver_matches = .;
    }

    .shell_providers : ALIGN(8)
    {
        __start_shell_providers = .;
        KEEP(*(shell_providers))
        __stop_shell_providers = .;
    }

    /* Service registry + service-bus provider/adapter registries (M29). */
    .services : ALIGN(8)
    {
        __start_services = .;
        KEEP(*(services))
        __stop_services = .;
    }

    /* Crash-sink registry (§M47) — CRASH_SINK() drops a `struct crash_sink`
     * here; crash_drain() walks it to deliver each record.  Same KEEP() +
     * ALIGN(4) discipline as the driver/service registries: KEEP is required
     * because nothing REFERENCES these objects by name, so --gc-sections would
     * otherwise drop every crash reporter in the build. */
    .crashsinks : ALIGN(4)
    {
        __start_crashsinks = .;
        KEEP(*(crashsinks))
        __stop_crashsinks = .;
    }
    .bus_providers : ALIGN(8)
    {
        __start_bus_providers = .;
        KEEP(*(bus_providers))
        __stop_bus_providers = .;
    }
    .bus_adapters : ALIGN(8)
    {
        __start_bus_adapters = .;
        KEEP(*(bus_adapters))
        __stop_bus_adapters = .;
    }

    /* Cron job registry (M30) — CRON_JOB() entries; the cron service walks it. */
    .cron_jobs : ALIGN(8)
    {
        __start_cron_jobs = .;
        KEEP(*(cron_jobs))
        __stop_cron_jobs = .;
    }

    /* §1.1 — user-access exception table.  Each entry (insn, fixup) is emitted
     * by the inline asm in kernel/hal/aarch64/uaccess.c; the EL1 abort handler
     * looks the faulting ELR up here and resumes at the fixup instead of
     * halting, so an EL0 pointer that goes bad DURING a copy returns -EFAULT. */
    .ex_table : ALIGN(8)
    {
        __start_ex_table = .;
        KEEP(*(ex_table))
        __stop_ex_table = .;
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
