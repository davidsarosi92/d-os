/* =============================================================================
 * multiboot.c — cache and pretty-print the multiboot info GRUB hands us.
 *
 * `mboot_init` validates the loader magic and stashes the info pointer so
 * later subsystems (PMM in the next milestone, VBE later) can walk the
 * memory map.  `mboot_print_meminfo` powers the shell `meminfo` command
 * and is also useful for eyeballing what the loader actually gave us.
 *
 * We deliberately don't parse the struct beyond recording the pointer;
 * each field is consumed lazily when a consumer calls for it.  That keeps
 * this translation unit tiny and avoids committing to interpretations we
 * might regret later.
 * ============================================================================= */

#include "multiboot.h"
#include "printf.h"
#include <stdint.h>

static const struct mboot_info* g_mbi = 0;

/* Human-readable names for the five mmap type codes. */
static const char* mmap_type_name(uint32_t t) {
    switch (t) {
        case MMAP_TYPE_AVAILABLE:    return "AVAILABLE";
        case MMAP_TYPE_RESERVED:     return "RESERVED";
        case MMAP_TYPE_ACPI_RECLAIM: return "ACPI RECLAIM";
        case MMAP_TYPE_ACPI_NVS:     return "ACPI NVS";
        case MMAP_TYPE_BAD:          return "BAD RAM";
        default:                     return "unknown";
    }
}

/* Print one 64-bit value in hex.  Our kprintf only knows %x on 32-bit
 * integers, so we emit `hi_lo` when high is non-zero or just `lo` when
 * it fits in 32 bits — more readable than always printing 16 hex digits. */
static void print_u64_hex(uint64_t v) {
    uint32_t hi = (uint32_t)(v >> 32);
    uint32_t lo = (uint32_t)(v & 0xFFFFFFFFu);
    if (hi) kprintf("%x_%x", hi, lo);
    else    kprintf("%x", lo);
}

int mboot_init(uint32_t magic, uintptr_t info_ptr) {
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) return -1;
    if (info_ptr == 0) return -1;
    g_mbi = (const struct mboot_info*)info_ptr;
    return 0;
}

const struct mboot_info* mboot_get_info(void) {
    return g_mbi;
}

void mboot_print_meminfo(void) {
    if (!g_mbi) {
        kprintf("meminfo: multiboot info unavailable\n");
        return;
    }

    /* Legacy totals, useful as a quick sanity check. */
    if (g_mbi->flags & MBI_FLAG_MEM) {
        kprintf("mem_lower: %u KiB\n", g_mbi->mem_lower);
        kprintf("mem_upper: %u KiB (%u MiB)\n",
                g_mbi->mem_upper, g_mbi->mem_upper / 1024);
    } else {
        kprintf("mem_lower/upper: not provided by loader\n");
    }

    /* The real memory map. */
    if ((g_mbi->flags & MBI_FLAG_MMAP) == 0 || g_mbi->mmap_length == 0) {
        kprintf("memory map: not provided\n");
        return;
    }

    kprintf("memory map (%u bytes @ %p):\n",
            g_mbi->mmap_length, (void*)(uintptr_t)g_mbi->mmap_addr);

    uintptr_t p   = g_mbi->mmap_addr;
    uintptr_t end = g_mbi->mmap_addr + g_mbi->mmap_length;
    int idx = 0;
    uint64_t total_available = 0;

    /* Each entry advances `size + 4` bytes — `size` excludes its own
     * 4-byte field.  We also defensively cap the iteration count to avoid
     * a runaway loop if a bogus `size` points us backwards. */
    while (p < end && idx < 64) {
        const struct mboot_mmap_entry* e = (const struct mboot_mmap_entry*)p;

        kprintf("  [%d] base=0x", idx);
        print_u64_hex(e->base);
        kprintf(" len=0x");
        print_u64_hex(e->length);
        kprintf(" %s\n", mmap_type_name(e->type));

        if (e->type == MMAP_TYPE_AVAILABLE) total_available += e->length;

        p += e->size + 4;
        idx++;
    }

    /* Report usable RAM as a tidy MiB figure.  Cast down because our
     * kprintf doesn't speak 64-bit. */
    uint32_t avail_mib = (uint32_t)(total_available / (1024ull * 1024ull));
    kprintf("available: ~%u MiB\n", avail_mib);
}
