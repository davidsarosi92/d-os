/* =============================================================================
 * main_entry.c — bridge from boot.s long_mode_entry to kernel_main.
 *
 * boot.s leaves us in 64-bit long mode with:
 *   rdi = multiboot2 loader magic (0x36d76289)
 *   rsi = physical address of the mb2 info structure
 *   rsp = top of the boot stack
 *
 * Two jobs:
 *   1. Translate the mb2 info structure into the mb1 shape the rest
 *      of the kernel expects (kernel.c, pmm.c, fb_terminal.c all read
 *      mb1 fields).  See mb2.c.
 *   2. Call kernel_main(MULTIBOOT_BOOTLOADER_MAGIC, mb1_ptr).
 *      kernel.c stays arch-independent.
 *
 * Why a C bridge instead of inlining in boot.s: the translation needs
 * tag-walking logic and bitmask hygiene that's ugly in nasm but
 * trivial in C.
 * ============================================================================= */

#include "multiboot.h"
#include "printf.h"
#include <stdint.h>

extern void kernel_main(uint32_t mb_magic, uintptr_t mb_info)
    __attribute__((noreturn));

extern uintptr_t mb2_translate_to_mb1(uintptr_t mb2_info);

void x86_64_main_entry(uint32_t mb2_magic, uintptr_t mb2_info)
    __attribute__((noreturn));

void x86_64_main_entry(uint32_t mb2_magic, uintptr_t mb2_info) {
    /* Sanity check the loader magic.  If GRUB handed us something
     * unexpected we still call kernel_main but with magic=0 so
     * kernel.c's mboot_init bails cleanly. */
    if (mb2_magic != 0x36d76289u) {
        kernel_main(0, 0);
        for (;;) __asm__ volatile ("cli; hlt");
    }

    uintptr_t mb1_info = mb2_translate_to_mb1(mb2_info);
    kernel_main(MULTIBOOT_BOOTLOADER_MAGIC, mb1_info);
    for (;;) __asm__ volatile ("cli; hlt");
}
