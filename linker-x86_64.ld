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

    /* Driver registry — sibling of .modules, populated by DRIVER().
     * Alignment is 8 to match the natural pointer/struct stride on
     * 64-bit (vs. 4 on i386). */
    .drivers : ALIGN(8)
    {
        __start_drivers = .;
        KEEP(*(drivers))
        __stop_drivers = .;
    }

    /* GUI app + desktop shell registries (M22.2) — same KEEP()
     * discipline; alignment 8 for the 64-bit struct stride. */
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

    /* Command-shell provider registry (S.1) — SHELL_PROVIDER() entries;
     * spawn sites resolve the active one via the shell.provider key. */
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
     * by the inline asm in kernel/hal/<arch>/uaccess.c; the page-fault handler
     * looks the faulting kernel EIP up here and resumes at the fixup instead of
     * panicking, so a user pointer that goes bad DURING a copy returns -EFAULT
     * rather than freezing the box.  KEEP() — nothing references the entries by
     * name, they are found only through these bounds. */
    .ex_table : ALIGN(8)
    {
        __start_ex_table = .;
        KEEP(*(ex_table))
        __stop_ex_table = .;
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
