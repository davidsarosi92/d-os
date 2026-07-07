/* =============================================================================
 * stubs.c — AArch64 glue for the arch-independent core (M21 Phase C).
 *
 * The core scheduler + memory manager are written against a few x86-shaped
 * services that are meaningless (or not yet ported) on ARM.  Rather than
 * #ifdef the core, we satisfy the symbols here with UP-correct stubs:
 *
 *   - **SMP / APIC topology.**  aarch64 is single-CPU for now, so `lapic_id`
 *     is 0, `smp_*` are no-ops, and the ACPI MADT getters return a 1-CPU
 *     answer.  (percpu.c stays in "not-ready" mode — this_cpu_id() short-
 *     circuits to 0 — because we never call percpu_init_bsp, so these are
 *     link-time-only symbols in practice.)
 *
 *   - **Boot memory map.**  There is no multiboot info on ARM; QEMU hands us
 *     a device tree instead (parsing it is a later phase).  We synthesise the
 *     `struct mboot_info` + one AVAILABLE mmap entry describing the `virt`
 *     board's RAM so the stock pmm.c mmap walk works unchanged.
 *
 *   - **Serial console sink.**  Registers the PL011 UART as a console sink so
 *     kprintf() output reaches the serial log (the ARM analogue of the COM1
 *     serial sink).
 *
 * All of these have stable prototypes in the shared headers; keeping the
 * definitions in one arch file means the core never learns ARM exists.
 * ============================================================================= */

#include "multiboot.h"
#include "console.h"
#include <stdint.h>

/* ---- synthetic boot memory map --------------------------------------------- */

/* QEMU `virt` RAM base + the size we tell the PMM to manage.  Keep it at or
 * below the `-m` value the run script passes (256 MiB).  RAM physically spans
 * [0x4000_0000, 0x4000_0000 + size); the kernel image (linked at 0x4008_0000)
 * inside it is carved out by pmm.c using the kernel_start/kernel_end linker
 * symbols. */
#define VIRT_RAM_BASE   0x40000000ULL
#define VIRT_RAM_SIZE   (256ULL * 1024 * 1024)   /* fallback if no DTB          */

/* MULTIBOOT_BOOTLOADER_MAGIC (multiboot1) — mboot_init validates against it. */
#define MB1_MAGIC       0x2BADB002u

/* Machine facts discovered from the device tree (dtb.c); 0 if no DTB. */
uint64_t dtb_ram_base(void);
uint64_t dtb_ram_size(void);

static struct mboot_mmap_entry aarch64_mmap[1];
static struct mboot_info       aarch64_mbi;

/* Build the fake multiboot info + register it so mboot_get_info() returns a
 * usable RAM map to pmm_init().  Call once, before pmm_init() (and after
 * dtb_init(), so the DTB-discovered RAM size is used when available). */
void aarch64_boot_meminfo_init(void) {
    /* Prefer the device-tree-discovered RAM window; fall back to the built-in
     * `virt` defaults if no DTB was found. */
    uint64_t base = dtb_ram_base() ? dtb_ram_base() : VIRT_RAM_BASE;
    uint64_t size = dtb_ram_size() ? dtb_ram_size() : VIRT_RAM_SIZE;

    aarch64_mmap[0].size   = sizeof(struct mboot_mmap_entry) - 4;  /* excl. size */
    aarch64_mmap[0].base   = base;
    aarch64_mmap[0].length = size;
    aarch64_mmap[0].type   = MMAP_TYPE_AVAILABLE;

    aarch64_mbi.flags       = MBI_FLAG_MMAP;
    aarch64_mbi.mmap_length = sizeof(aarch64_mmap);
    aarch64_mbi.mmap_addr   = (uint32_t)(uintptr_t)aarch64_mmap;

    mboot_init(MB1_MAGIC, (uintptr_t)&aarch64_mbi);
}

/* ---- PL011 serial console sink --------------------------------------------- */

void uart_early_putc(char c);   /* uart.c */

static void serial_sink_putchar(char c) {
    uart_early_putc(c);
}

static struct console_sink pl011_sink = {
    .name     = "pl011",
    .category = "serial",
    .putchar  = serial_sink_putchar,
    .clear    = 0,
    .active   = 1,
    .next     = 0,
};

/* Register the PL011 as a console sink so kprintf() reaches the serial log. */
void aarch64_serial_console_init(void) {
    console_sink_register(&pl011_sink);
}
