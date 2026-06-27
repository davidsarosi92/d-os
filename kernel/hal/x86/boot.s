; =============================================================================
; boot.s — Multiboot1 header and kernel entry point for i386.
;
; GRUB parses the multiboot header below, loads our kernel to the load address
; that the linker script specifies (1 MiB), then jumps to the `_start` symbol
; with the CPU already in 32-bit protected mode, paging off, and a minimal
; flat GDT provided by GRUB.
;
; Our job here is tiny: set up a stack we own, hand GRUB's values off to
; kernel_main as arguments, and halt forever if kernel_main ever returns.
;
; Reference: Multiboot1 Specification §3 (OS image format) and §3.2 (machine
; state on entry).
; =============================================================================

bits 32

; -----------------------------------------------------------------------------
; Multiboot1 header constants.
;
; Flag bits we use:
;   bit 0 (0x01) — page-align modules
;   bit 1 (0x02) — request memory map (mem_lower/upper + mmap_*)
;   bit 2 (0x04) — request video mode info (the mode_* fields below)
;
; When bit 2 is set the loader reads the mode_type / width / height / depth
; fields that follow.  It will try to switch the display to a mode that
; matches and populate the framebuffer_* fields in the info struct (which we
; consume from C).
;
; CHECKSUM is whatever makes (MAGIC + FLAGS + CHECKSUM) == 0 modulo 2^32.
; -----------------------------------------------------------------------------
MB_MAGIC    equ 0x1BADB002
MB_FLAGS    equ 0x00000007                      ; bits 0 | 1 | 2
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

; The header must be 4-byte aligned and live within the first 8 KiB of the
; final kernel image.  The linker script places `.multiboot` as the very
; first section to guarantee that.
;
; Because we set flag bit 2 (video mode), the header must include the full
; 48 bytes including the earlier header_addr/load_addr/... block (which we
; leave zero because we are not using flag bit 16 / a.out kludge).
section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM
    ; Fields for flag bit 16 — unused, zeroed:
    dd 0                                        ; header_addr
    dd 0                                        ; load_addr
    dd 0                                        ; load_end_addr
    dd 0                                        ; bss_end_addr
    dd 0                                        ; entry_addr
    ; Fields for flag bit 2:
    dd 0                                        ; mode_type: 0 = linear graphics
    dd 1024                                     ; preferred width  (pixels)
    dd 768                                      ; preferred height (pixels)
    dd 32                                       ; preferred depth  (bits per pixel)

; -----------------------------------------------------------------------------
; Kernel stack.  Lives in .bss so the bytes are not carried in the ELF image;
; the loader simply zero-fills the region at load time.  The stack grows
; downward on x86, so `stack_top` (the higher address) is what we load into
; %esp.  16 KiB is plenty for anything we do before a real memory manager
; exists; later milestones may replace this with per-task stacks.
; -----------------------------------------------------------------------------
section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

; -----------------------------------------------------------------------------
; _start — the actual entry point (declared as the ENTRY symbol in linker.ld).
;
; GRUB hands us two values on entry:
;   %eax = 0x2BADB002  (multiboot loader magic, i.e. "we really are multiboot")
;   %ebx = physical address of the multiboot_info structure
;
; We want to pass those to `kernel_main(uint32_t magic, uint32_t info_ptr)`.
; The System V i386 ABI pushes arguments right-to-left, so the info pointer
; goes on the stack first and magic goes second.  After `call`, if control
; ever returns (it shouldn't — kernel_main loops forever), we disable
; interrupts and halt so the CPU stops instead of executing garbage.
; -----------------------------------------------------------------------------
section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top                          ; switch to our stack
    push ebx                                    ; arg 2: multiboot_info*
    push eax                                    ; arg 1: multiboot magic
    call kernel_main
.hang:
    cli                                         ; mask maskable interrupts
    hlt                                         ; stop until the next (non-maskable) interrupt
    jmp .hang                                   ; just in case an NMI wakes us up
