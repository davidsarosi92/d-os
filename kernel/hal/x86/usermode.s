; =============================================================================
; usermode.s — drop the CPU from ring 0 to ring 3 (and a way back).
;
; Layout of the trick:
;
;   enter_user_mode_wrap(uint32_t eip, uint32_t esp):
;     - pushes callee-saved regs + EBP
;     - stashes the resulting kernel ESP and the address of the `.return`
;       label into the caller-supplied resume[0..1] (the task's exc_resume, #11)
;     - builds a ring-3 iret frame (SS, ESP, EFLAGS, CS, EIP)
;     - executes `iret` — CPU now runs in ring 3 at `eip` with stack `esp`
;
;   The ring-3 program runs until it issues `int 0x80` with EAX = 1
;   (the SYS_EXIT syscall).  syscall.c handles that by setting ESP =
;   the task's exc_resume[0] and jumping to exc_resume[1] — i.e. into our `.return`
;   label below — bypassing the normal iret-back-to-user path.
;
;   `.return` then pops the saved callee regs and `ret`s to whoever
;   called `enter_user_mode_wrap`.  From the caller's point of view,
;   the function ran a ring-3 program and returned normally.
;
; The kernel stack used by the syscall handler is TSS.esp0, which
; enter_user_mode_wrap (proc.c) points at the task's OWN stack 512 bytes below
; the caller, so the syscall doesn't trample the kernel state saved here (#11;
; it used to be a per-CPU shared buffer in tss.c).
; =============================================================================

bits 32
section .text

global arch_enter_user_wrap
global enter_user_mode
global enter_user_mode_regs

extern resume_kernel_after_syscall_exit

; #11 — the resume point is stored where the CALLER says (the task's
; exc_resume), not in globals: two excursions at once used to overwrite each
; other's saved ESP/EIP here.
section .data
align 4
global g_entry_gs
g_entry_gs: dw 0x23                              ; ring-3 %gs for enter_user_mode_regs

section .text

; void arch_enter_user_wrap(uint32_t eip, uint32_t esp, uint32_t* resume);
;   Stack on entry:  [esp+0]=ret_addr, [esp+4]=eip, [esp+8]=esp, [esp+12]=resume
arch_enter_user_wrap:
    pushad                                      ; save 8 GP regs (32 bytes)

    mov ecx, [esp + 44]                         ; resume (after pushad: +32)
    mov [ecx], esp                              ; resume[0] = kernel ESP
    mov dword [ecx + 4], .return                ; resume[1] = resume label

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

; -----------------------------------------------------------------------------
; void enter_user_mode(uint32_t eip, uint32_t esp);  — Tier B, ONE-WAY.
;
; The concurrent-user-process entry: unlike enter_user_mode_wrap it saves NO
; kernel resume context — the caller (a user-task bootstrap) never returns to
; kernel C code.  The task lives at ring 3; it re-enters the kernel only via
; syscalls/IRQs (on its own kstack, selected by TSS.esp0 the scheduler set),
; and SYS_EXIT ends the task via task_exit() rather than teleporting back.
;
; Stack on entry: [esp+0]=ret_addr, [esp+4]=eip, [esp+8]=esp(user)
; -----------------------------------------------------------------------------
enter_user_mode:
    mov eax, [esp + 4]                          ; eip (user entry)
    mov ebx, [esp + 8]                          ; esp (user stack)

    mov cx, 0x23                                ; GDT_USER_DS | RPL 3
    mov ds, cx
    mov es, cx
    mov fs, cx
    mov gs, cx

    push 0x23                                   ; SS3
    push ebx                                    ; ESP3 (user stack)
    pushfd
    pop ecx
    or ecx, 0x200                               ; IF=1 so IRQs preempt ring 3
    push ecx                                    ; EFLAGS
    push 0x1B                                   ; CS3 = GDT_USER_CS | RPL 3
    push eax                                    ; EIP3 (user entry)
    iret                                        ; → ring 3, never returns here

; -----------------------------------------------------------------------------
; void enter_user_mode_regs(struct user_regs* r);  — M34 fork child resume.
;
; Resume ring 3 with the FULL parent register set captured at fork's int 0x80,
; but with eax = r->eax (0 for the child).  struct user_regs offsets (uintptr_t
; = 4 bytes on i386): eax 0, ebx 4, ecx 8, edx 12, esi 16, edi 20, ebp 24,
; eip 28, eflags 32, user_sp 36.  Never returns.
; -----------------------------------------------------------------------------
enter_user_mode_regs:
    mov eax, [esp + 4]                          ; r (struct pointer)

    mov cx, 0x23                                ; user data segments
    mov ds, cx
    mov es, cx
    mov fs, cx
    mov gs, [g_entry_gs]                         ; %gs = TLS selector for a musl
                                                ; fork child (set by fork.c),
                                                ; else 0x23

    push 0x23                                   ; SS3
    push dword [eax + 36]                       ; ESP3 = r->user_sp
    mov ecx, [eax + 32]                         ; r->eflags
    or  ecx, 0x200                              ; force IF=1
    push ecx                                    ; EFLAGS
    push 0x1B                                   ; CS3
    push dword [eax + 28]                       ; EIP3 = r->eip

    ; Load the general regs (eax LAST — it holds the struct pointer until now).
    mov ebx, [eax + 4]
    mov ecx, [eax + 8]
    mov edx, [eax + 12]
    mov esi, [eax + 16]
    mov edi, [eax + 20]
    mov ebp, [eax + 24]
    mov eax, [eax + 0]                          ; r->eax (child sees fork()==0)
    iret                                        ; → ring 3 at the fork point
