/* =============================================================================
 * efistub_head.S — a PE/COFF header written by hand, and the EFI entry point
 * (§M85 stage 3).
 *
 * UEFI loads PE32+ images.  Rather than depend on a toolchain that can emit
 * one (binutils' aarch64 PE support is recent and uneven), the header is laid
 * out here byte by byte and the whole image is produced with `objcopy -O
 * binary` — the same approach as Linux's arm64 Image and gnu-efi.
 *
 * The rules that make it load, each one a way to be refused:
 *   - Machine 0xAA64, Magic 0x20B (PE32+), Subsystem 10 (EFI application);
 *   - RVAs EQUAL file offsets (SectionAlignment == FileAlignment, and the
 *     linker script aligns every section to it), so the flat binary IS the
 *     loaded image;
 *   - NO base-relocation directory and NO IMAGE_FILE_RELOCS_STRIPPED flag: the
 *     code is position independent, and a loader that finds neither simply
 *     does not relocate — with the flag set, EDK2 refuses to load the image
 *     anywhere but its ImageBase;
 *   - every size and offset below is an ABSOLUTE symbol from efistub.ld: an
 *     assembler cannot subtract symbols in different sections, the linker can.
 * ============================================================================= */

    .section .text.head, "ax"
    .globl  efi_image_start
efi_image_start:
    .ascii  "MZ"                       /* DOS header — only these two fields  */
    .skip   0x3a                       /* matter to a PE loader               */
    .long   pe_header - efi_image_start          /* e_lfanew at 0x3c          */

pe_header:
    .ascii  "PE\0\0"
    .short  0xAA64                     /* Machine: ARM64                      */
    .short  2                          /* NumberOfSections                    */
    .long   0                          /* TimeDateStamp                       */
    .long   0                          /* PointerToSymbolTable                */
    .long   0                          /* NumberOfSymbols                     */
    .short  opt_end - opt              /* SizeOfOptionalHeader                */
    .short  0x0206                     /* EXECUTABLE | LINE_NUMS_STRIPPED |   */
                                       /* DEBUG_STRIPPED — NOT relocs-stripped*/
opt:
    .short  0x020B                     /* PE32+                               */
    .byte   0, 0                       /* linker version                      */
    .long   pe_text_size               /* SizeOfCode                          */
    .long   pe_data_size               /* SizeOfInitializedData               */
    .long   0                          /* SizeOfUninitializedData             */
    .long   pe_entry                   /* AddressOfEntryPoint                 */
    .long   pe_text_rva                /* BaseOfCode                          */
    .quad   0                          /* ImageBase                           */
    .long   0x1000                     /* SectionAlignment                    */
    .long   0x1000                     /* FileAlignment                       */
    .short  0, 0, 0, 0, 0, 0           /* OS / image / subsystem versions     */
    .long   0                          /* Win32VersionValue                   */
    .long   pe_image_size              /* SizeOfImage                         */
    .long   pe_text_rva                /* SizeOfHeaders                       */
    .long   0                          /* CheckSum                            */
    .short  10                         /* Subsystem: EFI_APPLICATION          */
    .short  0                          /* DllCharacteristics                  */
    .quad   0, 0, 0, 0                 /* stack/heap reserve + commit         */
    .long   0                          /* LoaderFlags                         */
    .long   6                          /* NumberOfRvaAndSizes                 */
    .quad   0, 0, 0, 0, 0, 0           /* data directories — all empty        */
opt_end:

    /* Section table. */
    .ascii  ".text\0\0\0"
    .long   pe_text_size, pe_text_rva, pe_text_size, pe_text_rva
    .long   0, 0
    .short  0, 0
    .long   0x60000020                 /* CODE | EXECUTE | READ               */

    .ascii  ".data\0\0\0"
    .long   pe_data_size, pe_data_rva, pe_data_size, pe_data_rva
    .long   0, 0
    .short  0, 0
    .long   0xC0000040                 /* INITIALIZED_DATA | READ | WRITE     */

    .balign 0x1000

/* ---- entry: x0 = ImageHandle, x1 = SystemTable, lr = return to firmware ---- */
    .section .text, "ax"
    .globl  efi_entry
efi_entry:
    b       efi_main                   /* the standard ABI already applies    */

/* ---- the kernel, flat, as the linker laid it out at its load address ------- */
    .section .data.payload, "aw"
    .balign 16
    .globl  kernel_payload, kernel_payload_end
kernel_payload:
    .incbin KERNEL_IMG_PATH
kernel_payload_end:
