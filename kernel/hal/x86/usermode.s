; =============================================================================
; usermode.s — drop the CPU from ring 0 to ring 3 (and a way back).
;
; Layout of the trick:
;
;   enter_user_mode_wrap(uint32_t eip, uint32_t esp):
;     - pushes callee-saved regs + EBP
;     - stashes the resulting kernel ESP into `saved_esp`
;     - stashes the address of the `.return` label into `saved_eip`
;     - builds a ring-3 iret frame (SS, ESP, EFLAGS, CS, EIP)
;     - executes `iret` — CPU now runs in ring 3 at `eip` with stack `esp`
;
;   The ring-3 program runs until it issues `int 0x80` with EAX = 1
;   (the SYS_EXIT syscall).  syscall.c handles that by setting ESP =
;   `saved_esp` and jumping to `saved_eip` — i.e. into our `.return`
;   label below — bypassing the normal iret-back-to-user path.
;
;   `.return` then pops the saved callee regs and `ret`s to whoever
;   called `enter_user_mode_wrap`.  From the caller's point of view,
;   the function ran a ring-3 program and returned normally.
;
; The kernel stack used by syscall handler is TSS.esp0 (a separate 4 KiB
; buffer in tss.c), so the syscall doesn't trample the kernel state we
; saved here.  See tss.c for the rationale.
; =============================================================================

bits 32
section .text

global enter_user_mode_wrap
global saved_esp
global saved_eip

extern resume_kernel_after_syscall_exit

; Internal storage exported as global so syscall.c can reach them.
section .data
align 4
saved_esp: dd 0
saved_eip: dd 0

section .text

; void enter_user_mode_wrap(uint32_t eip, uint32_t esp);
;   Stack on entry:  [esp+0]=ret_addr, [esp+4]=eip, [esp+8]=esp
enter_user_mode_wrap:
    pushad                                      ; save 8 GP regs (32 bytes)

    mov [saved_esp], esp                        ; remember kernel ESP
    mov dword [saved_eip], .return              ; remember resume label

    ; Pull args off the stack.  After pushad, args are at +36, +40
    mov eax, [esp + 36]                         ; eip
    mov ebx, [esp + 40]                         ; esp (user)

    ; Load user data segments.  Selector 0x23 = GDT_USER_DS | RPL 3.
    mov cx, 0x23
    mov ds, cx
    mov es, cx
    mov fs, cx
    mov gs, cx

    ; Build iret frame on top of current (kernel) stack.
    push 0x23                                   ; SS3
    push ebx                                    ; ESP3 (user stack)
    pushfd                                      ; EFLAGS
    pop ecx
    or ecx, 0x200                               ; set IF so IRQs work in ring 3
    push ecx                                    ; EFLAGS
    push 0x1B                                   ; CS3 = GDT_USER_CS | RPL 3
    push eax                                    ; EIP3 (user entry)
    iret                                        ; → ring 3

.return:
    ; Reached when the SYS_EXIT syscall handler in syscall.c jumps here
    ; with ESP restored to its post-pushad value.  Restore segment
    ; registers, pop the saved GP regs, return to the original caller.
    mov cx, 0x10                                ; GDT_KERNEL_DS
    mov ds, cx
    mov es, cx
    mov fs, cx
    mov gs, cx

    popad
    ret
