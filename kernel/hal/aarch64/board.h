/* =============================================================================
 * board.h — what THIS aarch64 machine looks like (§M85 stage 1, 2026-09-26).
 *
 * The ARM port was written against QEMU `virt`'s memory map as constants: the
 * GIC at 0x0800_0000, the PL011 at 0x0900_0000 on SPI 1, thirty-two
 * virtio-mmio slots from 0x0a00_0000 on SPI 16+n, the PCIe ECAM at
 * 0x40_1000_0000...  Correct for exactly one board.  Every one of them is now a
 * FIELD here, filled from the device tree (dtb.c) — or ACPI, once a firmware
 * that describes the machine that way boots us (stage 3) — and the drivers ask
 * this file instead of carrying their own copy of the map.
 *
 * The `virt` values remain as DEFAULTS, used only when no description was
 * found, and every field says which source it came from (`board` command):
 * "the machine told us" and "we assumed" must not look the same in a report,
 * or a wrong assumption on a new board reads as a fact about it.
 *
 * Arch-local on purpose: nothing outside kernel/hal/aarch64 may know that an
 * ARM board has a GIC or a PL011 — the portable layers see IRQ numbers and
 * console sinks, as before.
 * ============================================================================= */
#ifndef DOS_AARCH64_BOARD_H
#define DOS_AARCH64_BOARD_H

#include <stdint.h>

#define BOARD_MAX_VIRTIO 32

enum board_src { BOARD_DEFAULT = 0, BOARD_DTB = 1, BOARD_ACPI = 2 };

struct board {
    /* Interrupt controller. */
    int       gic_version;            /* 2 or 3                                 */
    uint64_t  gicd;                   /* distributor                            */
    uint64_t  gicc;                   /* v2: CPU interface                      */
    uint64_t  gicr;                   /* v3: first redistributor frame          */
    uint64_t  gicr_size;              /* v3: size of the redistributor region   */
    uint32_t  timer_intid;            /* EL1 non-secure physical timer (a PPI)  */

    /* Console + clock. */
    uint64_t  uart;                   /* PL011 base                             */
    uint32_t  uart_intid;
    uint64_t  rtc;                    /* PL031 base (0 = none)                  */

    /* virtio-mmio transports, in ascending address order (so "slot n" means
     * the same thing it meant when it was base + n * 0x200 on `virt`). */
    int       nvirtio;
    uint64_t  virtio_base[BOARD_MAX_VIRTIO];
    uint32_t  virtio_intid[BOARD_MAX_VIRTIO];

    /* PCIe (generic ECAM host). */
    uint64_t  ecam;                   /* 0 = no PCIe                            */
    uint64_t  ecam_size;
    uint64_t  pci_mmio32;             /* 32-bit memory window for BARs          */
    uint64_t  pci_mmio32_size;

    const char* model;                /* the tree's /model, if any              */
    enum board_src src;               /* where the fields above came from       */
};

extern struct board g_board;

/* Accessors the drivers use (keep the struct itself out of their business). */
static inline int      board_virtio_count(void)   { return g_board.nvirtio; }
static inline uint64_t board_virtio_base(int i)   { return g_board.virtio_base[i]; }
static inline uint32_t board_virtio_intid(int i)  { return g_board.virtio_intid[i]; }

/* Called by dtb.c (and later the ACPI walker) before any device is touched;
 * fills in the `virt` defaults for whatever the description did not name. */
void board_finish(void);

#endif /* DOS_AARCH64_BOARD_H */
