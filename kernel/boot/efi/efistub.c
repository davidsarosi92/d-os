/* =============================================================================
 * efistub.c — the aarch64 EFI stub: from firmware to the d-os kernel
 * (§M85 stage 3, 2026-09-26).
 *
 * What it does, in the order that matters:
 *   1. find the ACPI RSDP and/or a device tree among the firmware's
 *      configuration tables;
 *   2. place the kernel anywhere 2 MiB-aligned (+ its TEXT_OFFSET) and copy
 *      the embedded image there; the .bss beyond the file is zeroed by the
 *      kernel itself, which maps and relocates itself (mmu.c, §M85 stage 4);
 *   3. get the memory map and EXIT BOOT SERVICES — after which no firmware
 *      call but the runtime ones may be made, so everything that needs one
 *      happens before, and the map is converted afterwards by pure arithmetic;
 *   4. clean the data cache over everything the kernel will read with its MMU
 *      off (a write sitting in a cache line is invisible to an uncached read),
 *      turn this EL's MMU and caches off, and jump with x0 = the boot info.
 *
 * Position independent and relocation free (see efistub_head.S): no pointer
 * to a static object may be stored in static data, because the value the
 * linker would put there is a link-time (zero-based) address.
 *
 * Deliberately small: no protocols beyond the console, no file I/O (the
 * kernel is inside this image), no graphics output handover yet.
 * ============================================================================= */

#include <stdint.h>
#include <stddef.h>
#include "efi_bootinfo.h"

#pragma GCC visibility push(hidden)

typedef uint64_t efi_status;
typedef uint16_t char16;

#define EFI_SUCCESS            0
#define EFI_BUFFER_TOO_SMALL   (0x8000000000000000ull | 5)

struct efi_guid { uint32_t a; uint16_t b, c; uint8_t d[8]; };

struct efi_table_header { uint64_t sig; uint32_t rev, size, crc, reserved; };

struct efi_text_out {
    void* reset;
    efi_status (*output_string)(struct efi_text_out*, const char16*);
};

struct efi_boot_services {
    struct efi_table_header hdr;
    void* raise_tpl; void* restore_tpl;
    efi_status (*allocate_pages)(int type, int mem_type, uint64_t pages, uint64_t* addr);
    efi_status (*free_pages)(uint64_t addr, uint64_t pages);
    efi_status (*get_memory_map)(uint64_t* size, void* map, uint64_t* key,
                                 uint64_t* desc_size, uint32_t* desc_ver);
    efi_status (*allocate_pool)(int mem_type, uint64_t size, void** buf);
    efi_status (*free_pool)(void* buf);
    void* create_event; void* set_timer; void* wait_for_event; void* signal_event;
    void* close_event; void* check_event;
    void* install_protocol; void* reinstall_protocol; void* uninstall_protocol;
    void* handle_protocol; void* reserved; void* register_protocol_notify;
    void* locate_handle; void* locate_device_path; void* install_config_table;
    void* load_image; void* start_image; void* exit; void* unload_image;
    efi_status (*exit_boot_services)(void* image, uint64_t map_key);
};

struct efi_config_table { struct efi_guid guid; void* table; };

struct efi_system_table {
    struct efi_table_header hdr;
    char16* fw_vendor; uint32_t fw_revision;
    void* con_in_handle; void* con_in;
    void* con_out_handle; struct efi_text_out* con_out;
    void* stderr_handle; void* std_err;
    void* runtime;
    struct efi_boot_services* boot;
    uint64_t n_tables;
    struct efi_config_table* tables;
};

struct efi_mem_desc { uint32_t type, pad; uint64_t phys, virt, pages, attr; };

enum { AllocateAnyPages = 0, AllocateMaxAddress = 1, AllocateAddress = 2 };
enum { EfiLoaderCode = 1, EfiLoaderData = 2, EfiBootServicesCode = 3,
       EfiBootServicesData = 4, EfiConventionalMemory = 7 };

extern const uint8_t kernel_payload[], kernel_payload_end[];

/* ---- tiny helpers (no libc here) ------------------------------------------ */
static void* mcpy(void* d, const void* s, uint64_t n) {
    uint8_t* dd = d; const uint8_t* ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
static void mset(void* d, int v, uint64_t n) { uint8_t* dd = d; while (n--) *dd++ = (uint8_t)v; }

static int guid_eq(const struct efi_guid* g, uint32_t a, uint16_t b, uint16_t c,
                   const uint8_t d[8]) {
    if (g->a != a || g->b != b || g->c != c) return 0;
    for (int i = 0; i < 8; i++) if (g->d[i] != d[i]) return 0;
    return 1;
}

static struct efi_system_table* g_st;

/* Console output.  UEFI text is UCS-2, so ASCII is widened on the stack —
 * a string constant cannot be pre-built as a char16 array without its
 * address becoming a relocation. */
static void puts8(const char* s) {
    if (!g_st || !g_st->con_out) return;
    char16 buf[128];
    while (*s) {
        int n = 0;
        while (*s && n < 126) {
            if (*s == '\n') { if (n > 124) break; buf[n++] = '\r'; }
            buf[n++] = (char16)(uint8_t)*s++;
        }
        buf[n] = 0;
        g_st->con_out->output_string(g_st->con_out, buf);
    }
}
static void puthex(uint64_t v) {
    char b[19]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) { int d = (v >> (60 - 4 * i)) & 15; b[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); }
    b[18] = 0;
    puts8(b);
}

/* Clean [p, p+n) to the point of coherency, line by line. */
static void dcache_clean(uint64_t p, uint64_t n) {
    uint64_t ctr;
    __asm__ volatile ("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ull << ((ctr >> 16) & 0xF);          /* DminLine, bytes */
    for (uint64_t a = p & ~(line - 1); a < p + n; a += line)
        __asm__ volatile ("dc cvac, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb sy" ::: "memory");
}

static int usable(uint32_t t) {
    /* Free once boot services are gone.  LoaderCode/Data includes THIS stub and
     * the boot-info page — the kernel reads the info before its allocator
     * exists (dtb_init copies it), and nothing else of ours is needed after
     * the jump.  The kernel's own image is carved out by the kernel. */
    return t == EfiConventionalMemory || t == EfiBootServicesCode ||
           t == EfiBootServicesData || t == EfiLoaderCode || t == EfiLoaderData;
}

efi_status efi_main(void* image, struct efi_system_table* st) {
    g_st = st;
    puts8("d-os EFI stub: starting\n");

    /* --- 1. what describes this machine --------------------------------- */
    static const uint8_t acpi20_d[8] = { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 };
    static const uint8_t fdt_d[8]    = { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 };
    uint64_t rsdp = 0, dtb = 0;
    for (uint64_t i = 0; i < st->n_tables; i++) {
        const struct efi_config_table* t = &st->tables[i];
        if (guid_eq(&t->guid, 0x8868e871, 0xe4f1, 0x11d3, acpi20_d)) rsdp = (uint64_t)(uintptr_t)t->table;
        if (guid_eq(&t->guid, 0xb1b621d5, 0xf19c, 0x41a5, fdt_d))    dtb  = (uint64_t)(uintptr_t)t->table;
    }
    puts8("  acpi rsdp "); puthex(rsdp); puts8("  dtb "); puthex(dtb); puts8("\n");

    /* --- 2. the kernel's load range --------------------------------------- */
    /* ANYWHERE, 2 MiB-aligned + TEXT_OFFSET (§M85 stage 4): the kernel maps
     * its image with 2 MiB blocks and runs at a fixed virtual address, so the
     * physical placement only has to keep that alignment.  It used to be a
     * fixed address, which does not exist as RAM on most machines. */
    uint64_t img = (uint64_t)(kernel_payload_end - kernel_payload);
    uint64_t span = DOS_KERNEL_SPAN > img ? DOS_KERNEL_SPAN : img;
    uint64_t pages = (span + DOS_KERNEL_TEXT_OFFSET + 0x200000 + 0xFFF) >> 12;
    uint64_t region = 0;
    efi_status s = st->boot->allocate_pages(AllocateAnyPages, EfiLoaderData, pages, &region);
    if (s != EFI_SUCCESS) {
        puts8("d-os EFI stub: no room for the kernel ("); puthex(pages); puts8(" pages, status ");
        puthex(s); puts8(")\n");
        return s;
    }
    uint64_t load = ((region + 0x1FFFFF) & ~0x1FFFFFull) + DOS_KERNEL_TEXT_OFFSET;
    mcpy((void*)(uintptr_t)load, kernel_payload, img);
    puts8("  kernel "); puthex(img); puts8(" bytes at "); puthex(load); puts8("\n");

    /* --- 3. boot info + memory map, then leave boot services -------------- */
    struct dos_bootinfo* bi = 0;
    uint64_t bi_addr = 0;                      /* anywhere: the kernel copies it
                                                * with its MMU off (mmu.c) */
    if (st->boot->allocate_pages(AllocateAnyPages, EfiLoaderData,
                                 (sizeof *bi + 0xFFF) >> 12, &bi_addr) != EFI_SUCCESS) {
        puts8("d-os EFI stub: no page for the boot info\n");
        return 1;
    }
    bi = (struct dos_bootinfo*)(uintptr_t)bi_addr;
    mset(bi, 0, sizeof *bi);
    bi->magic = DOS_BOOTINFO_MAGIC;
    bi->rsdp = rsdp;
    bi->dtb = dtb;
    for (int i = 0; i < 63 && st->fw_vendor && st->fw_vendor[i]; i++)
        bi->fw_vendor[i] = st->fw_vendor[i] < 128 ? (char)st->fw_vendor[i] : '?';

    uint64_t el;
    __asm__ volatile ("mrs %0, CurrentEL" : "=r"(el));
    bi->entered_el = (uint32_t)((el >> 2) & 3);

    /* The map buffer is allocated BEFORE the final GetMemoryMap: allocating
     * after it changes the map, and ExitBootServices then rejects the key. */
    uint64_t map_cap = 64 * 1024, map_size, key, dsz;
    uint32_t dver;
    void* map = 0;
    if (st->boot->allocate_pool(EfiLoaderData, map_cap, &map) != EFI_SUCCESS) return 1;
    puts8("d-os EFI stub: exiting boot services\n");
    for (int tries = 0; ; tries++) {
        map_size = map_cap;
        s = st->boot->get_memory_map(&map_size, map, &key, &dsz, &dver);
        if (s != EFI_SUCCESS) return s;
        s = st->boot->exit_boot_services(image, key);
        if (s == EFI_SUCCESS) break;
        if (tries > 4) return s;            /* the map keeps changing: give up */
    }
    /* From here on: no boot services, no console. */

    uint32_t n = 0, raw = (uint32_t)(map_size / dsz);
    for (uint32_t i = 0; i < raw; i++) {
        const struct efi_mem_desc* d = (const struct efi_mem_desc*)((uint8_t*)map + i * dsz);
        if (!usable(d->type)) continue;
        uint64_t b = d->phys, len = d->pages << 12;
        if (n && bi->mem[n - 1].base + bi->mem[n - 1].size == b) { bi->mem[n - 1].size += len; continue; }
        if (n == DOS_BI_MAXMEM) break;
        bi->mem[n].base = b; bi->mem[n].size = len; n++;
    }
    bi->nmem = n;
    bi->efi_entries = raw;

    /* --- 4. hand over ---------------------------------------------------- */
    __asm__ volatile ("msr daifset, #0xf" ::: "memory");
    dcache_clean(load, img);
    dcache_clean(bi_addr, sizeof *bi);
    __asm__ volatile ("ic iallu\n dsb sy\n isb" ::: "memory");

    /* MMU + caches off at the current EL (EDK2 runs at EL1 on `virt`, EL2 on
     * machines with virtualisation), executing from identity-mapped memory. */
    if (bi->entered_el == 2) {
        uint64_t v;
        __asm__ volatile ("mrs %0, sctlr_el2" : "=r"(v));
        v &= ~((1ull << 0) | (1ull << 2) | (1ull << 12));
        __asm__ volatile ("msr sctlr_el2, %0\n isb" :: "r"(v));
    } else {
        uint64_t v;
        __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(v));
        v &= ~((1ull << 0) | (1ull << 2) | (1ull << 12));
        __asm__ volatile ("msr sctlr_el1, %0\n isb" :: "r"(v));
    }
    __asm__ volatile ("ic iallu\n tlbi vmalle1\n dsb sy\n isb" ::: "memory");

    void (*kernel)(uint64_t) = (void (*)(uint64_t))(uintptr_t)load;
    kernel(bi_addr);
    for (;;) __asm__ volatile ("wfe");
}
