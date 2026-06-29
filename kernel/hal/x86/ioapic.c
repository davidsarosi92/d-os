/* =============================================================================
 * ioapic.c — I/O APIC driver (M18).
 *
 * MMIO interface is tiny: two registers.
 *
 *   offset 0x00 IOREGSEL  — write the index of the register you want
 *   offset 0x10 IOWIN     — read/write the selected register's data
 *
 * Registers we use:
 *
 *   0x00 IOAPIC ID
 *   0x01 IOAPIC Version    bits 16..23 = max redirection entry count - 1
 *   0x10..0x3F Redirection Table — 24 (typical) entries × 2 dwords each
 *
 * Each redirection table entry is 64 bits, accessed as two 32-bit
 * regs: 0x10 + 2N (low) and 0x10 + 2N + 1 (high).  Layout (Intel
 * SDM Vol 3 §10.5.3):
 *
 *   bits 0..7    Vector (IDT vector to deliver)
 *   bits 8..10   Delivery Mode  (0=Fixed, 4=NMI, 5=INIT, 7=ExtINT)
 *   bit  11      Destination Mode (0=Physical APIC ID, 1=Logical)
 *   bit  12      Delivery Status (RO)
 *   bit  13      Pin Polarity (0=Active High, 1=Active Low)
 *   bit  14      Remote IRR (RO, level-triggered)
 *   bit  15      Trigger Mode (0=Edge, 1=Level)
 *   bit  16      Mask (1=masked)
 *   bits 56..63  Destination (APIC ID in physical mode)
 *
 * ACPI Interrupt Source Overrides ("ISO" — MADT entry type 2) tell us
 * when the firmware remapped legacy ISA IRQ N to a different GSI;
 * IRQ0 routed to GSI 2 is by far the most common case on QEMU.
 * ============================================================================= */

#include "ioapic.h"
#include "acpi.h"
#include "vmm.h"
#include "hal.h"
#include "printf.h"
#include <stdint.h>

#define IOAPIC_OFF_IOREGSEL 0x00
#define IOAPIC_OFF_IOWIN    0x10

#define IOAPIC_REG_ID       0x00
#define IOAPIC_REG_VERSION  0x01
#define IOAPIC_REG_REDIR(n) (0x10 + 2 * (n))    /* low; high = +1 */

#define IOAPIC_RTE_MASK     (1u << 16)
#define IOAPIC_RTE_LEVEL    (1u << 15)
#define IOAPIC_RTE_LOW_POL  (1u << 13)

static volatile uint8_t* g_ioapic_mmio = 0;
static uint32_t          g_gsi_base    = 0;
static int               g_max_entries = 0;

/* Read/write an indirect IOAPIC register via IOREGSEL/IOWIN. */
static uint32_t io_r(uint32_t reg) {
    *(volatile uint32_t*)(g_ioapic_mmio + IOAPIC_OFF_IOREGSEL) = reg;
    return *(volatile uint32_t*)(g_ioapic_mmio + IOAPIC_OFF_IOWIN);
}
static void io_w(uint32_t reg, uint32_t v) {
    *(volatile uint32_t*)(g_ioapic_mmio + IOAPIC_OFF_IOREGSEL) = reg;
    *(volatile uint32_t*)(g_ioapic_mmio + IOAPIC_OFF_IOWIN) = v;
}

int ioapic_init(uintptr_t phys, uint32_t gsi_base) {
    uintptr_t aligned = phys & ~((uintptr_t)0xFFFu);
    if (vmm_map(aligned, aligned, VMM_WRITABLE | VMM_CACHE_DIS) != 0) {
        /* Probably already mapped — proceed. */
    }
    g_ioapic_mmio = (volatile uint8_t*)phys;
    g_gsi_base    = gsi_base;

    uint32_t ver = io_r(IOAPIC_REG_VERSION);
    /* Max redirection entries is bits 16..23 of the version register,
     * stored as (count - 1).  Most QEMU IOAPICs report 23 (= 24 entries). */
    g_max_entries = ((ver >> 16) & 0xFF) + 1;

    /* Start everything masked so nothing fires until ioapic_route_isa
     * explicitly enables it. */
    for (int i = 0; i < g_max_entries; i++) {
        io_w(IOAPIC_REG_REDIR(i),     IOAPIC_RTE_MASK);
        io_w(IOAPIC_REG_REDIR(i) + 1, 0);
    }

    kprintf("ioapic: %d entries at %p, gsi_base=%u\n",
            g_max_entries, (void*)phys, gsi_base);
    return 0;
}

int ioapic_route_isa(int isa_irq, uint8_t vector, uint8_t dest_apic_id) {
    /* Look up ACPI ISO override if present.  Default is identity
     * mapping (legacy ISA IRQ N → GSI N), so absent any override we
     * just use isa_irq as the GSI index. */
    uint32_t gsi = (uint32_t)isa_irq;
    uint16_t flags = 0;
    acpi_irq_override(isa_irq, &gsi, &flags);

    if (gsi < g_gsi_base) return -1;
    int idx = (int)(gsi - g_gsi_base);
    if (idx < 0 || idx >= g_max_entries) return -1;

    /* MADT ISO flag bits 0..1 = polarity (00=bus default, 01=active hi,
     * 11=active lo), bits 2..3 = trigger (00=bus default, 01=edge,
     * 11=level).  Bus default for ISA = edge + active high. */
    uint32_t low = vector;                              /* DM=fixed, dest mode=physical */
    if ((flags & 0x3) == 0x3) low |= IOAPIC_RTE_LOW_POL;
    if (((flags >> 2) & 0x3) == 0x3) low |= IOAPIC_RTE_LEVEL;

    uint32_t high = ((uint32_t)dest_apic_id) << 24;

    /* Always write high BEFORE low — low contains the mask bit, and we
     * want destination latched before we unmask. */
    io_w(IOAPIC_REG_REDIR(idx) + 1, high);
    io_w(IOAPIC_REG_REDIR(idx),     low);                /* mask=0 implicitly */
    return 0;
}

void ioapic_mask_gsi(uint32_t gsi, int masked) {
    if (gsi < g_gsi_base) return;
    int idx = (int)(gsi - g_gsi_base);
    if (idx < 0 || idx >= g_max_entries) return;
    uint32_t low = io_r(IOAPIC_REG_REDIR(idx));
    if (masked) low |=  IOAPIC_RTE_MASK;
    else        low &= ~IOAPIC_RTE_MASK;
    io_w(IOAPIC_REG_REDIR(idx), low);
}
