; =============================================================================
; boot.s — Multiboot2 header + 32-bit → 64-bit long-mode entry for x86_64.
;
; M20 Phase 2: this is the smallest interesting bring-up in the
; codebase.  It receives control from GRUB in 32-bit protected mode
; (multiboot2 §3.1.5) and is responsible for:
;
;   1. Sanity-check that long mode is actually available (CPUID).
;   2. Build a 4-level identity-mapped page table that covers the
;      first 1 GiB of physical memory.  Uses 2 MiB large pages (PS=1
;      in the PD) so the table only needs 3 frames (PML4 + PDPT + PD)
;      regardless of how much we map.
;   3. Flip the CPU into 64-bit long mode by walking the canonical
;      Intel/AMD sequence: enable PAE in CR4, point CR3 at the PML4,
;      set EFER.LME, then enable paging in CR0.  The CPU is now in
;      long-mode-compatibility submode (32-bit code with 64-bit
;      paging).
;   4. Load a tiny 64-bit GDT and `jmp` through a 64-bit code segment.
;      This transitions to true 64-bit long mode.
;   5. (In bits 64.) Print "Hello from x86_64 long mode\r\n" to COM1
;      via polled UART, then halt.
;
; Phase 2's DoD is exactly the serial print.  Phase 3+ will replace
; the print+halt tail with a call to a C entry point.
;
; Multiboot2 hand-off state we preserve through the transition:
;   eax = 0x36d76289                       (loader magic)
;   ebx = phys addr of mb2 info structure  (sized & aligned per spec)
; We stash both in r12/r13 (callee-saved across the 64-bit jmp) so
; that future phases can hand them to kernel_main without re-deriving.
;
; References:
;   Intel SDM Vol 3A §9.8 (Transition to IA-32e Mode) — the sequence
;     of CR/EFER ops below comes from the worked example there.
;   AMD64 APM Vol 2 §14 (Memory Paging).
;   Multiboot2 spec §3.1.4 (machine state on entry).
; =============================================================================

bits 32

; -----------------------------------------------------------------------------
; Multiboot2 header.  Same shape as Phase 1; GRUB parses this before
; loading the rest of the kernel.
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

    ; Framebuffer-request tag (type=5).  Asks GRUB to switch the display
    ; to a graphics mode of the given preferred size and hand us the
    ; framebuffer info via a type-8 reply tag at runtime.  The mb1
    ; equivalent was the (flags bit 2 + mode_type/width/height/depth)
    ; fields in the original boot.s header.
    ;
    ; Layout (mb2 §3.1.10):
    ;   u16 type   = 5
    ;   u16 flags  = 0      (must be honored)
    ;   u32 size   = 20
    ;   u32 width  preferred  pixels
    ;   u32 height preferred  pixels
    ;   u32 depth  preferred  bits-per-pixel  (0 = "I don't care")
    align 8
    dw 5            ; type: framebuffer
    dw 0            ; flags
    dd 20           ; size
    dd 1024         ; width
    dd 768          ; height
    dd 32           ; depth

    ; End tag — required terminator.  type=0, flags=0, size=8.
    align 8
    dw 0
    dw 0
    dd 8
header_end:

; -----------------------------------------------------------------------------
; Stack (.bss, zero-filled at load) and early page tables.
;
; The three page table frames are placed back-to-back in .bss with 4
; KiB alignment, which gives us PML4 / PDPT / PD at three contiguous
; 4 KiB-aligned addresses.  Zero-init from .bss is exactly what we
; want for the "this slot is empty" bit pattern (P=0); we only need
; to write the handful of slots that point to the next level or to a
; 2 MiB page.
; -----------------------------------------------------------------------------

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

align 4096
global pml4
pml4:    resb 4096
global pdpt
pdpt:    resb 4096
global pd_low
pd_low:  resb 4096

; -----------------------------------------------------------------------------
; 64-bit GDT.  Two real entries:
;   index 1 (selector 0x08) — 64-bit code segment.  Long-mode code
;     descriptors are identified by L=1 (bit 53 of the descriptor) and
;     ignore base/limit; the CPU treats the segment as flat 0..2^64.
;   index 2 (selector 0x10) — data segment.  In long mode data
;     segments are almost ignored, but we still need a non-null entry
;     to load into ds/es/ss without #GP.
;
; Encoded as raw qwords because building them with nasm bitfield
; syntax is more error-prone than reading the AMD APM tables once.
;
; Bit layout (descriptor qword, low-to-high):
;   0..15   limit_low      (ignored in long mode; we set 0xFFFF)
;   16..31  base_low       (ignored in long mode; 0)
;   32..39  base_mid       (ignored in long mode; 0)
;   40..47  access:  type(4) | S(1) | DPL(2) | P(1)
;             code64: 0x9A = present, ring0, code, executable, readable
;             data:   0x92 = present, ring0, data, writable
;   48..51  limit_high     (ignored in long mode; 0)
;   52..55  flags:   AVL(1) | L(1) | D/B(1) | G(1)
;             code64: 0xA = G=1, L=1, D=0 (D MUST be 0 when L=1)
;             data:   0xC = G=1, D/B=1, L=0 (legacy data flags ok)
;   56..63  base_high      (ignored in long mode; 0)
; -----------------------------------------------------------------------------

section .rodata
align 16
gdt64:
    dq 0                                      ; index 0: null
.code: equ $ - gdt64
    dq 0x00AF9A000000FFFF                     ; index 1: 64-bit code
.data: equ $ - gdt64
    dq 0x00CF92000000FFFF                     ; index 2: data
gdt64_end:

; 6-byte GDTR pointer for the 32-bit lgdt.  In 32-bit mode lgdt loads
; a 16-bit limit + 32-bit base; the CPU zero-extends the base to 64
; bits when long mode is later entered, so this pointer is correct
; for both modes as long as gdt64 lives in the lower 4 GiB (which it
; does — kernel is loaded at 1 MiB).
gdtr64:
    dw gdt64_end - gdt64 - 1
    dd gdt64

; Hello string for the post-long-mode print.  '\r\n' so it shows up
; nicely on a raw serial console.
hello_msg:
    db "Hello from x86_64 long mode", 13, 10, 0

; "no long mode" error string, in case CPUID says we can't get there.
no_lm_msg:
    db "ERROR: CPU does not support long mode", 13, 10, 0

; -----------------------------------------------------------------------------
; _start — 32-bit entry from GRUB.
; -----------------------------------------------------------------------------

section .text
global _start
extern x86_64_main_entry

_start:
    ; -------------------------------------------------------------------------
    ; Stash multiboot2 hand-off state so it survives the long-mode
    ; transition.  edi/esi don't survive the CR-twiddling cleanly; r12
    ; isn't accessible until we're in 64-bit mode; so we just keep
    ; them in fixed memory in .bss.  Phase 3+ will reload them.
    ; -------------------------------------------------------------------------
    mov esp, stack_top                        ; private stack
    mov [mb2_magic], eax                      ; save loader magic
    mov [mb2_info],  ebx                      ; save info ptr

    ; -------------------------------------------------------------------------
    ; CPUID long-mode check.  Two-step:
    ;   1. Is CPUID extended-leaf 0x80000001 supported at all?  Query
    ;      0x80000000 and look at the max-leaf in eax.
    ;   2. If yes, query 0x80000001 and look at EDX bit 29 (LM).
    ; If either fails, fall through to .no_lm.
    ; -------------------------------------------------------------------------
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_lm

    mov eax, 0x80000001
    cpuid
    bt  edx, 29                               ; LM bit
    jnc .no_lm

    ; -------------------------------------------------------------------------
    ; Build identity-mapped page tables for the first 1 GiB using 2
    ; MiB pages.  Frames are in .bss (already zeroed); we only set:
    ;   PML4[0] -> PDPT      (present, writable)
    ;   PDPT[0] -> PD        (present, writable)
    ;   PD[i]   = i*2MiB     (present, writable, PS=1)  for i in 0..511
    ; -------------------------------------------------------------------------

    ; PML4[0] = pdpt | 0x3 (P | RW)
    mov eax, pdpt
    or  eax, 0x3
    mov [pml4], eax                           ; low half
    mov dword [pml4 + 4], 0                   ; high half: 0 since phys < 4 GiB

    ; PDPT[0] = pd_low | 0x3
    mov eax, pd_low
    or  eax, 0x3
    mov [pdpt], eax
    mov dword [pdpt + 4], 0

    ; PD entries: i = 0..511, PD[i] = (i * 2MiB) | 0x83 (P | RW | PS)
    mov edi, pd_low
    mov eax, 0x83                             ; flags + addr_low for i=0
    mov ecx, 512
.fill_pd:
    mov [edi],     eax
    mov dword [edi + 4], 0                    ; high half always 0 for phys < 1 GiB
    add eax, 0x200000                         ; next 2 MiB
    add edi, 8                                ; next PD entry (8 bytes)
    loop .fill_pd

    ; -------------------------------------------------------------------------
    ; Long-mode entry sequence (Intel SDM Vol 3A §9.8.5).
    ;
    ; Steps must happen in this order or the CPU faults:
    ;   1. Disable paging (already off — GRUB hands us paging=off).
    ;   2. Enable PAE (CR4 bit 5).  REQUIRED before turning on paging
    ;      with long-mode tables.
    ;   3. Load CR3 with PML4 phys addr.
    ;   4. Set EFER.LME (MSR 0xC0000080 bit 8).  This arms long mode
    ;      but does not enter it.
    ;   5. Enable paging (CR0 bit 31).  Setting PG with LME armed AND
    ;      PAE on is what makes the CPU latch IA-32e mode.  After this
    ;      the CPU is in compatibility submode of long mode — 32-bit
    ;      code, 64-bit paging.
    ;   6. Far-jmp through a 64-bit code descriptor to enter true 64-
    ;      bit mode.
    ; -------------------------------------------------------------------------

    ; (2) PAE on
    mov eax, cr4
    or  eax, 1 << 5                           ; CR4.PAE
    mov cr4, eax

    ; (3) CR3 = pml4
    mov eax, pml4
    mov cr3, eax

    ; (4) EFER.LME = 1
    mov ecx, 0xC0000080                       ; EFER MSR
    rdmsr
    or  eax, 1 << 8                           ; LME
    wrmsr

    ; (5) CR0.PG = 1 (PE is already 1 since GRUB hands us in protected mode)
    mov eax, cr0
    or  eax, 1 << 31                          ; PG
    mov cr0, eax

    ; (6) Load 64-bit GDT, then far-jmp into a 64-bit code segment.
    ; The lgdt operand is the 6-byte gdtr64 (limit + 32-bit base).
    lgdt [gdtr64]

    ; Far-jmp encoded as 0xEA + 32-bit offset + 16-bit selector.
    ; nasm renders `jmp 0x08:long_mode_entry` as exactly that.
    jmp 0x08:long_mode_entry

.no_lm:
    ; CPU can't long-mode.  Print error and halt.  We have no GDT/IDT
    ; that lets us call out cleanly, so just polled COM1 inline.
    mov esi, no_lm_msg
.nlm_loop:
    mov al, [esi]
    test al, al
    jz .hang32
    call print_byte_32
    inc esi
    jmp .nlm_loop

.hang32:
    cli
    hlt
    jmp .hang32

; -----------------------------------------------------------------------------
; print_byte_32 — 32-bit polled UART output of al on COM1 (0x3F8).
;
; QEMU's default COM1 is unconfigured at boot but it accepts writes
; anyway because the underlying chardev (stdio / file) doesn't care
; about UART line-control bits.  Real hardware would need a
; full configure first; we get away with it here because all our
; testing is in QEMU.  Phase 3 will switch to the real serial driver.
; -----------------------------------------------------------------------------
print_byte_32:
    push edx
    push eax
    mov ah, al                                ; save char
.wait_tx:
    mov dx, 0x3FD                             ; LSR
    in  al, dx
    test al, 0x20                             ; THRE
    jz .wait_tx
    mov dx, 0x3F8                             ; DATA
    mov al, ah
    out dx, al
    pop eax
    pop edx
    ret

; -----------------------------------------------------------------------------
; 64-bit entry — landing pad of the far-jmp.
;
; By the time we're here:
;   - CPU is in 64-bit submode of long mode (REX prefixes, 64-bit
;     GPRs accessible).
;   - CS = 0x08 (the 64-bit code descriptor we loaded).
;   - Data segment registers (ds/es/fs/gs/ss) still hold their
;     32-bit values from GRUB's GDT — in long mode that's almost
;     always fine (CPU ignores them for most accesses) but we set
;     them to 0 to be hygienic.  ss must be non-null for some
;     instructions (push/pop semantics rely on ss base), so we load
;     selector 0x10 (our data descriptor) into it.
;   - rsp still points at stack_top (valid in both modes).
;   - r12/r13 etc are full 64-bit registers now; use them to load
;     the saved multiboot magic/info for Phase 3+ use.
; -----------------------------------------------------------------------------

bits 64

long_mode_entry:
    ; Reload data segs.  Null is fine for ds/es/fs/gs in long mode
    ; (CPU just zero-extends and ignores base/limit), but use 0x10 for
    ; ss because some CPUs reject a null ss for certain ops.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Print the hello message via COM1 as a "we made it" signal.
    ; This is harmless to leave in — gives us a sentinel line in the
    ; serial log that long-mode bring-up succeeded before we hand
    ; off to the C-level boot path.
    lea rsi, [rel hello_msg]
.hello_loop:
    mov al, [rsi]
    test al, al
    jz .hello_done
    call print_byte_64
    inc rsi
    jmp .hello_loop

.hello_done:
    ; Pass multiboot2 hand-off (saved by 32-bit _start in .bss) into
    ; the System V x86_64 first-arg registers.  rdi = uint32_t magic
    ; (zero-extended from eax-load); rsi = uintptr_t info pointer.
    ;
    ; The kernel was loaded by GRUB into low memory, so mb2_info fits
    ; in 32 bits — we just zero-extend.
    xor edi, edi
    mov edi, [mb2_magic]
    xor esi, esi
    mov esi, [mb2_info]

    ; Stack must be 16-byte aligned just before `call` per System V
    ; ABI.  stack_top is .bss-aligned to 16, and we haven't pushed
    ; anything on the long-mode side, so we're good.
    call x86_64_main_entry

    ; x86_64_main_entry is marked noreturn — if we ever come back
    ; here something went very wrong.  Halt.
.hang64:
    cli
    hlt
    jmp .hang64

; -----------------------------------------------------------------------------
; print_byte_64 — 64-bit polled UART output of al on COM1.
;
; Same protocol as print_byte_32, just with REX-friendly register
; choices.  Clobber-clean by saving rdx/rax.
; -----------------------------------------------------------------------------
print_byte_64:
    push rdx
    push rax
    mov ah, al
.wait_tx64:
    mov dx, 0x3FD
    in  al, dx
    test al, 0x20
    jz .wait_tx64
    mov dx, 0x3F8
    mov al, ah
    out dx, al
    pop rax
    pop rdx
    ret

; -----------------------------------------------------------------------------
; Saved multiboot2 hand-off (.bss, zero-init).  Filled by _start in
; 32-bit mode, reloaded by long_mode_entry into r12/r13.  Lives at a
; fixed addr in low memory so both modes can reference it directly.
; -----------------------------------------------------------------------------
section .bss
align 4
mb2_magic: resd 1
mb2_info:  resd 1
