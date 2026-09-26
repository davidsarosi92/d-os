/* efistub.ld — the EFI stub's layout (§M85 stage 3).  See efistub_head.S.
 * Linked at 0 so an RVA and a link address are the same number; every
 * section starts on the 4 KiB section/file alignment the header declares. */
ENTRY(efi_entry)
SECTIONS
{
    . = 0;
    .head : { *(.text.head) }
    . = ALIGN(0x1000);
    _text_start = .;
    .text : { *(.text .text.*) *(.rodata .rodata.*) . = ALIGN(0x1000); }
    _text_end = .;
    _data_start = .;
    /* .bss folded into .data: a PE section with raw data is zero-filled from
     * the FILE, so nothing depends on a loader clearing uninitialised data. */
    .data : { *(.data .data.*) *(.bss .bss.*) *(COMMON) . = ALIGN(0x1000); }
    _data_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.interp) *(.dynamic)
                  *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) }

    pe_text_rva   = _text_start;
    pe_text_size  = _text_end - _text_start;
    pe_data_rva   = _data_start;
    pe_data_size  = _data_end - _data_start;
    pe_image_size = _data_end;
    pe_entry      = efi_entry;
}
