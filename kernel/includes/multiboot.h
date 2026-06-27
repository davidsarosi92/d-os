/* =============================================================================
 * multiboot.h — Multiboot1 info structure layout + access helpers.
 *
 * On entry to kernel_main, GRUB (or any multiboot1-compliant loader) has
 * left a filled-in `struct mboot_info` at the physical address `%ebx`
 * held on entry.  The relevant fields for us right now are:
 *
 *   flags          — bitmask indicating which other fields are valid.
 *                    Bit 0  → mem_lower / mem_upper present
 *                    Bit 6  → mmap_length / mmap_addr present
 *                    Bit 11 → VBE fields present
 *                    Bit 12 → framebuffer_* fields present
 *   mem_lower/_upper — legacy rough memory figures in KiB (640 KiB limit
 *                    on lower).  The real source of truth is the mmap.
 *   mmap_addr      — physical address of an array of mmap entries of
 *                    variable length; total bytes = mmap_length.  Each
 *                    entry starts with a `size` field telling you how far
 *                    to advance to the next one: `next = entry + size + 4`.
 *
 * Reference: Multiboot Specification v0.6.96 §3.3 "Boot information
 * format".
 * ============================================================================= */

#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

/* Magic GRUB (or any multiboot1 loader) puts in %eax on entry. */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/* Flag bits we care about. */
#define MBI_FLAG_MEM      (1u << 0)
#define MBI_FLAG_MMAP     (1u << 6)
#define MBI_FLAG_VBE      (1u << 11)
#define MBI_FLAG_FB       (1u << 12)

/* Memory-map entry types (Multiboot §3.3 "Memory map"). */
#define MMAP_TYPE_AVAILABLE   1
#define MMAP_TYPE_RESERVED    2
#define MMAP_TYPE_ACPI_RECLAIM 3
#define MMAP_TYPE_ACPI_NVS    4
#define MMAP_TYPE_BAD         5

/* The info header.  Laid out exactly as the spec defines it; unused fields
 * are kept as uint32_t so offsets match. */
struct mboot_info {
    uint32_t flags;
    uint32_t mem_lower;          /* KiB, only valid if MBI_FLAG_MEM */
    uint32_t mem_upper;          /* KiB, only valid if MBI_FLAG_MEM */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;        /* bytes, only valid if MBI_FLAG_MMAP */
    uint32_t mmap_addr;          /* physical addr, only valid if MBI_FLAG_MMAP */
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6];
} __attribute__((packed));

/* Memory-map entry.  `size` does not include itself (advance by size + 4). */
struct mboot_mmap_entry {
    uint32_t size;
    uint64_t base;
    uint64_t length;
    uint32_t type;
} __attribute__((packed));

/* Validate and cache the info pointer for later queries.  Returns 0 on
 * success, -1 if the magic is wrong or the pointer is obviously bogus. */
int mboot_init(uint32_t magic, uint32_t info_ptr);

/* Return the cached info pointer so other subsystems (PMM, VBE) can walk
 * the same data without re-validating.  NULL if `mboot_init` failed. */
const struct mboot_info* mboot_get_info(void);

/* Dump legacy mem_lower/mem_upper + every memory-map entry to the console.
 * Used by the `meminfo` shell command. */
void mboot_print_meminfo(void);

#endif
