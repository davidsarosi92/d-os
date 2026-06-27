; =============================================================================
; isr_stubs.s — per-vector asm stubs + common interrupt prologue/epilogue.
;
; For every vector we care about (0..47) we need an entry point the CPU can
; jump to.  Each stub does just two things:
;   1. push a dummy error code (unless the CPU already pushed a real one);
;   2. push the vector number;
; then jumps to `isr_common` which saves registers, sets up kernel data
; segments, calls the C dispatcher, restores registers, and does `iret`.
;
; Error code behavior (Intel SDM Vol 3, §6.3.1, Table 6-1):
;   Exceptions that push a real error code:  8, 10, 11, 12, 13, 14, 17.
;   Everyone else (including all IRQs):       nothing — we push a fake 0.
; Keeping the stack layout uniform is what lets `isr_common` and the C
; handler treat every vector identically.
;
; Stack layout by the time `isr_handler` runs (low address first):
;    [esp +  0]  gs
;    [esp +  4]  fs
;    [esp +  8]  es
;    [esp + 12]  ds
;    [esp + 16]  edi, esi, ebp, esp_ignored, ebx, edx, ecx, eax  (pusha)
;    [esp + 48]  int_no
;    [esp + 52]  err_code
;    [esp + 56]  eip, cs, eflags  (pushed by the CPU)
;    [esp + 68]  user_esp, ss     (only on privilege change)
; =============================================================================

bits 32
section .text

extern isr_handler                      ; C dispatcher in idt.c

; -----------------------------------------------------------------------------
; ISR stub generators.  Two macros: one for vectors with no CPU-pushed
; error code (we push a fake 0), one for those that have one (we push the
; intno right away and let the CPU's error code stay under it).
; -----------------------------------------------------------------------------

%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0                        ; fake error code
    push dword %1                       ; interrupt / vector number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1                       ; error code already on stack
    jmp isr_common
%endmacro

; -----------------------------------------------------------------------------
; CPU exceptions 0..31.  Error-code ones per the SDM table.
; -----------------------------------------------------------------------------
ISR_NOERR 0                             ; Divide Error
ISR_NOERR 1                             ; Debug
ISR_NOERR 2                             ; NMI
ISR_NOERR 3                             ; Breakpoint
ISR_NOERR 4                             ; Overflow
ISR_NOERR 5                             ; BOUND Range Exceeded
ISR_NOERR 6                             ; Invalid Opcode
ISR_NOERR 7                             ; Device Not Available
ISR_ERR   8                             ; Double Fault
ISR_NOERR 9                             ; Coprocessor Segment Overrun (obsolete)
ISR_ERR   10                            ; Invalid TSS
ISR_ERR   11                            ; Segment Not Present
ISR_ERR   12                            ; Stack-Segment Fault
ISR_ERR   13                            ; General Protection Fault
ISR_ERR   14                            ; Page Fault
ISR_NOERR 15                            ; reserved
ISR_NOERR 16                            ; x87 FP
ISR_ERR   17                            ; Alignment Check
ISR_NOERR 18                            ; Machine Check
ISR_NOERR 19                            ; SIMD FP
ISR_NOERR 20                            ; Virtualization
ISR_NOERR 21                            ; Control Protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; -----------------------------------------------------------------------------
; IRQ stubs for vectors 32..47 (after PIC remap).  None carry an error code.
; -----------------------------------------------------------------------------
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

; -----------------------------------------------------------------------------
; Syscall vector — int 0x80.  Ring-3 callable; the IDT entry for this
; vector is installed with DPL=3 so user mode is allowed to invoke it.
; The label is literally `isr128` (NASM expands %1 verbatim into the name)
; so we use decimal to keep the C side's extern matching simple.
; -----------------------------------------------------------------------------
ISR_NOERR 128

; -----------------------------------------------------------------------------
; Common continuation: save remaining state, swap to kernel segments,
; call C, restore, iret.
; -----------------------------------------------------------------------------
isr_common:
    pusha                               ; edi, esi, ebp, esp, ebx, edx, ecx, eax

    push ds                             ; save data segments
    push es
    push fs
    push gs

    mov ax, 0x10                        ; GDT_KERNEL_DS
    mov ds, ax                          ; switch to kernel data everywhere,
    mov es, ax                          ; in case we came from ring 3 later
    mov fs, ax
    mov gs, ax

    push esp                            ; arg: pointer to struct int_frame
    call isr_handler
    add esp, 4                          ; discard the argument

    pop gs                              ; restore caller's segments
    pop fs
    pop es
    pop ds

    popa                                ; restore general regs
    add esp, 8                          ; pop int_no + err_code
    iret                                ; returns to saved cs:eip with eflags
