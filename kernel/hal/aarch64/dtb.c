/* =============================================================================
 * dtb.c — minimal Flattened Device Tree (FDT/DTB) parser (M21 Phase H).
 *
 * On ARM there is no BIOS/ACPI enumerating the machine — the firmware hands
 * the kernel a **device tree**: a compact binary description of the RAM,
 * CPUs, and MMIO devices.  Real kernels read it instead of hard-coding board
 * layout.  This phase teaches d-os to discover the machine: RAM size, CPU
 * count, and the board model, so the PMM sizes itself to whatever `-m` QEMU
 * was given rather than a baked-in constant.
 *
 * Finding the blob: QEMU's `virt` loader passes the DTB address in x0 for the
 * Linux boot protocol, but when it jumps straight to an ELF entry (our case)
 * x0 is 0.  So we accept x0 if it holds a valid FDT magic, otherwise scan low
 * RAM for the magic — a pragmatic discovery that a bootloader would normally
 * make unnecessary.
 *
 * The FDT is big-endian; every multi-byte field is byte-swapped on read.  We
 * parse just enough of the structure block: the `/memory` node's `reg` (base +
 * size) and a count of `/cpus/cpu@*` nodes.  Full DTB consumers (device
 * discovery, IRQ maps) are a later concern; this is the "know the machine"
 * slice.
 *
 * Spec: the Devicetree Specification v0.4, §5 (Flattened Devicetree).
 * ============================================================================= */

#include "printf.h"
#include <stdint.h>
#include <stddef.h>

/* FDT header (all fields big-endian u32). */
struct fdt_header {
    uint32_t magic;              /* 0xd00dfeed                                */
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

#define FDT_MAGIC        0xd00dfeedu
#define FDT_BEGIN_NODE   0x1
#define FDT_END_NODE     0x2
#define FDT_PROP         0x3
#define FDT_NOP          0x4
#define FDT_END          0x9

/* Discovered machine facts (0 = unknown).
 *
 * §M86 stage 4 (2026-09-26): RAM is a LIST of ranges.  This used to keep one
 * (base, size) and overwrite it at every `reg` it met, so a tree with two
 * /memory nodes — a NUMA machine, sbsa-ref, most real boards — handed the PMM
 * whichever node came LAST.  Measured with two QEMU NUMA nodes (1 GiB + 3 GiB):
 * 1014 MiB managed of 4096, and the kept range was not even the one holding
 * the kernel image.  Every `reg` tuple of every memory node is collected now,
 * and the reservation block is read so firmware-reserved RAM is never handed
 * out. */
#define DTB_MAX_RANGES 16
static uint64_t g_mem_base[DTB_MAX_RANGES], g_mem_size[DTB_MAX_RANGES];
static int      g_nmem;
static uint64_t g_rsv_base[DTB_MAX_RANGES], g_rsv_size[DTB_MAX_RANGES];
static int      g_nrsv;
static uint64_t g_ram_base;              /* lowest range's base (legacy getter) */
static uint64_t g_ram_size;              /* TOTAL RAM across all ranges         */
static int      g_ncpu;

static uint32_t rd32(const uint8_t* p);

/* A value of `cells` 32-bit cells, big-endian (1 or 2 in practice).  Built on
 * rd32 and NOT on `p[0] << 24`: that expression is an int, so a cell whose top
 * byte is >= 0x80 sign-extends into the upper word — the first version read
 * the bank at 0x8000_0000 as 0xFFFF_FFFF_8000_0000 and handed the PMM a range
 * ending before it began. */
static uint64_t rd_cells(const uint8_t* p, uint32_t cells) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < cells; i++) v = (v << 32) | (uint64_t)rd32(p + 4 * i);
    return v;
}

static inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

static uint32_t rd32(const uint8_t* p) {
    uint32_t v; __builtin_memcpy(&v, p, 4); return be32(v);
}

static int str_prefix(const char* s, const char* pfx) {
    while (*pfx) { if (*s++ != *pfx++) return 0; }
    return 1;
}

/* Locate the DTB: prefer the pointer firmware passed (x0), else scan low RAM
 * for the FDT magic (4 KiB-aligned, as the loader always page-aligns it). */
/* Fixed address the run script loads the DTB at (`-device loader,addr=...`),
 * since QEMU's direct-ELF `-kernel` entry provides neither an x0 pointer nor
 * an in-memory DTB of its own. */
#define DTB_LOAD_ADDR 0x48000000

static const struct fdt_header* fdt_find(uint64_t x0) {
    /* 1. Firmware-passed pointer (x0), if valid. */
    if (x0) {
        const struct fdt_header* h = (const struct fdt_header*)(uintptr_t)x0;
        if (be32(h->magic) == FDT_MAGIC) return h;
    }
    /* 2. The run-script load address. */
    {
        const struct fdt_header* h = (const struct fdt_header*)DTB_LOAD_ADDR;
        if (be32(h->magic) == FDT_MAGIC) return h;
    }
    /* 3. Fallback: scan the first 256 MiB of RAM (always present). */
    for (uintptr_t a = 0x40000000; a < 0x50000000; a += 0x1000) {
        const struct fdt_header* h = (const struct fdt_header*)a;
        if (be32(h->magic) == FDT_MAGIC) return h;
    }
    return NULL;
}

/* Parse the structure block for /memory reg + a /cpus cpu@* count. */
static void fdt_parse(const struct fdt_header* h) {
    const uint8_t* base    = (const uint8_t*)h;
    const uint8_t* strings = base + be32(h->off_dt_strings);
    const uint8_t* p       = base + be32(h->off_dt_struct);
    const uint8_t* end     = p + be32(h->size_dt_struct);

    /* Small node-name stack so a property knows which node it is in. */
    const char* namestk[8];
    int depth = 0;
    int in_cpus = 0, cpus_depth = -1;
    /* The root's #address-cells / #size-cells govern a /memory node's `reg`
     * (the spec's defaults are 2 and 1).  `virt` uses 2/2; taking that as a
     * constant would be right on the board it was written for and silently
     * wrong on the next — the same shape as a hard-coded stream count. */
    uint32_t acells = 2, scells = 1;

    int guard = 0;
    while (p < end && guard++ < 100000) {
        uint32_t tok = rd32(p); p += 4;
        if (tok == FDT_BEGIN_NODE) {
            const char* name = (const char*)p;
            size_t n = 0; while (p[n]) n++;
            p += n + 1;
            p = (const uint8_t*)(((uintptr_t)p + 3) & ~(uintptr_t)3);
            if (depth < 8) namestk[depth] = name;
            depth++;
            if (str_prefix(name, "cpus") && (name[4] == 0 || name[4] == '@')) {
                in_cpus = 1; cpus_depth = depth;   /* children are at depth+1 */
            }
            if (in_cpus && depth == cpus_depth + 1 && str_prefix(name, "cpu@")) {
                g_ncpu++;
            }
        } else if (tok == FDT_END_NODE) {
            if (in_cpus && depth == cpus_depth) in_cpus = 0;
            depth--;
        } else if (tok == FDT_PROP) {
            uint32_t len     = rd32(p); p += 4;
            uint32_t nameoff = rd32(p); p += 4;
            const char* pname = (const char*)(strings + nameoff);
            const uint8_t* val = p;
            p += len;
            p = (const uint8_t*)(((uintptr_t)p + 3) & ~(uintptr_t)3);

            if (depth == 1 && pname[0] == '#') {
                if (str_prefix(pname, "#address-cells") && len == 4) acells = rd32(val);
                if (str_prefix(pname, "#size-cells")    && len == 4) scells = rd32(val);
            }
            /* /memory@.../reg = one or more <base size> tuples. */
            const char* cur = (depth >= 1 && depth <= 8) ? namestk[depth - 1] : "";
            uint32_t tup = 4 * (acells + scells);
            if (depth == 2 && str_prefix(cur, "memory") && str_prefix(pname, "reg") &&
                acells >= 1 && acells <= 2 && scells >= 1 && scells <= 2) {
                for (uint32_t o = 0; o + tup <= len; o += tup) {
                    uint64_t b0 = rd_cells(val + o, acells);
                    uint64_t sz = rd_cells(val + o + 4 * acells, scells);
                    if (!sz) continue;
                    if (g_nmem == DTB_MAX_RANGES) {
                        kprintf("dtb: more than %d RAM ranges - ignoring %p+%u MiB\n",
                                DTB_MAX_RANGES, (void*)(uintptr_t)b0, (unsigned)(sz >> 20));
                        continue;
                    }
                    g_mem_base[g_nmem] = b0; g_mem_size[g_nmem] = sz; g_nmem++;
                }
            }
        } else if (tok == FDT_END) {
            break;
        }
        /* FDT_NOP: nothing. */
    }
}

/* Public: discover the machine from the DTB.  Called early (after the console
 * is up).  Safe to call even if no DTB is found — leaves the getters at 0. */
void dtb_init(uint64_t x0) {
    const struct fdt_header* h = fdt_find(x0);
    if (!h) { kprintf("dtb: no device tree found (using built-in defaults)\n"); return; }

    fdt_parse(h);

    /* The reservation block: (address, size) pairs of 64-bit big-endian values,
     * ended by a zero pair.  Firmware puts things there it expects to survive
     * (spin tables, secure-world carve-outs); handing one to the allocator is a
     * corruption nobody on this side would see coming. */
    const uint8_t* rv = (const uint8_t*)h + be32(h->off_mem_rsvmap);
    for (int i = 0; i < 64; i++, rv += 16) {
        uint64_t b0 = rd_cells(rv, 2), sz = rd_cells(rv + 8, 2);
        if (!b0 && !sz) break;
        if (g_nrsv < DTB_MAX_RANGES) { g_rsv_base[g_nrsv] = b0; g_rsv_size[g_nrsv] = sz; g_nrsv++; }
    }

    g_ram_base = 0; g_ram_size = 0;
    for (int i = 0; i < g_nmem; i++) {
        if (!g_ram_base || g_mem_base[i] < g_ram_base) g_ram_base = g_mem_base[i];
        g_ram_size += g_mem_size[i];
    }
    kprintf("dtb: found @ %p - RAM %u MiB in %d range(s), %d reserved, %d CPU(s)\n",
            (void*)h, (unsigned)(g_ram_size >> 20), g_nmem, g_nrsv, g_ncpu);
    for (int i = 0; i < g_nmem; i++)
        kprintf("dtb:   ram %p .. %p (%u MiB)\n", (void*)(uintptr_t)g_mem_base[i],
                (void*)(uintptr_t)(g_mem_base[i] + g_mem_size[i]),
                (unsigned)(g_mem_size[i] >> 20));
    for (int i = 0; i < g_nrsv; i++)
        kprintf("dtb:   reserved %p + %u KiB\n", (void*)(uintptr_t)g_rsv_base[i],
                (unsigned)(g_rsv_size[i] >> 10));
}

uint64_t dtb_ram_base(void) { return g_ram_base; }
uint64_t dtb_ram_size(void) { return g_ram_size; }
int dtb_mem_count(void) { return g_nmem; }
int dtb_mem_range(int i, uint64_t* base, uint64_t* size) {
    if (i < 0 || i >= g_nmem) return -1;
    *base = g_mem_base[i]; *size = g_mem_size[i]; return 0;
}
int dtb_rsv_count(void) { return g_nrsv; }
int dtb_rsv_range(int i, uint64_t* base, uint64_t* size) {
    if (i < 0 || i >= g_nrsv) return -1;
    *base = g_rsv_base[i]; *size = g_rsv_size[i]; return 0;
}
int      dtb_ncpu(void)     { return g_ncpu; }
