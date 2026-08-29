/* =============================================================================
 * ahci.c — SATA storage through an AHCI host controller.
 *
 * -----------------------------------------------------------------------------
 * WHY THIS AND NOT ANOTHER VIRTIO DEVICE
 *
 * Until now this system had exactly one block driver — virtio-blk — which means
 * that on ANY PHYSICAL MACHINE d-os has no disk at all: no persistent config, no
 * exFAT mount, nothing that survives a reboot.  Everything §M63 and §M64 built
 * on top of "the persistent volume" is, on real hardware, built on nothing.
 *
 * AHCI is what every SATA controller made this century speaks, and it is what
 * QEMU, VirtualBox and VMware all present.  ONE DRIVER, EVERY PLATFORM — which
 * is why it outranks anything hypervisor-specific: a VirtualBox-only device
 * would be a driver for one vendor's emulator.
 *
 * The device manager put this on the list before the driver existed: on a q35
 * machine it already printed
 *
 *     Intel ICH9 SATA controller (AHCI)  0:1f.2  8086:2922  (none)  needs a driver
 *
 * which is the machine reporting the gap about itself.
 *
 * -----------------------------------------------------------------------------
 * THE SHAPE OF AHCI, IN ONE PARAGRAPH
 *
 * The controller exposes an MMIO window (ABAR = BAR5) with a global block and
 * up to 32 PORT blocks at 0x100 + n*0x80.  A port owns two pieces of memory the
 * CONTROLLER reads by physical address: a COMMAND LIST of 32 slots, and a
 * RECEIVED FIS area it writes status into.  Each command slot points at a
 * COMMAND TABLE holding the ATA command (as a "register FIS") plus a scatter
 * list (the PRDT) naming the data buffers.  Issuing a command means filling a
 * slot and setting its bit in CI; the controller clears that bit when done.
 *
 * -----------------------------------------------------------------------------
 * FOUR THINGS THAT ARE EASY TO GET WRONG AND SILENT WHEN YOU DO
 *
 * 1. **ALIGNMENT IS ARCHITECTURAL, NOT ADVISORY.**  The command list must be
 *    1 KiB aligned, the FIS area 256 B, each command table 128 B.  The
 *    controller ignores the low bits rather than complaining, so a misaligned
 *    structure means it reads a DIFFERENT address than the one you wrote — and
 *    the failure is a wrong transfer, not an error.
 *
 * 2. **THE PORT MUST BE STOPPED BEFORE ITS POINTERS MOVE.**  CLB/FB may only be
 *    changed with ST and FRE clear AND with CR and FR observed to have gone
 *    clear.  Writing them while the engine runs points live hardware at memory
 *    that is about to be something else.
 *
 * 3. **PRDT BYTE COUNT IS "COUNT MINUS ONE".**  A field that is off by one in
 *    the direction that still works for most transfers is the worst kind: a
 *    512-byte read written as 512 transfers 513 bytes.
 *
 * 4. **A BOUNCE BUFFER, DELIBERATELY.**  The caller's buffer is a kernel
 *    virtual address and the controller needs a PHYSICAL one; on i386 those
 *    happen to be equal inside the identity map and on x86_64 they are not.
 *    Rather than make every caller's allocation a DMA question, this driver
 *    owns one contiguous buffer and copies.  It costs a memcpy per transfer and
 *    removes a class of bug where a driver works until somebody passes it a
 *    buffer from the wrong allocator.
 * ============================================================================= */

#include "driver.h"
#include "hwdev.h"
#include "block.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "printf.h"
#include "klog.h"
#include "task.h"
#include "hal_api.h"
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- */
/* Register map.                                                     */
/* ---------------------------------------------------------------- */

#define HBA_CAP        0x00
#define HBA_GHC        0x04
#define HBA_IS         0x08
#define HBA_PI         0x0C
#define HBA_VS         0x10
#define HBA_CAP2       0x24
#define HBA_BOHC       0x28

#define GHC_HR         (1u << 0)      /* HBA reset            */
#define GHC_IE         (1u << 1)      /* interrupt enable     */
#define GHC_AE         (1u << 31)     /* AHCI enable          */

#define CAP2_BOH       (1u << 0)      /* BIOS/OS handoff supported */
#define BOHC_BOS       (1u << 0)      /* BIOS owned           */
#define BOHC_OOS       (1u << 1)      /* OS owned             */
#define BOHC_BB        (1u << 4)      /* busy                 */

#define PORT_BASE      0x100
#define PORT_STRIDE    0x80

#define P_CLB          0x00
#define P_CLBU         0x04
#define P_FB           0x08
#define P_FBU          0x0C
#define P_IS           0x10
#define P_IE           0x14
#define P_CMD          0x18
#define P_TFD          0x20
#define P_SIG          0x24
#define P_SSTS         0x28
#define P_SCTL         0x2C
#define P_SERR         0x30
#define P_SACT         0x34
#define P_CI           0x38

#define PCMD_ST        (1u << 0)
#define PCMD_FRE       (1u << 4)
#define PCMD_FR        (1u << 14)
#define PCMD_CR        (1u << 15)

#define TFD_ERR        (1u << 0)
#define TFD_DRQ        (1u << 3)
#define TFD_BSY        (1u << 7)

#define SSTS_DET_MASK  0x0F
#define SSTS_DET_READY 0x03           /* device present, PHY communicating */

#define SIG_ATA        0x00000101u
#define SIG_ATAPI      0xEB140101u

/* ATA commands. */
#define ATA_IDENTIFY       0xEC
#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_FLUSH_EXT      0xEA

#define FIS_TYPE_REG_H2D   0x27

#define SECTOR_SIZE        512
/* One transfer's worth of bounce buffer.  64 KiB = 128 sectors, which is what
 * the block cache asks for at its largest and keeps the allocation to a single
 * 16-frame buddy block. */
#define AHCI_DMA_BYTES     (64 * 1024)
#define AHCI_MAX_SECTORS   (AHCI_DMA_BYTES / SECTOR_SIZE)

/* ---------------------------------------------------------------- */
/* In-memory structures the CONTROLLER reads.  Sizes are fixed by    */
/* the specification, so they are asserted rather than trusted.      */
/* ---------------------------------------------------------------- */

struct ahci_cmd_header {
    uint16_t flags;          /* bits 0-4 CFL (dwords), bit 6 = write */
    uint16_t prdtl;
    volatile uint32_t prdbc; /* bytes transferred, written by the HBA */
    uint32_t ctba, ctbau;
    uint32_t rsv[4];
};

struct ahci_prdt_entry {
    uint32_t dba, dbau;
    uint32_t rsv;
    uint32_t dbc;            /* bits 0-21 = byte count MINUS ONE */
};

struct ahci_cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    struct ahci_prdt_entry prdt[8];
};

/* A compile-time check rather than a comment: these layouts are a contract with
 * hardware, and a padding change would otherwise be found by a wrong transfer. */
_Static_assert(sizeof(struct ahci_cmd_header) == 32, "AHCI command header is 32 bytes");
_Static_assert(sizeof(struct ahci_prdt_entry) == 16, "AHCI PRDT entry is 16 bytes");
_Static_assert(sizeof(struct ahci_cmd_table) == 128 + 8 * 16, "AHCI command table layout");

/* ---------------------------------------------------------------- */
/* Driver state.                                                     */
/* ---------------------------------------------------------------- */

static struct {
    int                 present;
    struct pci_device   pd;
    volatile uint8_t*   abar;
    int                 port;          /* the one port we use */
    uint64_t            sectors;

    struct ahci_cmd_header* clb;       /* command list  (1 KiB aligned) */
    void*                   fis;       /* received FIS  (256 B aligned) */
    struct ahci_cmd_table*  ctab;      /* slot 0's table (128 B aligned) */
    uint64_t                clb_phys, fis_phys, ctab_phys;
    pmm_phys_t              struct_frames;   /* the one allocation above */

    uint8_t*                dma;       /* bounce buffer */
    uint64_t                dma_phys;
} g_ahci;

static struct block_device g_sda;

/* ---- MMIO --------------------------------------------------------------- */

static uint32_t hr32(int off) {
    return *(volatile uint32_t*)(g_ahci.abar + off);
}
static void hw32(int off, uint32_t v) {
    *(volatile uint32_t*)(g_ahci.abar + off) = v;
}
static int poff(int port, int reg) { return PORT_BASE + port * PORT_STRIDE + reg; }
static uint32_t pr32(int port, int reg) { return hr32(poff(port, reg)); }
static void pw32(int port, int reg, uint32_t v) { hw32(poff(port, reg), v); }

/* A bounded wait on a register condition.  EVERY wait here is bounded and says
 * so when it expires: a storage driver that spins forever on a controller that
 * stopped answering does not merely fail to read a sector, it takes down
 * whatever task asked — and at boot that is the boot task. */
static int wait_clear(int port, int reg, uint32_t mask, int ms, const char* what) {
    for (int i = 0; i < ms * 10; i++) {
        if ((pr32(port, reg) & mask) == 0) return 0;
        hal_cpu_pause();
        if ((i % 10) == 9) task_msleep(1);
    }
    kprintf("ahci: timed out waiting for %s on port %d\n", what, port);
    return -1;
}

/* ---- port bring-up ------------------------------------------------------- */

static int port_stop(int port) {
    uint32_t cmd = pr32(port, P_CMD);
    pw32(port, P_CMD, cmd & ~PCMD_ST);
    if (wait_clear(port, P_CMD, PCMD_CR, 500, "CR to clear") != 0) return -1;
    cmd = pr32(port, P_CMD);
    pw32(port, P_CMD, cmd & ~PCMD_FRE);
    if (wait_clear(port, P_CMD, PCMD_FR, 500, "FR to clear") != 0) return -1;
    return 0;
}

static void port_start(int port) {
    /* FRE first, then ST.  The receive engine has to be able to take the reply
     * before a command can be issued; the other order has a window in which the
     * controller answers into an area it has not been told about. */
    pw32(port, P_CMD, pr32(port, P_CMD) | PCMD_FRE);
    pw32(port, P_CMD, pr32(port, P_CMD) | PCMD_ST);
}

/* Allocate the three structures the controller reads.  ONE contiguous
 * allocation, laid out by hand so every alignment requirement is satisfied by
 * construction rather than by three separate allocations that each happen to
 * come back aligned:
 *
 *   +0     command list   1024 bytes, 1 KiB aligned (the frame is 4 KiB aligned)
 *   +1024  received FIS    256 bytes, 256 B aligned
 *   +2048  command table   256 bytes, 128 B aligned
 */
static int alloc_structs(void) {
    pmm_phys_t f = pmm_alloc_contiguous_dma32(1);
    if (!f) { kprintf("ahci: no DMA32 frame for the command list\n"); return -1; }
    g_ahci.struct_frames = f;

    uint8_t* base = (uint8_t*)phys_to_virt((uint64_t)f);
    for (int i = 0; i < 4096; i++) base[i] = 0;

    g_ahci.clb       = (struct ahci_cmd_header*)base;
    g_ahci.clb_phys  = (uint64_t)f;
    g_ahci.fis       = base + 1024;
    g_ahci.fis_phys  = (uint64_t)f + 1024;
    g_ahci.ctab      = (struct ahci_cmd_table*)(base + 2048);
    g_ahci.ctab_phys = (uint64_t)f + 2048;

    pmm_phys_t d = pmm_alloc_contiguous_dma32(AHCI_DMA_BYTES / 4096);
    if (!d) {
        kprintf("ahci: no %d-byte DMA buffer\n", AHCI_DMA_BYTES);
        pmm_free_contiguous(f, 1);
        g_ahci.struct_frames = 0;
        return -1;
    }
    g_ahci.dma      = (uint8_t*)phys_to_virt((uint64_t)d);
    g_ahci.dma_phys = (uint64_t)d;
    return 0;
}

static void free_structs(void) {
    if (g_ahci.struct_frames) {
        pmm_free_contiguous(g_ahci.struct_frames, 1);
        g_ahci.struct_frames = 0;
    }
    if (g_ahci.dma_phys) {
        pmm_free_contiguous((pmm_phys_t)g_ahci.dma_phys, AHCI_DMA_BYTES / 4096);
        g_ahci.dma_phys = 0;
    }
    g_ahci.clb = NULL; g_ahci.fis = NULL; g_ahci.ctab = NULL; g_ahci.dma = NULL;
}

/* ---- issuing one command ------------------------------------------------- */

/* Build slot 0 and run it to completion.  Slot 0 only: this driver is
 * synchronous by design — the block layer above it is — and 32 slots buy
 * nothing until somebody issues overlapping requests.  Saying so is better than
 * a comment claiming a queue depth the code does not have. */
static int run_command(uint8_t ata_cmd, uint64_t lba, uint32_t nsectors,
                       int write, uint32_t bytes) {
    int port = g_ahci.port;

    /* Clear any latched error before the command, so what we read afterwards
     * belongs to THIS command and not to an earlier one. */
    pw32(port, P_SERR, pr32(port, P_SERR));
    pw32(port, P_IS, pr32(port, P_IS));

    if (wait_clear(port, P_TFD, TFD_BSY | TFD_DRQ, 1000, "the device to go idle") != 0)
        return -1;

    struct ahci_cmd_header* h = &g_ahci.clb[0];
    h->flags = (uint16_t)((sizeof(uint32_t) * 5 / sizeof(uint32_t))  /* CFL = 5 dwords */
                          | (write ? (1u << 6) : 0));
    h->prdtl = bytes ? 1 : 0;
    h->prdbc = 0;
    h->ctba  = (uint32_t)g_ahci.ctab_phys;
    h->ctbau = (uint32_t)(g_ahci.ctab_phys >> 32);

    struct ahci_cmd_table* t = g_ahci.ctab;
    for (int i = 0; i < 64; i++) t->cfis[i] = 0;

    t->cfis[0]  = FIS_TYPE_REG_H2D;
    t->cfis[1]  = 0x80;                       /* this is a COMMAND, not a control write */
    t->cfis[2]  = ata_cmd;
    t->cfis[4]  = (uint8_t)(lba);
    t->cfis[5]  = (uint8_t)(lba >> 8);
    t->cfis[6]  = (uint8_t)(lba >> 16);
    t->cfis[7]  = 0x40;                       /* LBA mode */
    t->cfis[8]  = (uint8_t)(lba >> 24);
    t->cfis[9]  = (uint8_t)(lba >> 32);
    t->cfis[10] = (uint8_t)(lba >> 40);
    t->cfis[12] = (uint8_t)(nsectors);
    t->cfis[13] = (uint8_t)(nsectors >> 8);

    if (bytes) {
        t->prdt[0].dba  = (uint32_t)g_ahci.dma_phys;
        t->prdt[0].dbau = (uint32_t)(g_ahci.dma_phys >> 32);
        t->prdt[0].rsv  = 0;
        /* MINUS ONE.  See the file header — an off-by-one here transfers one
         * byte too many and works often enough to look correct. */
        t->prdt[0].dbc  = bytes - 1;
    }

    pw32(port, P_CI, 1u << 0);

    /* Poll.  An interrupt would be better and is deliberately not wired yet:
     * §M55's rule is that a driver should learn its interrupt works by
     * RECEIVING one, and until that is built a poll is correct and merely
     * coarse.  The wait is bounded, so a dead controller costs a timeout. */
    for (int i = 0; i < 30000; i++) {
        if ((pr32(port, P_CI) & 1u) == 0) break;
        if (pr32(port, P_IS) & (1u << 30)) {       /* task file error */
            kprintf("ahci: task file error, tfd=%x serr=%x\n",
                    pr32(port, P_TFD), pr32(port, P_SERR));
            return -1;
        }
        hal_cpu_pause();
        if ((i % 100) == 99) task_msleep(1);
    }
    if (pr32(port, P_CI) & 1u) {
        kprintf("ahci: command %x did not complete\n", ata_cmd);
        return -1;
    }
    if (pr32(port, P_TFD) & TFD_ERR) {
        kprintf("ahci: command %x failed, tfd=%x\n", ata_cmd, pr32(port, P_TFD));
        return -1;
    }
    return 0;
}

/* ---- block_device ops ---------------------------------------------------- */

static int ahci_read(struct block_device* dev, uint64_t lba, uint32_t count, void* buf) {
    (void)dev;
    if (!g_ahci.present || !count) return -1;
    uint8_t* out = (uint8_t*)buf;

    while (count) {
        uint32_t n = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;
        if (run_command(ATA_READ_DMA_EXT, lba, n, 0, n * SECTOR_SIZE) != 0)
            return -1;
        for (uint32_t i = 0; i < n * SECTOR_SIZE; i++) out[i] = g_ahci.dma[i];
        out   += n * SECTOR_SIZE;
        lba   += n;
        count -= n;
    }
    return 0;
}

static int ahci_write(struct block_device* dev, uint64_t lba, uint32_t count,
                      const void* buf) {
    (void)dev;
    if (!g_ahci.present || !count) return -1;
    const uint8_t* in = (const uint8_t*)buf;

    while (count) {
        uint32_t n = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;
        for (uint32_t i = 0; i < n * SECTOR_SIZE; i++) g_ahci.dma[i] = in[i];
        if (run_command(ATA_WRITE_DMA_EXT, lba, n, 1, n * SECTOR_SIZE) != 0)
            return -1;
        in    += n * SECTOR_SIZE;
        lba   += n;
        count -= n;
    }
    return 0;
}

static int ahci_flush(struct block_device* dev) {
    (void)dev;
    if (!g_ahci.present) return -1;
    return run_command(ATA_FLUSH_EXT, 0, 0, 0, 0);
}

/* ---- lifecycle ----------------------------------------------------------- */

/* Class 0x01 subclass 0x06 = SATA; prog-if 0x01 = AHCI 1.0.  MATCHED BY CLASS,
 * not by ID, and that is the whole reason this driver is worth more than a
 * VirtualBox-specific one: Intel, AMD, VMware and VirtualBox all present
 * different vendor IDs and the same programming interface. */
static int ahci_found;
static struct pci_device ahci_pd;

static void ahci_visit(const struct pci_device* d, void* ctx) {
    (void)ctx;
    if (ahci_found) return;
    if (d->class_code == 0x01 && d->subclass == 0x06 && d->prog_if == 0x01) {
        ahci_pd = *d;
        ahci_found = 1;
    }
}

static int ahci_probe(void* ctx) {
    (void)ctx;
    ahci_found = 0;
    pci_scan(ahci_visit, NULL);
    return ahci_found ? 0 : -1;
}

/* Take the controller from the firmware, if it says it has it.  Skipping this
 * on a machine whose BIOS is still driving the controller means two owners
 * issuing commands into two command lists. */
static void bios_handoff(void) {
    if (!(hr32(HBA_CAP2) & CAP2_BOH)) return;
    hw32(HBA_BOHC, hr32(HBA_BOHC) | BOHC_OOS);
    for (int i = 0; i < 2000; i++) {
        uint32_t b = hr32(HBA_BOHC);
        if (!(b & BOHC_BOS) && !(b & BOHC_BB)) return;
        task_msleep(1);
    }
    kprintf("ahci: firmware did not hand the controller over (BOHC=%x)\n",
            hr32(HBA_BOHC));
}

static int ahci_init(void* ctx) {
    (void)ctx;
    if (!ahci_found && ahci_probe(NULL) != 0) return -1;
    g_ahci.pd = ahci_pd;

    uint16_t cmd = pci_read16(ahci_pd.bus, ahci_pd.slot, ahci_pd.func, PCI_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    pci_write16(ahci_pd.bus, ahci_pd.slot, ahci_pd.func, PCI_COMMAND, cmd);

    /* ABAR is BAR5, always — the one BAR whose index the specification fixes. */
    uint32_t bar5 = ahci_pd.bar[5];
    if (bar5 & 0x1) { kprintf("ahci: BAR5 is I/O space (%x)?\n", bar5); return -1; }
    uint32_t abar = bar5 & ~0xFu;
    if (!abar) { kprintf("ahci: BAR5 is not programmed\n"); return -1; }

    /* Map it uncached.  §M33 stage 5 paid for the second half of that: a device
     * register read through a cached mapping returns whatever the CPU cached
     * the first time and silently stops tracking the hardware. */
    uint32_t aligned = abar & 0xFFC00000u;
    vmm_map_4mib(aligned, aligned, VMM_WRITABLE | VMM_CACHE_DIS);
    g_ahci.abar = (volatile uint8_t*)(uintptr_t)abar;

    bios_handoff();
    hw32(HBA_GHC, hr32(HBA_GHC) | GHC_AE);

    uint32_t cap = hr32(HBA_CAP);
    uint32_t pi  = hr32(HBA_PI);
    int nports   = (int)((cap & 0x1F) + 1);
    int nslots   = (int)(((cap >> 8) & 0x1F) + 1);

    kprintf("ahci: controller at %x:%x.%x, ABAR %x, %d port(s), %d slot(s), "
            "AHCI %x\n",
            ahci_pd.bus, ahci_pd.slot, ahci_pd.func, abar, nports, nslots,
            hr32(HBA_VS));

    /* Find the first port with an ATA device on it.  ATAPI is REFUSED by name
     * rather than attempted: a CD-ROM speaks packet commands, its "sectors" are
     * 2048 bytes, and driving it as if it were a disk would produce a mount
     * that fails in a way nothing explains. */
    int found = -1;
    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;
        uint32_t ssts = pr32(p, P_SSTS);
        if ((ssts & SSTS_DET_MASK) != SSTS_DET_READY) continue;
        uint32_t sig = pr32(p, P_SIG);
        if (sig == SIG_ATAPI) {
            kprintf("ahci: port %d holds an ATAPI device — not handled\n", p);
            continue;
        }
        if (sig != SIG_ATA) {
            kprintf("ahci: port %d has signature %x — not a disk\n", p, sig);
            continue;
        }
        found = p;
        break;
    }
    if (found < 0) {
        kprintf("ahci: no SATA disk attached to any implemented port\n");
        return -1;
    }
    g_ahci.port = found;

    if (alloc_structs() != 0) return -1;
    if (port_stop(found) != 0) { free_structs(); return -1; }

    pw32(found, P_CLB,  (uint32_t)g_ahci.clb_phys);
    pw32(found, P_CLBU, (uint32_t)(g_ahci.clb_phys >> 32));
    pw32(found, P_FB,   (uint32_t)g_ahci.fis_phys);
    pw32(found, P_FBU,  (uint32_t)(g_ahci.fis_phys >> 32));
    pw32(found, P_SERR, pr32(found, P_SERR));      /* clear latched errors */
    pw32(found, P_IE,   0);                        /* polled; see run_command */
    port_start(found);

    /* IDENTIFY, for the capacity.  Reading it is not optional: without it the
     * only honest sector count is "unknown", and a block device that lies about
     * its size lets the filesystem write past the end of the disk. */
    if (run_command(ATA_IDENTIFY, 0, 0, 0, 512) != 0) {
        kprintf("ahci: IDENTIFY failed on port %d\n", found);
        free_structs();
        return -1;
    }
    {
        const uint16_t* id = (const uint16_t*)g_ahci.dma;
        /* Words 100-103 are the 48-bit LBA count.  Word 60-61 is the older
         * 28-bit one; preferring the 48-bit field and falling back is what
         * makes this work on both a modern disk and an old one. */
        uint64_t s48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16)
                     | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
        uint32_t s28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
        g_ahci.sectors = s48 ? s48 : (uint64_t)s28;
    }
    if (!g_ahci.sectors) {
        kprintf("ahci: the disk reports zero sectors — refusing to register it\n");
        free_structs();
        return -1;
    }

    g_ahci.present = 1;

    g_sda.name         = "sda";
    g_sda.sector_size  = SECTOR_SIZE;
    g_sda.sector_count = g_ahci.sectors;
    g_sda.read         = ahci_read;
    g_sda.write        = ahci_write;
    g_sda.flush        = ahci_flush;
    g_sda.priv         = &g_ahci;
    if (blk_register(&g_sda) != 0) {
        kprintf("ahci: blk_register refused 'sda'\n");
        g_ahci.present = 0;
        free_structs();
        return -1;
    }

    kprintf("ahci: sda registered on port %d, %u sectors (~%u MiB)\n",
            found, (unsigned)g_ahci.sectors,
            (unsigned)(g_ahci.sectors / 2048));
    klog(KLOG_INFO, "ahci", "SATA disk on port %d, %u sectors\n",
         found, (unsigned)g_ahci.sectors);
    return 0;
}

static int ahci_shutdown(void* ctx) {
    (void)ctx;
    if (!g_ahci.present) return 0;
    /* Stop the engine BEFORE the memory it is reading goes back to the
     * allocator — the same ordering §M33 had to learn for the IOMMU domain, and
     * for the same reason: a device pointed at freed memory is worse than a
     * device pointed at nothing. */
    port_stop(g_ahci.port);
    g_ahci.present = 0;
    free_structs();
    kprintf("ahci: stopped\n");
    return 0;
}

static const struct driver_ops ahci_ops = {
    .probe    = ahci_probe,
    .init     = ahci_init,
    .shutdown = ahci_shutdown,
};

DRIVER_MATCH(m_ahci) = { .driver = "ahci", .name = "SATA controller (AHCI)",
                         .vendor = HW_ANY_VENDOR, .cls = 0x01, .sub = 0x06 };

/* DMA-capable: the controller reads the command list and writes the data buffer
 * by physical address.  Not boot-critical — the machine comes up without a disk
 * and says so. */
DRIVER_EX(ahci, "block", &ahci_ops, NULL, DOMAIN_KERNEL, DRVF_DMA);
