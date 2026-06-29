/* =============================================================================
 * mb2.c — Multiboot2 → Multiboot1 info-struct shim (x86_64).
 *
 * d-os was originally built around multiboot1 (kernel/core/multiboot.c
 * + kernel/includes/multiboot.h).  multiboot1 cannot directly boot a
 * 64-bit ELF, so for the x86_64 port boot.s declares a multiboot2
 * header instead — but at C-entry time we still want the rest of the
 * kernel (pmm.c, fb_terminal.c, mboot_print_meminfo, ...) to consume
 * the familiar `struct mboot_info` flat layout.
 *
 * This file walks the mb2 tag stream and synthesises an equivalent
 * mb1-compatible `struct mboot_info` from it.  The synth struct lives
 * in .bss and is returned by pointer for the caller (boot path) to
 * hand into mboot_init() with magic=MULTIBOOT_BOOTLOADER_MAGIC.
 *
 * Tags we translate today:
 *   - type 4  (basic mem info)   → mem_lower / mem_upper
 *   - type 6  (memory map)       → mmap_addr / mmap_length
 *   - type 8  (framebuffer info) → framebuffer_*
 * Unsupported tags are skipped silently — that's fine, the consumers
 * gracefully degrade when the relevant flags bit is clear.
 *
 * Memory-map shape: mb2's entries are 24 bytes each (base + length +
 * type + reserved), while mb1's `mboot_mmap_entry` has a 4-byte size
 * prefix.  We allocate a side buffer in .bss and copy/repack the
 * entries so the mb1 `mmap_addr` points at the familiar prefixed
 * layout.
 *
 * Reference: GNU Multiboot2 spec, §3.5 (Boot Information Format).
 * ============================================================================= */

#include "multiboot.h"
#include "printf.h"
#include <stdint.h>

/* Multiboot2 magic.  Different from the mb1 magic 0x2BADB002. */
#define MB2_LOADER_MAGIC 0x36d76289u

/* Tag types from the spec. */
#define MB2_TAG_END         0
#define MB2_TAG_CMDLINE     1
#define MB2_TAG_BOOTLOADER  2
#define MB2_TAG_MODULE      3
#define MB2_TAG_BASIC_MEM   4
#define MB2_TAG_MMAP        6
#define MB2_TAG_FRAMEBUFFER 8

struct mb2_header_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct mb2_basic_mem {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;
    uint32_t mem_upper;
} __attribute__((packed));

struct mb2_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed));

struct mb2_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb2_mmap_entry entries[];
} __attribute__((packed));

struct mb2_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;
    uint16_t reserved;
    uint8_t  color_info[6];
} __attribute__((packed));

/* Synthesised mb1 info struct + mmap-entry repacking buffer.  Sized
 * for up to 32 mmap entries (QEMU typically reports 4-6 for our
 * memory config). */
#define MAX_MMAP_ENTRIES 32

static struct mboot_info       synth_info;
static struct mboot_mmap_entry synth_mmap[MAX_MMAP_ENTRIES];

/* Translate a multiboot2 info structure to mb1 shape.
 *
 *   mb2_info  — physical address GRUB left in EBX on entry (we
 *               grabbed it into r13 in boot.s, then pass here).
 *   Returns   — pointer to the synth mb1 struct (NULL on failure).
 *
 * Safe to call from very early boot; uses only .bss and printf. */
uintptr_t mb2_translate_to_mb1(uintptr_t mb2_info) {
    if (!mb2_info) return 0;

    /* mb2 info starts with: uint32_t total_size, uint32_t reserved,
     * then tags.  Each tag is 8-byte aligned. */
    const uint8_t* base = (const uint8_t*)mb2_info;
    uint32_t total_size = *(const uint32_t*)base;
    if (total_size < 8 || total_size > (1u << 20)) return 0;     /* sanity */

    /* Reset synth state. */
    for (uint32_t i = 0; i < sizeof(synth_info); i++) ((uint8_t*)&synth_info)[i] = 0;
    int synth_mmap_n = 0;

    /* Walk tags starting at offset 8 (skip total_size + reserved). */
    const uint8_t* p   = base + 8;
    const uint8_t* end = base + total_size;
    while (p < end) {
        const struct mb2_header_tag* t = (const struct mb2_header_tag*)p;
        if (t->type == MB2_TAG_END) break;
        if (t->size < 8) break;                                  /* malformed */

        switch (t->type) {
        case MB2_TAG_BASIC_MEM: {
            const struct mb2_basic_mem* m = (const struct mb2_basic_mem*)p;
            synth_info.mem_lower = m->mem_lower;
            synth_info.mem_upper = m->mem_upper;
            synth_info.flags    |= MBI_FLAG_MEM;
            break;
        }
        case MB2_TAG_MMAP: {
            const struct mb2_mmap* mm = (const struct mb2_mmap*)p;
            if (mm->entry_size < sizeof(struct mb2_mmap_entry)) break;
            uint32_t nentries = (mm->size - 16) / mm->entry_size;
            if (nentries > MAX_MMAP_ENTRIES) nentries = MAX_MMAP_ENTRIES;
            for (uint32_t i = 0; i < nentries; i++) {
                const struct mb2_mmap_entry* e =
                    (const struct mb2_mmap_entry*)((const uint8_t*)mm->entries +
                                                   i * mm->entry_size);
                /* mb1 entry layout: size, base, length, type — size
                 * excludes itself (advance = size + 4). */
                synth_mmap[synth_mmap_n].size   = sizeof(struct mboot_mmap_entry) - 4;
                synth_mmap[synth_mmap_n].base   = e->base;
                synth_mmap[synth_mmap_n].length = e->length;
                synth_mmap[synth_mmap_n].type   = e->type;
                synth_mmap_n++;
            }
            synth_info.mmap_addr   = (uint32_t)(uintptr_t)&synth_mmap[0];
            synth_info.mmap_length =
                (uint32_t)(synth_mmap_n * sizeof(struct mboot_mmap_entry));
            synth_info.flags      |= MBI_FLAG_MMAP;
            break;
        }
        case MB2_TAG_FRAMEBUFFER: {
            const struct mb2_framebuffer* fb = (const struct mb2_framebuffer*)p;
            synth_info.framebuffer_addr   = fb->addr;
            synth_info.framebuffer_pitch  = fb->pitch;
            synth_info.framebuffer_width  = fb->width;
            synth_info.framebuffer_height = fb->height;
            synth_info.framebuffer_bpp    = fb->bpp;
            synth_info.framebuffer_type   = fb->fb_type;
            for (int i = 0; i < 6; i++) synth_info.color_info[i] = fb->color_info[i];
            synth_info.flags |= MBI_FLAG_FB;
            break;
        }
        default: break;
        }

        /* Advance to next tag, 8-byte aligned. */
        uint32_t advance = (t->size + 7) & ~7u;
        p += advance;
    }

    return (uintptr_t)&synth_info;
}

/* Convenience wrapper called from main_entry.c — also stamps the
 * mb1 loader magic on success so the caller can pass both into the
 * existing mboot_init() / kernel_main() unchanged. */
uint32_t mb2_loader_magic(void) {
    return MB2_LOADER_MAGIC;
}
