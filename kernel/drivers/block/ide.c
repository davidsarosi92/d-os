/* =============================================================================
 * ide.c — legacy ATA (IDE) disks, the controller every machine still has.
 *
 * -----------------------------------------------------------------------------
 * WHY THIS EXISTS ALONGSIDE AHCI
 *
 * AHCI covers modern SATA.  IDE covers the case AHCI cannot:
 *
 *   * **VirtualBox's DEFAULT controller is IDE (PIIX4).**  A guest there gets
 *     an IDE disk unless somebody goes and changes it, so an AHCI-only system
 *     boots with no storage on the hypervisor this work is aimed at.
 *   * QEMU's i440fx machine — the default, and what `run_qemu.sh` boots — has a
 *     PIIX3 IDE controller and no AHCI at all.
 *   * Old hardware, and every CD-ROM ever attached to a PC.
 *
 * The device manager named it before this existed: `Intel PIIX3 IDE controller
 * 0:1.1 8086:7010 (none) needs a driver`.
 *
 * -----------------------------------------------------------------------------
 * PIO, DELIBERATELY, AND WHAT THAT COSTS
 *
 * This driver moves data with `insw`/`outsw` through the data port — 256 words
 * per sector, by the CPU, one sector at a time.  Bus-master DMA would be
 * faster and is NOT built, for a reason worth stating rather than leaving as an
 * omission: BMDMA needs a PRD table, a second BAR, and a completion path, and
 * its whole benefit is throughput on a device that is here as a COMPATIBILITY
 * fallback.  A correct slow driver that works on every machine is worth more
 * than a fast one that works on the machines AHCI already covers.
 *
 * The cost is real and measured in the log: a PIO read is a busy CPU for the
 * duration of the transfer.  It is bounded per sector and the driver yields
 * between sectors, so a slow disk costs throughput rather than a wedged task.
 *
 * -----------------------------------------------------------------------------
 * THREE THINGS THAT BITE
 *
 * 1. **THE 400 ns SETTLE AFTER A DRIVE SELECT.**  The status register does not
 *    become valid immediately after writing the drive/head register, and the
 *    canonical way to wait is four reads of the ALTERNATE status port (which,
 *    unlike the primary one, does not clear the interrupt).  Skipping it means
 *    reading the PREVIOUS drive's status and concluding the wrong thing about
 *    which disks exist.
 *
 * 2. **A FLOATING BUS READS AS 0xFF.**  An absent controller returns all-ones
 *    from every port, which passes any "is the busy bit clear" test.  The
 *    presence check therefore looks for a status that is neither 0x00 nor 0xFF
 *    before believing anything else.
 *
 * 3. **ATAPI IS DETECTED AND REFUSED BY NAME.**  A CD-ROM answers IDENTIFY with
 *    an ABORT and then puts a signature in the LBA-mid/high registers.  This
 *    matters more here than it did for AHCI: on every one of our test boots the
 *    d-os ISO is an ATAPI device on this very controller, so a driver that took
 *    it for a disk would try to mount the CD as exFAT with 512-byte sectors.
 * ============================================================================= */

#include "driver.h"
#include "hwdev.h"
#include "block.h"
#include "pci.h"
#include "printf.h"
#include "klog.h"
#include "task.h"
#include "hal_api.h"
#include "hal.h"   /* inb/outb/inw/outw — this driver IS port I/O */
#include <stddef.h>
#include <stdint.h>

/* Legacy port bases.  These are fixed by 30 years of convention, not read from
 * the BARs: a PIIX-class controller in "compatibility mode" — which is how
 * every one of them comes up — decodes exactly these and reports BAR values
 * that are zero or meaningless. */
#define IDE_PRIMARY_IO     0x1F0
#define IDE_PRIMARY_CTRL   0x3F6
#define IDE_SECONDARY_IO   0x170
#define IDE_SECONDARY_CTRL 0x376

#define R_DATA      0
#define R_FEATURES  1
#define R_ERROR     1
#define R_SECCOUNT  2
#define R_LBA0      3
#define R_LBA1      4
#define R_LBA2      5
#define R_DRIVE     6
#define R_COMMAND   7
#define R_STATUS    7

#define ST_ERR      0x01
#define ST_DRQ      0x08
#define ST_DF       0x20
#define ST_DRDY     0x40
#define ST_BSY      0x80

#define CMD_READ_PIO       0x20
#define CMD_READ_PIO_EXT   0x24
#define CMD_WRITE_PIO      0x30
#define CMD_WRITE_PIO_EXT  0x34
#define CMD_FLUSH          0xE7
#define CMD_FLUSH_EXT      0xEA
#define CMD_IDENTIFY       0xEC

#define SECTOR_SIZE 512
/* Two channels, two drives each — the shape of the bus, and the reason the
 * loop below is a nested pair rather than a list. */
#define IDE_MAX_DISKS 4

struct ide_disk {
    int      used;
    uint16_t io, ctrl;
    int      slave;              /* 0 = master, 1 = slave */
    int      lba48;
    uint64_t sectors;
    char     name[8];            /* "hda".."hdd" */
    struct block_device blk;
};

static struct ide_disk g_disks[IDE_MAX_DISKS];
static int g_ndisks;
static int g_have_pci;
static struct pci_device g_pd;

/* ---- low level ---------------------------------------------------------- */

/* The 400 ns settle, done the canonical way: four reads of the ALTERNATE status
 * port.  It must be the alternate one — reading the primary status register
 * acknowledges the interrupt, which is a side effect this has no business
 * having. */
static void settle(struct ide_disk* d) {
    for (int i = 0; i < 4; i++) (void)inb(d->ctrl);
}

static int wait_not_busy(struct ide_disk* d, int ms) {
    for (int i = 0; i < ms * 100; i++) {
        uint8_t st = inb(d->io + R_STATUS);
        if (st == 0xFF) return -1;                 /* floating bus: nothing here */
        if (!(st & ST_BSY)) return 0;
        hal_cpu_pause();
        if ((i % 100) == 99) task_msleep(1);
    }
    return -1;
}

/* Wait for DRQ (the device wants a data transfer) or an error.  Returns 0 when
 * data may move, -1 otherwise — and reports WHY, because "the read failed" and
 * "the device reported a fault" send somebody to different places. */
static int wait_drq(struct ide_disk* d, int ms) {
    for (int i = 0; i < ms * 100; i++) {
        uint8_t st = inb(d->io + R_STATUS);
        if (st == 0xFF) return -1;
        if (st & (ST_ERR | ST_DF)) {
            kprintf("ide: %s reported an error (status %x, error %x)\n",
                    d->name, st, inb(d->io + R_ERROR));
            return -1;
        }
        if (!(st & ST_BSY) && (st & ST_DRQ)) return 0;
        hal_cpu_pause();
        if ((i % 100) == 99) task_msleep(1);
    }
    kprintf("ide: %s timed out waiting for data\n", d->name);
    return -1;
}

static void select_drive(struct ide_disk* d, uint8_t head_bits) {
    outb(d->io + R_DRIVE, (uint8_t)(0xE0 | (d->slave << 4) | (head_bits & 0x0F)));
    settle(d);
}

/* ---- one transfer -------------------------------------------------------- */

/* A single sector in or out.  ONE SECTOR PER COMMAND, deliberately: multi-sector
 * PIO needs the device's reported block size and an interrupt per block, and
 * getting that wrong transfers the right number of bytes into the wrong place.
 * The loop above pays a command per sector and is correct on every device. */
static int rw_one(struct ide_disk* d, uint64_t lba, void* buf, int write) {
    if (wait_not_busy(d, 1000) != 0) return -1;

    if (d->lba48) {
        select_drive(d, 0);
        outb(d->io + R_SECCOUNT, 0);                      /* count high */
        outb(d->io + R_LBA0, (uint8_t)(lba >> 24));
        outb(d->io + R_LBA1, (uint8_t)(lba >> 32));
        outb(d->io + R_LBA2, (uint8_t)(lba >> 40));
        outb(d->io + R_SECCOUNT, 1);                      /* count low  */
        outb(d->io + R_LBA0, (uint8_t)(lba));
        outb(d->io + R_LBA1, (uint8_t)(lba >> 8));
        outb(d->io + R_LBA2, (uint8_t)(lba >> 16));
        outb(d->io + R_COMMAND, write ? CMD_WRITE_PIO_EXT : CMD_READ_PIO_EXT);
    } else {
        /* LBA28 puts the top four address bits in the DRIVE register itself —
         * the one place in this interface where an address and a device
         * selection share a byte. */
        select_drive(d, (uint8_t)((lba >> 24) & 0x0F));
        outb(d->io + R_SECCOUNT, 1);
        outb(d->io + R_LBA0, (uint8_t)(lba));
        outb(d->io + R_LBA1, (uint8_t)(lba >> 8));
        outb(d->io + R_LBA2, (uint8_t)(lba >> 16));
        outb(d->io + R_COMMAND, write ? CMD_WRITE_PIO : CMD_READ_PIO);
    }

    if (wait_drq(d, 3000) != 0) return -1;

    uint16_t* w = (uint16_t*)buf;
    if (write) {
        for (int i = 0; i < SECTOR_SIZE / 2; i++) outw(d->io + R_DATA, w[i]);
        /* A write is not on the medium until the cache is flushed, and the
         * flush is what makes "the file survived a reboot" true rather than
         * likely. */
        if (wait_not_busy(d, 1000) != 0) return -1;
        outb(d->io + R_COMMAND, d->lba48 ? CMD_FLUSH_EXT : CMD_FLUSH);
        if (wait_not_busy(d, 3000) != 0) return -1;
    } else {
        for (int i = 0; i < SECTOR_SIZE / 2; i++) w[i] = inw(d->io + R_DATA);
    }
    return 0;
}

static int ide_read(struct block_device* dev, uint64_t lba, uint32_t count, void* buf) {
    struct ide_disk* d = (struct ide_disk*)dev->priv;
    uint8_t* p = (uint8_t*)buf;
    for (uint32_t i = 0; i < count; i++) {
        if (rw_one(d, lba + i, p + (uint64_t)i * SECTOR_SIZE, 0) != 0) return -1;
        /* Yield between sectors.  A PIO transfer is the CPU doing the copying,
         * so a large read would otherwise hold a core for its whole duration —
         * §M31's lesson, where provisioning that never yielded made a healthy
         * machine look wedged to the watchdog. */
        if ((i & 0x1F) == 0x1F) task_msleep(0);
    }
    return 0;
}

static int ide_write(struct block_device* dev, uint64_t lba, uint32_t count,
                     const void* buf) {
    struct ide_disk* d = (struct ide_disk*)dev->priv;
    uint8_t* p = (uint8_t*)(uintptr_t)buf;
    for (uint32_t i = 0; i < count; i++) {
        if (rw_one(d, lba + i, p + (uint64_t)i * SECTOR_SIZE, 1) != 0) return -1;
        if ((i & 0x1F) == 0x1F) task_msleep(0);
    }
    return 0;
}

static int ide_flush(struct block_device* dev) {
    struct ide_disk* d = (struct ide_disk*)dev->priv;
    if (wait_not_busy(d, 1000) != 0) return -1;
    select_drive(d, 0);
    outb(d->io + R_COMMAND, d->lba48 ? CMD_FLUSH_EXT : CMD_FLUSH);
    return wait_not_busy(d, 3000);
}

/* ---- detection ----------------------------------------------------------- */

/* Probe one (channel, drive) position.  Returns 1 when a usable ATA disk is
 * there and has been filled into `d`. */
static int identify(struct ide_disk* d) {
    select_drive(d, 0);
    outb(d->io + R_SECCOUNT, 0);
    outb(d->io + R_LBA0, 0);
    outb(d->io + R_LBA1, 0);
    outb(d->io + R_LBA2, 0);
    outb(d->io + R_COMMAND, CMD_IDENTIFY);
    settle(d);

    uint8_t st = inb(d->io + R_STATUS);
    /* 0x00 means nothing is attached; 0xFF means the whole channel is a
     * floating bus.  Both are "no device", and neither is an error worth
     * printing on every boot of every machine. */
    if (st == 0 || st == 0xFF) return 0;

    if (wait_not_busy(d, 1000) != 0) return 0;

    /* ATAPI ANNOUNCES ITSELF HERE.  A packet device aborts IDENTIFY and leaves
     * 0x14/0xEB (or 0x69/0x96) in LBA-mid/high.  This is not a corner case in
     * this project: every test boot has the d-os ISO on this controller, so a
     * driver that missed it would try to mount a CD-ROM as exFAT. */
    uint8_t mid = inb(d->io + R_LBA1), hi = inb(d->io + R_LBA2);
    if ((mid == 0x14 && hi == 0xEB) || (mid == 0x69 && hi == 0x96)) {
        kprintf("ide: %s is an ATAPI device (CD/DVD) — not handled\n", d->name);
        return 0;
    }
    if (mid || hi) return 0;             /* SATA or something else entirely */

    /* AN EMPTY SLOT ABORTS IDENTIFY, and that is the NORMAL answer rather than
     * a fault.  A channel with one disk on it does not float — the present
     * drive drives the bus for both — so the absent one answers with
     * ERR|DRDY and ABRT in the error register.  The first version of this
     * driver called wait_drq here, which PRINTS, so every ordinary boot of
     * every machine with a single IDE disk reported an error for the empty
     * slot next to it.  *A driver that reports the normal case as a fault
     * teaches people to ignore its output.* */
    {
        uint8_t s2 = inb(d->io + R_STATUS);
        if (s2 & (ST_ERR | ST_DF)) return 0;         /* nothing in this slot */
        if (!(s2 & ST_DRQ)) return 0;
    }

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(d->io + R_DATA);

    /* Word 83 bit 10 says the device supports the 48-bit commands.  Asking
     * rather than inferring from the capacity matters: a small disk can support
     * LBA48 and a driver that decided from the size would send the wrong
     * command to it. */
    d->lba48 = (id[83] & (1u << 10)) ? 1 : 0;
    uint64_t s48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16)
                 | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    uint32_t s28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    d->sectors = (d->lba48 && s48) ? s48 : (uint64_t)s28;
    if (!d->sectors) return 0;
    return 1;
}

/* ---- lifecycle ----------------------------------------------------------- */

static int ide_probe(void* ctx) {
    (void)ctx;
    /* A PCI IDE controller is the normal case and is not required: the ports
     * are legacy and answer on machines that predate PCI enumeration of them.
     * Finding the PCI device is therefore a nicety for the report, and the
     * PROBE is a port read — which is also what makes this work if the
     * controller is hiding behind an unexpected ID. */
    g_have_pci = 0;
    {
        struct pci_device pd;
        if (pci_find_device(0x8086, 0x7010, &pd) == 0 ||   /* PIIX3 */
            pci_find_device(0x8086, 0x7111, &pd) == 0) {   /* PIIX4 */
            g_pd = pd;
            g_have_pci = 1;
        }
    }
    uint8_t st = inb(IDE_PRIMARY_IO + R_STATUS);
    if (st != 0xFF && st != 0x00) return 0;
    st = inb(IDE_SECONDARY_IO + R_STATUS);
    if (st != 0xFF && st != 0x00) return 0;
    return g_have_pci ? 0 : -1;
}

static int ide_init(void* ctx) {
    (void)ctx;
    if (g_have_pci) {
        /* I/O space only.  NO BUS MASTER: this driver does PIO, and enabling
         * bus mastering for a device that will never DMA hands out a capability
         * for nothing — the opposite of what §M33 spent a milestone on. */
        uint16_t cmd = pci_read16(g_pd.bus, g_pd.slot, g_pd.func, PCI_COMMAND);
        cmd |= PCI_CMD_IO_SPACE;
        pci_write16(g_pd.bus, g_pd.slot, g_pd.func, PCI_COMMAND, cmd);
    }

    static const uint16_t io[2]   = { IDE_PRIMARY_IO,   IDE_SECONDARY_IO };
    static const uint16_t ctrl[2] = { IDE_PRIMARY_CTRL, IDE_SECONDARY_CTRL };

    g_ndisks = 0;
    for (int ch = 0; ch < 2; ch++) {
        for (int dr = 0; dr < 2; dr++) {
            if (g_ndisks >= IDE_MAX_DISKS) break;
            struct ide_disk* d = &g_disks[g_ndisks];
            d->io    = io[ch];
            d->ctrl  = ctrl[ch];
            d->slave = dr;
            /* hda..hdd, the traditional names — and deliberately NOT sda,
             * which AHCI already uses.  Two drivers registering one name is a
             * collision the block layer would refuse, and the refusal would
             * read as "the disk is missing". */
            d->name[0] = 'h'; d->name[1] = 'd';
            d->name[2] = (char)('a' + ch * 2 + dr); d->name[3] = 0;

            if (!identify(d)) continue;

            d->used = 1;
            d->blk.name         = d->name;
            d->blk.sector_size  = SECTOR_SIZE;
            d->blk.sector_count = d->sectors;
            d->blk.read         = ide_read;
            d->blk.write        = ide_write;
            d->blk.flush        = ide_flush;
            d->blk.priv         = d;
            if (blk_register(&d->blk) != 0) {
                kprintf("ide: blk_register refused '%s'\n", d->name);
                d->used = 0;
                continue;
            }
            kprintf("ide: %s on %s %s, %u sectors (~%u MiB), LBA%d\n",
                    d->name, ch ? "secondary" : "primary",
                    dr ? "slave" : "master",
                    (unsigned)d->sectors, (unsigned)(d->sectors / 2048),
                    d->lba48 ? 48 : 28);
            g_ndisks++;
        }
    }

    if (!g_ndisks) {
        kprintf("ide: controller present, no ATA disk attached\n");
        return -1;
    }
    klog(KLOG_INFO, "ide", "%d ATA disk(s) on the legacy ports\n", g_ndisks);
    return 0;
}

static int ide_shutdown(void* ctx) {
    (void)ctx;
    for (int i = 0; i < g_ndisks; i++)
        if (g_disks[i].used) ide_flush(&g_disks[i].blk);
    g_ndisks = 0;
    return 0;
}

static const struct driver_ops ide_ops = {
    .probe    = ide_probe,
    .init     = ide_init,
    .shutdown = ide_shutdown,
};

/* Two IDs and a class match: the PIIX3 and PIIX4 controllers by name (QEMU's
 * i440fx and VirtualBox's default), plus anything else presenting the legacy
 * IDE programming interface. */
DRIVER_MATCH(m_ide_piix3) = { .driver = "ide", .name = "IDE controller (PIIX3)",
                              .vendor = 0x8086, .device = 0x7010 };
DRIVER_MATCH(m_ide_piix4) = { .driver = "ide", .name = "IDE controller (PIIX4)",
                              .vendor = 0x8086, .device = 0x7111 };
DRIVER_MATCH(m_ide_class) = { .driver = "ide", .name = "IDE controller",
                              .vendor = HW_ANY_VENDOR, .cls = 0x01, .sub = 0x01 };

/* NOT DRVF_DMA: this driver moves every byte with the CPU.  Saying so is not
 * pedantry — `drv domain` reads that flag to decide whether a placement outside
 * the kernel would be isolation or theatre, and a driver that claimed DMA it
 * does not do would be reported as less isolated than it is. */
DRIVER_EX(ide, "block", &ide_ops, NULL, DOMAIN_KERNEL, 0);
