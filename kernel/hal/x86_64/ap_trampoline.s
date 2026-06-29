; =============================================================================
; ap_trampoline.s — AP boot trampoline for x86_64 (M20.5 Phase B).
;
; Three-stage transition mirrors what GRUB does for the BSP on a
; multiboot2 boot, just compressed into a flat-binary blob:
;
;   16-bit real mode  →  32-bit protected mode  →  64-bit long mode
;
; The AP wakes via a Startup IPI (SIPI) with vector V = AP_BOOT_ADDR >> 12.
; The CPU starts executing at physical V * 4 KiB in REAL mode, so the
; trampoline lives at a fixed low-memory address that satisfies that
; constraint.  We pick 0x8000 (vector 0x08) — same convention as the
; i386 port.
;
; Why a self-contained trampoline GDT (instead of the kernel's GDT
; like i386's i386 trampoline does):
;
;   The kernel's GDT pointer is 10 bytes in long mode (2-byte limit +
;   8-byte base).  `lgdt` in 16-bit real mode can only load 6 bytes
;   (16-bit limit + 24-bit base).  So we need at least *a* 32-bit-
;   capable GDT in the trampoline to do the initial mode switch.  Once
;   we're in 64-bit mode, we lgdt the kernel's real GDT (read from the
;   info struct) and a far-ret reloads CS to the kernel's code selector.
;
; The per-AP launch info is at AP_INFO_ADDR (0x9000):
;
;   AP_INFO_ADDR + 0    qword  pml4_phys           (CR3 value)
;   AP_INFO_ADDR + 8    qword  stack_top
;   AP_INFO_ADDR + 16   qword  c_entry             (full 64-bit address)
;   AP_INFO_ADDR + 24   word   kernel_gdtr_limit
;   AP_INFO_ADDR + 26   qword  kernel_gdtr_base    (8-byte; unaligned ok)
;
; smp.c writes this BEFORE sending the INIT IPI.  Only one AP at a
; time goes through the trampoline (the BSP launches them serially
; and waits for online status), so the info struct doesn't need to
; be per-AP.
; =============================================================================

bits 16
org 0x8000

AP_INFO_ADDR    equ 0x9000

; -----------------------------------------------------------------------------
; Stage 1 — 16-bit real mode entry.
; CS:IP on entry: 0x0800:0000 (or 0x0000:0x8000, equivalent).
; -----------------------------------------------------------------------------

ap_trampoline_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000              ; temporary real-mode stack (below trampoline)

    ; Load the inline trampoline GDT.  The 6-byte form (lgdt with
    ; m16:32 — limit:2 + base:4) is what NASM emits by default in
    ; 16-bit mode when given a 6-byte memory operand.  The base
    ; field is 32-bit; only the low 24 bits are used in real mode
    ; but tramp_gdt sits below 0x10000 so 16 bits are enough anyway.
    lgdt [tramp_gdtr]

    ; Enable protected mode (CR0.PE = 1).  Paging stays off for now.
    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    ; Far-jmp to 32-bit code.  The `dword` prefix forces a 32-bit
    ; operand-size encoding so the assembler emits the right opcode
    ; for the inter-segment jump.  Selector 0x08 is the 32-bit code
    ; descriptor in our trampoline GDT.
    jmp dword 0x08:ap_pm_entry

; -----------------------------------------------------------------------------
; Stage 2 — 32-bit protected mode.
; -----------------------------------------------------------------------------

bits 32
ap_pm_entry:
    ; Reload data segs with the trampoline GDT's 32-bit data selector.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; CR4.PAE = 1 (required before turning on paging with long-mode
    ; tables, just like the BSP path in boot.s).
    mov eax, cr4
    or  eax, 1 << 5                 ; CR4.PAE
    mov cr4, eax

    ; CR3 = PML4 phys (shared with the BSP).  The PML4 lives in low
    ; memory (kernel image at 1 MiB), so the high 32 bits of the qword
    ; field are zero — we only need the low half.
    mov eax, [AP_INFO_ADDR + 0]
    mov cr3, eax

    ; EFER.LME = 1 (arm long mode).
    mov ecx, 0xC0000080             ; EFER MSR
    rdmsr
    or  eax, 1 << 8                 ; LME
    wrmsr

    ; CR0.PG = 1 — CPU enters long-mode compatibility submode.
    mov eax, cr0
    or  eax, 1 << 31                ; PG
    mov cr0, eax

    ; Far-jmp through trampoline GDT's 64-bit code descriptor
    ; (selector 0x18) to true 64-bit mode.
    jmp 0x18:ap_lm_entry

; -----------------------------------------------------------------------------
; Stage 3 — 64-bit long mode.
; -----------------------------------------------------------------------------

bits 64
ap_lm_entry:
    ; Long mode mostly ignores data segments, but ss must be a valid
    ; selector for some push/pop semantics; load the trampoline
    ; GDT's 64-bit data descriptor (0x20).
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Per-AP kernel stack (full 64-bit address from info struct).
    mov rsp, [AP_INFO_ADDR + 8]

    ; Switch to the kernel's real GDT.  After lgdt, the descriptor
    ; cache for CS is stale — CS still references "selector 0x18 in
    ; old GDT" which would now be re-evaluated against the new GDT
    ; the next time something touches CS.  We MUST do a far-jmp or
    ; far-ret immediately to atomically reload CS with a selector
    ; that's correct in the new GDT.
    lgdt [AP_INFO_ADDR + 24]

    ; Build a far-ret frame on the stack: push selector (kernel CS =
    ; 0x08 in the new kernel GDT) and target RIP, then retfq.  retfq
    ; pops {rip, cs} atomically and the new CS comes from the new
    ; GDT.
    lea rax, [rel ap_after_gdt]
    push qword 0x08                 ; kernel CS in kernel GDT
    push rax
    retfq

ap_after_gdt:
    ; CS is now 0x08 (kernel code64).  Reload data segs with kernel
    ; DS (0x10) to match.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Indirect absolute jump to the C entry — full 64-bit virt addr
    ; (ap_main, somewhere in the kernel image).  The kernel image is
    ; identity-mapped via the boot.s PML4, so the virtual address
    ; resolves directly.
    mov rax, [AP_INFO_ADDR + 16]
    jmp rax

; -----------------------------------------------------------------------------
; Inline trampoline GDT.
;
; Slot layout:
;   0 (0x00)  null
;   1 (0x08)  32-bit kernel code  (G=1, D=1, L=0)
;   2 (0x10)  32-bit kernel data
;   3 (0x18)  64-bit kernel code  (G=1, L=1, D=0)  — long-mode code
;   4 (0x20)  64-bit kernel data
; -----------------------------------------------------------------------------

align 8
tramp_gdt:
    dq 0                                ; 0: null
    dq 0x00CF9A000000FFFF               ; 1: 32-bit code (access=9A flags=CF)
    dq 0x00CF92000000FFFF               ; 2: 32-bit data
    dq 0x00AF9A000000FFFF               ; 3: 64-bit code (flags=AF: G=1 L=1 D=0)
    dq 0x00CF92000000FFFF               ; 4: 64-bit data
tramp_gdt_end:

tramp_gdtr:
    dw tramp_gdt_end - tramp_gdt - 1    ; limit
    dd tramp_gdt                        ; base (32-bit; tramp_gdt is low)

ap_trampoline_end:
