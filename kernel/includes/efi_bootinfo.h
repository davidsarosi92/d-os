/* =============================================================================
 * efi_bootinfo.h — what the aarch64 EFI stub hands the kernel (§M85 stage 3).
 *
 * The raw `-kernel` path gives the kernel a device-tree pointer in x0 (or
 * nothing).  A firmware boot (UEFI: `virt` with EDK2, `sbsa-ref`, real
 * machines) goes through a small EFI application (kernel/boot/efi/) that asks
 * the firmware the three things the kernel needs and cannot ask for itself
 * once boot services are gone:
 *
 *   - the MEMORY MAP — authoritative under UEFI.  A device tree's /memory node
 *     describes the whole bank, but the firmware keeps runtime services, ACPI
 *     tables and its own reservations INSIDE that bank; only the EFI map says
 *     which pages are really free.  The stub lists only what may be used.
 *   - the ACPI RSDP, if the firmware describes the machine with ACPI;
 *   - a device tree, if it describes it with one (EDK2 on `virt` may do both).
 *
 * Passed in x0 exactly where a DTB pointer would be; the kernel tells the two
 * apart by the first eight bytes (an FDT starts with 0xd00dfeed, this with
 * DOS_BOOTINFO_MAGIC).  Shared by the stub and the kernel, so plain fixed-width
 * fields only — both sides are aarch64, but nothing here may depend on a
 * pointer's width being the same in two separately built images.
 * ============================================================================= */
#ifndef DOS_EFI_BOOTINFO_H
#define DOS_EFI_BOOTINFO_H

#include <stdint.h>

#define DOS_BOOTINFO_MAGIC  0x3130494645534f44ull   /* "DOSEFI01", little-endian */
#define DOS_BI_MAXMEM       128

struct dos_bi_range { uint64_t base, size; };

struct dos_bootinfo {
    uint64_t magic;
    uint64_t rsdp;              /* ACPI 2.0+ RSDP physical address, 0 = none  */
    uint64_t dtb;               /* flattened device tree, 0 = none            */
    uint32_t nmem;              /* usable RAM ranges (merged, ascending)      */
    uint32_t entered_el;        /* the EL the stub ran at (1 or 2)            */
    uint32_t efi_entries;       /* raw descriptor count, for the report       */
    uint32_t reserved;
    struct dos_bi_range mem[DOS_BI_MAXMEM];
    char     fw_vendor[64];     /* the firmware's own name, ASCII-folded      */
};

#endif /* DOS_EFI_BOOTINFO_H */
