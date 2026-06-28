; =============================================================================
; boot.s — Multiboot2 header + 32-bit entry stub for the x86_64 port.
;
; M20 Phase 1: minimal-viable stub.  Goal here is just to validate the
; end-to-end build path: nasm assembles, ld links an ELF64, grub-mkrescue
; produces a bootable ISO, GRUB recognises the multiboot2 header, loads
; the kernel, and jumps to _start.
;
; What this file does on Phase 1:
;   1. Emits a valid multiboot2 header (magic + arch + length + checksum +
;      end-tag).
;   2. Provides a 32-bit _start that disables interrupts and halts forever.
;
; What this file does NOT do yet (Phase 2 will add):
;   - Set up early page tables.
;   - Enable PAE / long mode (EFER.LME).
;   - Jump to 64-bit code.
;   - Print to serial.
;
; Even though the binary is ELF64, the _start symbol contains 32-bit
; instructions because GRUB hands us control in 32-bit protected mode
; (same as multiboot1).  We don't get to 64-bit code until we've set up
; CR0/CR3/CR4/EFER ourselves and done a far jmp through a 64-bit GDT.
;
; Multiboot2 reference: https://www.gnu.org/software/grub/manual/multiboot2/
; =============================================================================

bits 32

; -----------------------------------------------------------------------------
; Multiboot2 header.
;
; Structure (§3.1.1 of the spec):
;   dd magic     = 0xe85250d6
;   dd arch      = 0 (i386 — meaning 32-bit protected mode entry, which is
;                     what GRUB hands us even when booting a 64-bit kernel)
;   dd length    = header_end - header_start
;   dd checksum  = -(magic + arch + length)
;   ... optional tags ...
;   end tag: dw 0 type, dw 0 flags, dd 8 size
;
; All fields are little-endian dwords.  The header must be 8-byte aligned
; and live within the first 32 KiB of the final image (placement at the
; very front of the binary via linker-x86_64.ld's .multiboot section).
; -----------------------------------------------------------------------------

MB2_MAGIC equ 0xe85250d6
MB2_ARCH  equ 0                                ; 0 = i386 protected-mode entry

section .multiboot
align 8
header_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd header_end - header_start
    dd 0x100000000 - (MB2_MAGIC + MB2_ARCH + (header_end - header_start))

    ; End tag — required terminator.  type=0, flags=0, size=8.
    align 8
    dw 0
    dw 0
    dd 8
header_end:

; -----------------------------------------------------------------------------
; Initial kernel stack.  Same pattern as i386: zero-filled at load time,
; %esp pointed at the high end.  16 KiB is enough for the early bring-up
; and for the call frames Phase 2-6 will plant on it.
; -----------------------------------------------------------------------------
section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

; -----------------------------------------------------------------------------
; _start — entry from GRUB.
;
; GRUB hands us in 32-bit protected mode with:
;   %eax = 0x36d76289       (multiboot2 loader magic)
;   %ebx = physical address of the multiboot2 info structure
;
; Phase 1: just park.  Phase 2 will replace .hang with real long-mode
; setup.  We deliberately do NOT touch %eax/%ebx here so the future
; long-mode handoff can still hand them off to kernel_main.
; -----------------------------------------------------------------------------
section .text
global _start

_start:
    mov esp, stack_top                          ; private stack
.hang:
    cli
    hlt
    jmp .hang
