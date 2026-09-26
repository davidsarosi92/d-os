/* =============================================================================
 * board.c — the aarch64 machine description, its defaults and its report
 * (§M85 stage 1).  See board.h for why this exists.
 * ============================================================================= */

#include "board.h"
#include "printf.h"
#include "shellcmd.h"

struct board g_board;                 /* zero until dtb.c / ACPI fill it   */

/* QEMU `virt` — the board every constant in this port used to assume.  Used
 * ONLY for fields no description supplied; `board` says so per field. */
#define VIRT_GICD        0x08000000ULL
#define VIRT_GICC        0x08010000ULL
#define VIRT_UART        0x09000000ULL
#define VIRT_UART_INTID  33               /* SPI 1                               */
#define VIRT_RTC         0x09010000ULL
#define VIRT_VIRTIO      0x0a000000ULL    /* 32 slots, 0x200 apart, SPI 16 + n   */
#define VIRT_ECAM        0x4010000000ULL
#define VIRT_ECAM_SIZE   0x10000000ULL
#define VIRT_MMIO32      0x10000000ULL
#define VIRT_MMIO32_SIZE 0x2eff0000ULL
#define VIRT_TIMER_INTID 30               /* PPI 14                              */

/* Which fields were defaulted — a bit per field, for the report. */
static uint32_t g_defaulted;
enum { D_GIC = 1, D_UART = 2, D_RTC = 4, D_VIRTIO = 8, D_PCI = 16, D_TIMER = 32 };

void board_finish(void) {
    if (!g_board.gicd) {
        g_board.gic_version = 2; g_board.gicd = VIRT_GICD; g_board.gicc = VIRT_GICC;
        g_defaulted |= D_GIC;
    }
    if (!g_board.timer_intid) { g_board.timer_intid = VIRT_TIMER_INTID; g_defaulted |= D_TIMER; }
    if (!g_board.uart) {
        g_board.uart = VIRT_UART; g_board.uart_intid = VIRT_UART_INTID; g_defaulted |= D_UART;
    }
    /* No description at all → assume `virt`'s RTC.  WITH a description that
     * names none, there is none: a default in that case would have a driver
     * read a register at an address the board may use for something else. */
    if (!g_board.rtc && g_board.src == BOARD_DEFAULT) { g_board.rtc = VIRT_RTC; g_defaulted |= D_RTC; }
    if (!g_board.nvirtio && g_board.src == BOARD_DEFAULT) {
        for (int i = 0; i < BOARD_MAX_VIRTIO; i++) {
            g_board.virtio_base[i]  = VIRT_VIRTIO + (uint64_t)i * 0x200;
            g_board.virtio_intid[i] = 48 + (uint32_t)i;
        }
        g_board.nvirtio = BOARD_MAX_VIRTIO;
        g_defaulted |= D_VIRTIO;
    }
    if (!g_board.ecam && g_board.src == BOARD_DEFAULT) {
        g_board.ecam = VIRT_ECAM; g_board.ecam_size = VIRT_ECAM_SIZE;
        g_board.pci_mmio32 = VIRT_MMIO32; g_board.pci_mmio32_size = VIRT_MMIO32_SIZE;
        g_defaulted |= D_PCI;
    }
    /* Sort the virtio slots by address: a tree lists them in whatever order
     * its generator emitted (QEMU: descending), and the drivers' "slot n" —
     * which appears in logs and tests — has always meant ascending. */
    for (int i = 1; i < g_board.nvirtio; i++)
        for (int j = i; j > 0 && g_board.virtio_base[j] < g_board.virtio_base[j - 1]; j--) {
            uint64_t b = g_board.virtio_base[j];  g_board.virtio_base[j] = g_board.virtio_base[j - 1];  g_board.virtio_base[j - 1] = b;
            uint32_t q = g_board.virtio_intid[j]; g_board.virtio_intid[j] = g_board.virtio_intid[j - 1]; g_board.virtio_intid[j - 1] = q;
        }
}

static const char* src_of(uint32_t bit) {
    if (g_defaulted & bit) return "DEFAULT (virt)";
    return g_board.src == BOARD_ACPI ? "acpi" : "dtb";
}

/* `board` — what the port believes this machine is, and WHY it believes it. */
static void cmd_board(const char* args) {
    (void)args;
    kprintf("board: %s (described by %s)\n", g_board.model ? g_board.model : "(no model)",
            g_board.src == BOARD_DTB ? "device tree" : g_board.src == BOARD_ACPI ? "ACPI" :
            "NOTHING - every field below is assumed");
    kprintf("  gic      v%d  dist %p  %s %p   [%s]\n", g_board.gic_version,
            (void*)(uintptr_t)g_board.gicd, g_board.gic_version == 3 ? "redist" : "cpuif",
            (void*)(uintptr_t)(g_board.gic_version == 3 ? g_board.gicr : g_board.gicc), src_of(D_GIC));
    kprintf("  timer    INTID %u   [%s]\n", g_board.timer_intid, src_of(D_TIMER));
    kprintf("  psci     via %s\n", g_board.psci_smc ? "SMC" : "HVC");
    kprintf("  uart     %p  INTID %u   [%s]\n", (void*)(uintptr_t)g_board.uart,
            g_board.uart_intid, src_of(D_UART));
    kprintf("  rtc      %p   [%s]\n", (void*)(uintptr_t)g_board.rtc, g_board.rtc ? src_of(D_RTC) : "none");
    kprintf("  virtio   %d transport(s)", g_board.nvirtio);
    if (g_board.nvirtio)
        kprintf(", %p..%p, INTID %u..%u",
                (void*)(uintptr_t)g_board.virtio_base[0],
                (void*)(uintptr_t)g_board.virtio_base[g_board.nvirtio - 1],
                g_board.virtio_intid[0], g_board.virtio_intid[g_board.nvirtio - 1]);
    kprintf("   [%s]\n", g_board.nvirtio ? src_of(D_VIRTIO) : "none");
    kprintf("  pcie     ecam %p (%u MiB), mmio32 %p (%u MiB)   [%s]\n",
            (void*)(uintptr_t)g_board.ecam, (unsigned)(g_board.ecam_size >> 20),
            (void*)(uintptr_t)g_board.pci_mmio32, (unsigned)(g_board.pci_mmio32_size >> 20),
            g_board.ecam ? src_of(D_PCI) : "none");
}
SHELL_CMD(board) = { "board", "", "what this aarch64 machine is, and where each fact came from",
                     SHELL_G_DEV, cmd_board, SHELL_P_ANY };

/* §M85 — hal_api.h.  The only platform device a portable driver asks about so
 * far is the PL031; a new one is a new name here, not a new constant there. */
int acpi_arm_find(const char* hid, int nth, uint64_t* mem, uint32_t* irq);
static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
int hal_platform_window(const char* name, uint64_t* base, uint64_t* len) {
    if (streq(name, "pl031") && g_board.rtc) { *base = g_board.rtc; *len = 0x1000; return 0; }
    /* Controllers on a SYSTEM bus (sbsa-ref), named only by ACPI _HID. */
    uint32_t irq;
    if (streq(name, "ahci") && acpi_arm_find("LNRO001E", 0, base, &irq) == 0) { *len = 0x1000; return 0; }
    if (streq(name, "xhci") && acpi_arm_find("PNP0D10", 0, base, &irq) == 0)  { *len = 0x4000; return 0; }
    return -1;
}
