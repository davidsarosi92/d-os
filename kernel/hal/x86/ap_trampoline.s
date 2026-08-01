; =============================================================================
; ap_trampoline.s — AP boot trampoline (M18).
;
; This file is assembled as a **flat binary** (nasm -f bin) and the
; resulting bytes are linked into the kernel image via objcopy as a
; data blob.  At SMP boot time the kernel memcpy's the blob to
; physical address 0x8000.  The BSP then sends a Startup IPI to each
; AP with vector 0x08, which makes the AP start executing at physical
; 0x8000 in 16-bit real mode.
;
; Why bin format and not the usual elf32: the trampoline runs at a
; fixed physical address (0x8000) that is NOT where it lives in the
; kernel image.  ELF + an org directive doesn't help — the labels
; need to resolve to 0x8000+offset at run time.  Flat binary with
; `org 0x8000` does exactly that.
;
; The per-AP launch info (page directory phys, stack top, C entry
; point, GDTR struct) is written by the BSP into a small data region
; at AP_INFO_ADDR (= 0x9000) just before sending the SIPI:
;
;   AP_INFO_ADDR + 0   uint32  page_directory_phys (CR3 value)
;   AP_INFO_ADDR + 4   uint32  stack_top
;   AP_INFO_ADDR + 8   uint32  c_entry (= address of ap_main)
;   AP_INFO_ADDR + 12  6 bytes GDTR (limit:2 + base:4)
;
; The trampoline reads from there at well-known offsets.  Keep this
; layout in sync with smp.c.
; =============================================================================

bits 16
org 0x8000

AP_INFO_ADDR equ 0x9000

ap_trampoline_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000              ; temporary real-mode stack (below trampoline)

    ; Load the GDT pointer the BSP planted at AP_INFO_ADDR + 12.
    ;
    ; The `o32` prefix is NOT cosmetic — it is load-bearing.  With the default
    ; 16-bit operand size, `lgdt m16&32` loads only the LOW 24 BITS of the base
    ; and forces bits 31:24 to zero (Intel SDM Vol 2A, LGDT: "the operand-size
    ; attribute determines whether a 16-bit or 32-bit base is loaded; with a
    ; 16-bit operand size the upper byte of the base is not loaded").  The
    ; 32-bit operand size loads the full 32-bit base.
    ;
    ; This was a silent time bomb: it worked for years because the kernel's
    ; .bss (where `gdt[]` lives) sat below 16 MiB, so the truncated byte was
    ; zero anyway.  Once the image grew past 16 MiB (embedded font + NetSurf /
    ; freetype blobs), `gdt` moved to ~0x014f61a0, the AP loaded GDTR base
    ; 0x004f61a0 instead, and the very next instruction that needs a descriptor
    ; — the far jump to selector 0x08 — took a #GP.  With no IDT yet that
    ; escalated #GP → #DF → triple fault, i.e. the whole box died the moment
    ; you passed `-smp 2`.  Symptom to remember: an AP that dies EXACTLY at the
    ; protected-mode far jump usually means a bad GDTR, not a bad descriptor.
    o32 lgdt [AP_INFO_ADDR + 12]

    ; Enable protected mode: CR0.PE = 1.
    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    ; Far jump to flush the prefetch queue and load CS = 0x08 (kernel
    ; code segment).  `dword` prefix forces a 32-bit operand size so
    ; the assembler emits the right encoding.
    jmp dword 0x08:ap_pm_entry

bits 32
ap_pm_entry:
    ; Reload data segment registers with kernel DS.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Load CR3 with the kernel page directory address.
    mov eax, [AP_INFO_ADDR + 0]
    mov cr3, eax

    ; Enable PSE (CR4 bit 4) so the BSP's 4 MiB identity map works.
    mov eax, cr4
    or  eax, 0x10
    mov cr4, eax

    ; Enable paging (CR0 bit 31).  After this instruction, EIP is
    ; translated via the page tables — but the identity map covers
    ; the trampoline's current physical location, so execution
    ; continues seamlessly.
    mov eax, cr0
    or  eax, 0x80000000
    mov cr0, eax

    ; Switch to the per-AP kernel stack.
    mov esp, [AP_INFO_ADDR + 4]

    ; Jump to the C entry point.  We use an indirect absolute jump so
    ; the trampoline doesn't need to know the kernel's link address.
    mov eax, [AP_INFO_ADDR + 8]
    jmp eax

ap_trampoline_end:
