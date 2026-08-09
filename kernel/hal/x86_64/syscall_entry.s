; =============================================================================
; syscall_entry.s — the SYSCALL-instruction entry point (x86_64, §M20.6.1).
;
; WHY this exists: an unmodified x86_64 musl/Linux binary issues system calls
; with the `syscall` instruction (its inline `syscall_arch.h` hard-codes it —
; we can NOT change that without patching musl, and pristine musl is the whole
; point of the Linux-ABI personality).  i386 musl used `int 0x80`; x86_64 musl
; uses `syscall`.  So to run x86_64 musl we must wire up the fast-syscall path.
;
; THE SIMPLIFICATION (why this is finally cheap after being deferred all
; project long): the deferred note said SYSCALL/SYSRET "needs a GDT slot reorg
; to satisfy SYSRET's selector arithmetic".  True for SYSRET — but we do NOT
; have to return via SYSRET.  SYSCALL's *entry* only reads STAR[47:32] for the
; kernel CS/SS (already 0x08/0x10 in our GDT), and we return via `iretq` by
; fabricating an interrupt frame identical to the one `int 0x80` produces and
; falling into the shared `isr_common` tail.  That sidesteps the SYSRET
; +8/+16 selector constraint entirely — no GDT reorg.
;
; ON ENTRY (set by the CPU from the SYSCALL semantics):
;   rcx = user RIP to resume       (SYSCALL clobbers rcx with return address)
;   r11 = user RFLAGS              (SYSCALL clobbers r11 with saved RFLAGS)
;   CS  = 0x08, SS = 0x10          (loaded from STAR[47:32] / +8)
;   RFLAGS masked by IA32_FMASK    (we clear IF there → interrupts OFF, like an
;                                    int-gate; matches the int 0x80 contract)
;   rsp = STILL the user stack     (SYSCALL does NOT switch stacks — we must)
;   rax = Linux syscall number; rdi/rsi/rdx/r10/r8/r9 = args 0..5 (SysV/Linux).
;
; STACK SWITCH: SYSCALL gives us no kernel stack — rsp is STILL the user stack
; on entry — so we must stash the user rsp and load this task's kernel stack
; top.  And we must do it with no free register: rcx and r11 are already gone
; (the instruction clobbers them) and everything else holds a syscall argument.
;
; §M52 — THIS USED TO BE A PAIR OF GLOBALS, and the header said so:
;
;     "This uses a GLOBAL scratch pair, so it is UP-correct only — which
;      matches x86_64's current single (non-per-CPU) TSS: ring-3 tasks only run
;      on the BSP today. ... Noted, not built."
;
; The note outlived its premise.  §M35 gave x86_64 a per-CPU TSS and ring-3
; tasks have run on APs ever since, so two CPUs could be inside SYSCALL at the
; same time — overwriting each other's stashed user rsp AND running the kernel
; on the SAME stack.  What that looked like was two musl processes (a shell and
; the coreutil it forked) corrupting each other only under -smp: a child
; executing its own argv strings, and the kernel returning to address 3.
;
; `swapgs` is the instruction that exists for precisely this problem: it swaps
; IA32_KERNEL_GS_BASE into GS.base atomically, handing the stub a per-CPU base
; it can address through WITHOUT needing a spare register to compute one.  GS
; is free for it — x86_64 musl keeps thread-local storage in FS.
;
;   [gs:0]  this CPU's kernel stack top   (written by hal_set_kernel_stack)
;   [gs:8]  this CPU's user-rsp stash
;
; We swapgs back BEFORE entering the shared isr_common tail, so the rest of the
; kernel — and the iretq back to ring 3 — sees exactly the state it always did
; and needs no knowledge of any of this.  Nothing can interrupt the window
; between the two swapgs: FMASK cleared IF, and both memory accesses touch
; already-mapped kernel pages.
; =============================================================================

bits 64
section .text

extern isr_common                       ; shared GPR-save + iretq tail (isr_stubs.s)
global syscall_entry_64

section .text
syscall_entry_64:
    ; Interrupts are already masked (FMASK cleared IF).  Switch stacks via
    ; this CPU's own slot — see the header for why swapgs and not a global.
    swapgs                               ; GS.base = &g_syscall_area[this cpu]
    mov [gs:8], rsp                      ; stash the caller's (user) rsp
    mov rsp, [gs:0]                      ; ring-0 stack top for the running task

    ; Fabricate the exact frame `int 0x80` leaves for isr_common: the CPU-
    ; pushed 5-quadword iretq frame (ss, rsp, rflags, cs, rip), then err_code
    ; and int_no.  int_no = 0x81 is our sentinel meaning "arrived via the
    ; SYSCALL instruction" → idt.c routes it straight to the Linux-ABI x86_64
    ; dispatcher (SysV register convention), distinct from int 0x80 (0x80,
    ; native d-os convention).
    push qword 0x23                      ; SS  = user data (RPL 3)
    push qword [gs:8]                    ; RSP = user stack (our own slot)
    push r11                             ; RFLAGS (user's, saved by SYSCALL)
    push qword 0x1B                      ; CS  = user code64 (RPL 3)
    push rcx                             ; RIP = user resume (saved by SYSCALL)
    push qword 0                         ; err_code (none)
    push qword 0x81                      ; int_no sentinel = SYSCALL instruction
    swapgs                               ; hand GS back before ordinary kernel code
    jmp isr_common                       ; save GPRs → isr_handler → iretq back
