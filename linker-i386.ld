ENTRY(_start)

/* These symbols let C code discover where the kernel image lives in
 * physical memory — used by the PMM to mark the kernel frames as in-use
 * when building its free-frame bitmap. */
SECTIONS
{
    . = 1M;
    PROVIDE(kernel_start = .);

    .multiboot BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
    }

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }

    /* Module registry section.  Every MODULE() macro instantiation drops a
     * `struct module_def` here; the kernel walks [__start_modules,
     * __stop_modules) at boot to call each module's init.  KEEP() is
     * required so ld doesn't garbage-collect entries that aren't
     * referenced by name from main. */
    .modules : ALIGN(4)
    {
        __start_modules = .;
        KEEP(*(modules))
        __stop_modules = .;
    }

    /* Driver registry — sibling of .modules, populated by DRIVER() in
     * kernel/includes/driver.h.  Same KEEP() + ALIGN(4) discipline as
     * above.  Note alignment must equal sizeof(struct driver) (16 on
     * i386) divided down to its GCD with 4 — `aligned(4)` on the entries
     * matches the natural pointer-array stride. */
    .drivers : ALIGN(4)
    {
        __start_drivers = .;
        KEEP(*(drivers))
        __stop_drivers = .;
    }

    /* GUI app registry (M22.2) — GUI_APP() drops a `struct gui_app_def`
     * here; the Start menu + the `launch` shell command walk it.  Same
     * KEEP() + alignment discipline as .modules. */
    .gui_apps : ALIGN(4)
    {
        __start_gui_apps = .;
        KEEP(*(gui_apps))
        __stop_gui_apps = .;
    }

    /* Desktop shell registry (M22.2) — DESKTOP_SHELL() instances; the
     * compositor picks the active one by the `gui.shell` config key. */
    .desktop_shells : ALIGN(4)
    {
        __start_desktop_shells = .;
        KEEP(*(desktop_shells))
        __stop_desktop_shells = .;
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    PROVIDE(kernel_end = .);
}
