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
#include "board.h"
#include "efi_bootinfo.h"
#include "hal_api.h"

extern struct dos_bootinfo aarch64_bootinfo;   /* mmu.c copied it at PA */
extern int aarch64_have_bootinfo;
void mmu_direct_map_range(uint64_t base, uint64_t size);

int acpi_arm_init(uint64_t rsdp_phys);
int acpi_arm_ncpu(void);
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

/* A physical tree address as a pointer: through the direct map (§M85 — RAM is
 * no longer identity-mapped), mapping its GiB first.  Addresses below the
 * direct map's base cannot be reached and yield NULL. */
static const struct fdt_header* fdt_at(uint64_t pa) {
    extern uint64_t aarch64_phys_offset;
    if (!pa || pa < aarch64_phys_offset) return NULL;
    mmu_direct_map_range(pa, 0x100000);
    const struct fdt_header* h = (const struct fdt_header*)phys_to_virt(pa);
    return be32(h->magic) == FDT_MAGIC ? h : NULL;
}

static const struct fdt_header* fdt_find(uint64_t x0) {
    const struct fdt_header* h;
    /* 1. Firmware-passed pointer (x0), if valid. */
    if ((h = fdt_at(x0))) return h;
    /* 2. The run-script load address. */
    if ((h = fdt_at(DTB_LOAD_ADDR))) return h;
    /* 3. Fallback: scan the first 256 MiB of `virt` RAM — only reachable, and
     *    only meaningful, when that is where the image was loaded. */
    for (uint64_t a = 0x40000000; a < 0x50000000; a += 0x1000)
        if ((h = fdt_at(a))) return h;
    return NULL;
}

/* ---- §M85 stage 1: the devices, from the tree ------------------------------
 *
 * A property describes the node it sits in, but that node's meaning (its
 * `compatible`) may arrive after its `reg` — so each node's interesting
 * properties are COLLECTED while it is open and acted on at its END.  The
 * cells that size a node's `reg` are its PARENT's #address-cells/#size-cells;
 * a node's own values size its CHILDREN (and its `ranges` child side). */
struct fdt_node {
    const char*    name;
    const uint8_t* compat;  uint32_t compat_len;
    const uint8_t* reg;     uint32_t reg_len;
    const uint8_t* intr;    uint32_t intr_len;
    const uint8_t* ranges;  uint32_t ranges_len;
    const uint8_t* imap;    uint32_t imap_len;       /* §M85 interrupt-map      */
    const uint8_t* imask;   uint32_t imask_len;      /* interrupt-map-mask      */
    const char*    method;                   /* /psci: "hvc" or "smc"         */
    int            disabled;
    uint32_t       acells, scells;          /* for this node's children      */
};

/* Does the NUL-separated compatible list contain `want`? */
static int compat_has(const struct fdt_node* n, const char* want) {
    const char* c = (const char*)n->compat;
    uint32_t off = 0;
    while (c && off < n->compat_len) {
        const char* s0 = c + off;
        uint32_t k = 0;
        while (off + k < n->compat_len && s0[k]) k++;
        uint32_t w = 0; while (want[w]) w++;
        if (k == w) {
            uint32_t i = 0; while (i < w && s0[i] == want[i]) i++;
            if (i == w) return 1;
        }
        off += k + 1;
    }
    return 0;
}

/* reg tuple `i` of node n, with the parent's cells. */
static int reg_tuple(const struct fdt_node* n, uint32_t ac, uint32_t sc, int i,
                     uint64_t* base, uint64_t* size) {
    uint32_t tup = 4 * (ac + sc);
    if (!n->reg || ac < 1 || ac > 2 || sc > 2 || (uint32_t)(i + 1) * tup > n->reg_len) return -1;
    *base = rd_cells(n->reg + i * tup, ac);
    *size = sc ? rd_cells(n->reg + i * tup + 4 * ac, sc) : 0;
    return 0;
}

/* GIC-style `interrupts` specifier `i` (3 cells: type, number, flags) as an
 * INTID: an SPI is 32 + n, a PPI 16 + n.  0 = none. */
static uint32_t intr_intid(const struct fdt_node* n, int i) {
    if (!n->intr || (uint32_t)(i + 1) * 12 > n->intr_len) return 0;
    uint32_t type = rd32(n->intr + i * 12), num = rd32(n->intr + i * 12 + 4);
    return type == 1 ? num + 16 : num + 32;
}

/* §M85 — PCI legacy interrupts on a DEVICE-TREE machine: the host bridge's
 * `interrupt-map`.  Copied out (the DTB's memory is not ours to keep), and
 * decoded only when a driver asks, because an entry's length depends on the
 * interrupt controller's own #address-cells, which may be parsed after the
 * bridge.  Entry = child unit address (3 cells) + child specifier (1: the
 * pin) + parent phandle (1) + parent unit address (GIC #address-cells) +
 * parent specifier (3: type, number, flags). */
#define IMAP_MAX 1024
static uint8_t  g_imap[IMAP_MAX];
static uint32_t g_imap_len;
static uint32_t g_imask[4];
static int      g_have_imask;
static uint32_t g_gic_acells = 2;

int dtb_pci_intx(uint8_t slot, uint8_t pin) {
    if (!g_imap_len || pin < 1 || pin > 4) return -1;
    uint32_t cells = 3 + 1 + 1 + g_gic_acells + 3;
    uint32_t stride = 4 * cells;
    uint32_t hi = (uint32_t)slot << 11;
    uint32_t mhi = g_have_imask ? g_imask[0] : 0xF800u;
    uint32_t mpin = g_have_imask ? g_imask[3] : 7u;
    for (uint32_t o = 0; o + stride <= g_imap_len; o += stride) {
        uint32_t ehi  = rd32(g_imap + o);
        uint32_t epin = rd32(g_imap + o + 12);
        if ((ehi & mhi) != (hi & mhi) || (epin & mpin) != (pin & mpin)) continue;
        const uint8_t* ps = g_imap + o + 4 * (3 + 1 + 1 + g_gic_acells);
        uint32_t type = rd32(ps), num = rd32(ps + 4);
        return (int)(type == 1 ? num + 16 : num + 32);
    }
    return -1;
}

static void node_commit(const struct fdt_node* n, uint32_t ac, uint32_t sc) {
    if (n->disabled || !n->compat) return;
    uint64_t b0, sz, b1, sz1;
    if (compat_has(n, "arm,gic-v3")) {
        if (reg_tuple(n, ac, sc, 0, &b0, &sz) == 0 && reg_tuple(n, ac, sc, 1, &b1, &sz1) == 0) {
            g_board.gic_version = 3; g_board.gicd = b0; g_board.gicr = b1; g_board.gicr_size = sz1;
        }
        g_gic_acells = n->acells;
    } else if (compat_has(n, "arm,cortex-a15-gic") || compat_has(n, "arm,gic-400") ||
               compat_has(n, "arm,cortex-a9-gic")  || compat_has(n, "arm,cortex-a7-gic")) {
        if (reg_tuple(n, ac, sc, 0, &b0, &sz) == 0 && reg_tuple(n, ac, sc, 1, &b1, &sz1) == 0) {
            g_board.gic_version = 2; g_board.gicd = b0; g_board.gicc = b1;
        }
        g_gic_acells = n->acells;
    } else if (compat_has(n, "arm,psci-1.0") || compat_has(n, "arm,psci-0.2") ||
               compat_has(n, "arm,psci")) {
        if (n->method) g_board.psci_smc = str_prefix(n->method, "smc");
    } else if (compat_has(n, "arm,armv8-timer")) {
        /* Specifiers: secure phys, NON-SECURE PHYS, virtual, hypervisor. */
        uint32_t id = intr_intid(n, 1);
        if (id) g_board.timer_intid = id;
    } else if (compat_has(n, "arm,pl011")) {
        if (!g_board.uart && reg_tuple(n, ac, sc, 0, &b0, &sz) == 0) {
            g_board.uart = b0; g_board.uart_intid = intr_intid(n, 0);
        }
    } else if (compat_has(n, "arm,pl031")) {
        if (!g_board.rtc && reg_tuple(n, ac, sc, 0, &b0, &sz) == 0) g_board.rtc = b0;
    } else if (compat_has(n, "virtio,mmio")) {
        if (g_board.nvirtio < BOARD_MAX_VIRTIO && reg_tuple(n, ac, sc, 0, &b0, &sz) == 0) {
            g_board.virtio_base[g_board.nvirtio]  = b0;
            g_board.virtio_intid[g_board.nvirtio] = intr_intid(n, 0);
            g_board.nvirtio++;
        }
    } else if (compat_has(n, "pci-host-ecam-generic")) {
        if (reg_tuple(n, ac, sc, 0, &b0, &sz) == 0) { g_board.ecam = b0; g_board.ecam_size = sz; }
        if (n->imap && n->imap_len <= IMAP_MAX) {
            for (uint32_t k = 0; k < n->imap_len; k++) g_imap[k] = n->imap[k];
            g_imap_len = n->imap_len;
        }
        if (n->imask && n->imask_len == 16) {
            for (int k = 0; k < 4; k++) g_imask[k] = rd32(n->imask + 4 * k);
            g_have_imask = 1;
        }
        /* ranges = <child-addr (the node's own #address-cells, 3 for PCI)
         *           parent-addr (ac)  size (the node's own #size-cells)>.
         * The top cell's bits 25:24 are the space: 2 = 32-bit memory. */
        uint32_t cac = n->acells, csc = n->scells;
        uint32_t ent = 4 * (cac + ac + csc);
        for (uint32_t o = 0; n->ranges && cac == 3 && o + ent <= n->ranges_len; o += ent) {
            uint32_t space = (rd32(n->ranges + o) >> 24) & 3;
            uint64_t cpu = rd_cells(n->ranges + o + 4 * cac, ac);
            uint64_t len = rd_cells(n->ranges + o + 4 * (cac + ac), csc);
            if (space == 2 && !g_board.pci_mmio32) { g_board.pci_mmio32 = cpu; g_board.pci_mmio32_size = len; }
        }
    }
}

static int g_have_bi_mem;       /* RAM came from the EFI map: ignore /memory */

/* Parse the structure block: /memory, /cpus, /model and the devices. */
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
    struct fdt_node nodes[8];

    int guard = 0;
    while (p < end && guard++ < 100000) {
        uint32_t tok = rd32(p); p += 4;
        if (tok == FDT_BEGIN_NODE) {
            const char* name = (const char*)p;
            size_t n = 0; while (p[n]) n++;
            p += n + 1;
            p = (const uint8_t*)(((uintptr_t)p + 3) & ~(uintptr_t)3);
            if (depth < 8) {
                namestk[depth] = name;
                struct fdt_node* nd = &nodes[depth];
                *nd = (struct fdt_node){ .name = name, .acells = 2, .scells = 1 };
            }
            depth++;
            if (str_prefix(name, "cpus") && (name[4] == 0 || name[4] == '@')) {
                in_cpus = 1; cpus_depth = depth;   /* children are at depth+1 */
            }
            if (in_cpus && depth == cpus_depth + 1 && str_prefix(name, "cpu@")) {
                g_ncpu++;
            }
        } else if (tok == FDT_END_NODE) {
            if (in_cpus && depth == cpus_depth) in_cpus = 0;
            /* Only the root's children are committed: that is where `virt`
             * (and sbsa-ref's tree, when it has one) put their devices.  A board
             * that nests them under a /soc bus with `ranges` translation would
             * need that translation applied first — refused rather than read
             * with untranslated addresses. */
            if (depth == 2) node_commit(&nodes[1], nodes[0].acells, nodes[0].scells);
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
            if (depth == 1 && str_prefix(pname, "model") && pname[5] == 0) {
                /* COPIED: the tree lives in RAM the allocator will reuse (the
                 * raw boot's 0x4800_0000, or firmware boot-services memory), so
                 * a pointer into it is a dangling pointer by the first shell. */
                static char model[64];
                uint32_t k = 0;
                while (k + 1 < sizeof model && k < len && val[k]) { model[k] = (char)val[k]; k++; }
                model[k] = 0;
                g_board.model = model;
            }
            if (depth >= 1 && depth <= 8) {
                struct fdt_node* nd = &nodes[depth - 1];
                if (str_prefix(pname, "#address-cells") && len == 4) nd->acells = rd32(val);
                else if (str_prefix(pname, "#size-cells") && len == 4) nd->scells = rd32(val);
                else if (str_prefix(pname, "compatible") && pname[10] == 0) { nd->compat = val; nd->compat_len = len; }
                else if (str_prefix(pname, "reg") && pname[3] == 0)        { nd->reg = val; nd->reg_len = len; }
                else if (str_prefix(pname, "interrupts") && pname[10] == 0) { nd->intr = val; nd->intr_len = len; }
                else if (str_prefix(pname, "ranges") && pname[6] == 0)     { nd->ranges = val; nd->ranges_len = len; }
                else if (str_prefix(pname, "interrupt-map-mask"))          { nd->imask = val; nd->imask_len = len; }
                else if (str_prefix(pname, "interrupt-map") && pname[13] == 0) { nd->imap = val; nd->imap_len = len; }
                else if (str_prefix(pname, "method") && pname[6] == 0)     nd->method = (const char*)val;
                else if (str_prefix(pname, "status") && pname[6] == 0 &&
                         !str_prefix((const char*)val, "okay") && !str_prefix((const char*)val, "ok"))
                    nd->disabled = 1;
            }
            /* /memory@.../reg = one or more <base size> tuples. */
            const char* cur = (depth >= 1 && depth <= 8) ? namestk[depth - 1] : "";
            uint32_t tup = 4 * (acells + scells);
            if (depth == 2 && !g_have_bi_mem && str_prefix(cur, "memory") && str_prefix(pname, "reg") &&
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
/* §M85 stage 3 — booted through the EFI stub (efi_bootinfo.h)?  The boot
 * info was copied into the image by mmu.c while it was still reachable at its
 * physical address (stage 4). */
#define g_bi       aarch64_bootinfo
#define g_have_bi  aarch64_have_bootinfo

void dtb_init(uint64_t x0) {
    const struct fdt_header* h;
    if (g_have_bi) {
        /* RAM first: the tree and the ACPI tables below live in it and are
         * read through the direct map. */
        for (uint32_t i = 0; i < g_bi.nmem; i++) mmu_direct_map_range(g_bi.mem[i].base, g_bi.mem[i].size);
        kprintf("efi: booted by '%s' (entered at EL%u): %u usable range(s) from %u "
                "descriptors, ACPI %p, device tree %p\n", g_bi.fw_vendor, g_bi.entered_el,
                g_bi.nmem, g_bi.efi_entries, (void*)(uintptr_t)g_bi.rsdp,
                (void*)(uintptr_t)g_bi.dtb);
        /* Only the tree the FIRMWARE named — never a scan of RAM for a magic
         * number, which on a board that is not `virt` reads whatever device
         * happens to live at 0x4000_0000 (sbsa-ref: the GIC). */
        h = fdt_at(g_bi.dtb);
    } else {
        h = fdt_find(x0);
    }
    if (g_have_bi) {
        /* UNDER UEFI THE MEMORY MAP IS THE FIRMWARE'S, NOT THE TREE'S.  A
         * /memory node describes the whole bank; runtime services, ACPI tables
         * and the firmware's own reservations sit inside it, and only the EFI
         * map says which pages are really free.  Taken first, so a tree parsed
         * below can add devices but not RAM. */
        for (uint32_t i = 0; i < g_bi.nmem && g_nmem < DTB_MAX_RANGES; i++) {
            g_mem_base[g_nmem] = g_bi.mem[i].base; g_mem_size[g_nmem] = g_bi.mem[i].size; g_nmem++;
        }
        g_have_bi_mem = g_bi.nmem > 0;
        if (g_bi.nmem > DTB_MAX_RANGES)
            kprintf("efi: %u usable ranges, only %d kept\n", g_bi.nmem, DTB_MAX_RANGES);
    }
    if (!h) {
        if (g_have_bi && g_bi.rsdp && acpi_arm_init(g_bi.rsdp) == 0) {
            g_ncpu = acpi_arm_ncpu();
            board_finish();
            return;
        }
        kprintf("dtb: no device tree found (using built-in defaults)\n");
        board_finish();
        return;
    }

    g_board.src = BOARD_DTB;
    fdt_parse(h);
    board_finish();

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
    kprintf("dtb:   gic v%d @ %p, %d virtio, ecam %p\n", g_board.gic_version,
            (void*)(uintptr_t)g_board.gicd, g_board.nvirtio, (void*)(uintptr_t)g_board.ecam);
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
