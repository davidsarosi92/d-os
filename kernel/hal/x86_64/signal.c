/* =============================================================================
 * signal.c — POSIX signal delivery on the return-to-user path (M34 slice E),
 * x86_64.  Twin of kernel/hal/x86/signal.c; read that file's header for the
 * design.  Only the register set and the slot width differ.
 *
 * The portable half (kill/sigaction in usyscall.c) sets per-task state:
 *   task->sig_pending      — bitmask of posted signals
 *   task->sig_handler[sig] — SIG_DFL(0) / SIG_IGN(1) / a ring-3 handler
 *   task->sig_restorer     — the libc trampoline that issues SYS_SIGRETURN
 *
 * The arch half is *delivery*: signal_deliver(f) runs after every syscall
 * returns to ring 3 (called from idt.c).  It rewrites the trapframe so the
 * pending iretq lands in the handler, having pushed a signal frame on the user
 * stack:
 *
 *     rsp ->  [ trampoline addr ]   (handler's return address)
 *             [ saved user context: rax..r15, rip, rflags, rsp ]
 *
 * ONE DELIBERATE DIFFERENCE FROM THE i386 TWIN.  There, the signal number is
 * pushed on the stack because that is where the cdecl ABI expects the first
 * argument.  amd64 SysV passes it in RDI, so the number goes in the register
 * and no argument slot exists on the stack — which also means sigreturn finds
 * the saved context 8 bytes above RSP (past the trampoline's own return
 * address) rather than 8 bytes past a pushed argument.  Getting that offset
 * wrong is silent: the handler runs fine and the RESTORE brings back garbage.
 *
 * Frames are written through the user VAs directly: delivery runs while the
 * target task's address space is active (its own syscall), so supervisor
 * writes to its user stack pages just work — but the range is validated first,
 * because a bad user RSP would otherwise fault in ring 0.
 * ============================================================================= */

#include "syscall.h"
#include "idt.h"
#include "task.h"
#include "vmm.h"
#include <stdint.h>

/* The ONLY RFLAGS bits ring-3 may set via sigreturn: the arithmetic +
 * direction flags (CF PF AF ZF SF DF OF).  IF, IOPL, NT, TF and the reserved
 * bits are kept from the kernel's current trapframe, so a signal handler cannot
 * disable interrupts or raise its privilege by returning a crafted RFLAGS. */
#define RFLAGS_USER_MASK 0x0000000000000CD5ull

/* Saved-context slot order (all 8 bytes wide). */
enum {
    S_RAX, S_RBX, S_RCX, S_RDX, S_RSI, S_RDI, S_RBP,
    S_R8, S_R9, S_R10, S_R11, S_R12, S_R13, S_R14, S_R15,
    S_RIP, S_RFLAGS, S_RSP, S_N
};

void signal_deliver(struct int_frame* f) {
    if ((f->cs & 3) != 3) return;              /* only when returning to ring 3 */
    struct task* t = task_current();
    if (!t || !t->sig_pending) return;

    for (int sig = 1; sig < NSIG; sig++) {
        uint32_t bit = 1u << sig;
        if (!(t->sig_pending & bit)) continue;
        /* §M57 — a BLOCKED signal stays pending; it is not consumed and not
         * delivered.  Skipping the `sig_pending &= ~bit` below is the whole
         * mechanism: sigprocmask must DEFER a signal, never lose one, and
         * clearing the bit here would turn "block SIGPIPE while I write" into
         * "throw away the SIGPIPE that arrived while I wrote".
         *
         * SIGKILL and SIGSTOP cannot be blocked — a process must not be able
         * to make itself unkillable, which on this kernel is not a policy
         * nicety but the thing §M46 exists to guarantee. */
        if ((t->sig_blocked & bit) && sig != SIGKILL) continue;
        t->sig_pending &= ~bit;

        uintptr_t h = t->sig_handler[sig];

        if (h == SIG_DFL) {
            /* Default: terminate on the fatal signals, ignore the rest. */
            if (sig == SIGINT || sig == SIGTERM || sig == SIGKILL || sig == SIGSEGV) {
                if (t->user_task) { fd_close_all(); task_exit_code(128 + sig); }
                /* (excursion tasks can't self-terminate cleanly here — ignore) */
            }
            continue;
        }
        if (h == SIG_IGN)      continue;
        if (!t->sig_restorer)  continue;        /* no trampoline → cannot deliver */

        /* The frame is written onto the process's OWN stack; if that stack
         * pointer is bad or exhausted the kernel-mode stores would #PF in ring
         * 0 (→ halt).  Validate the whole range is user-writable first; if not,
         * terminate the process (SIGSEGV) rather than fault. */
        uintptr_t need = (uintptr_t)(S_N * 8 + 8);
        uintptr_t frame_lo = (uintptr_t)f->rsp - need;
        if (!vmm_user_access_ok(frame_lo, need, 1)) {
            if (t->user_task) { fd_close_all(); task_exit_code(128 + SIGSEGV); }
            continue;
        }

        uint64_t sp = f->rsp;

        sp -= (uint64_t)S_N * 8;                /* saved user context           */
        uint64_t* sv = (uint64_t*)(uintptr_t)sp;
        sv[S_RAX] = f->rax; sv[S_RBX] = f->rbx; sv[S_RCX] = f->rcx; sv[S_RDX] = f->rdx;
        sv[S_RSI] = f->rsi; sv[S_RDI] = f->rdi; sv[S_RBP] = f->rbp;
        sv[S_R8]  = f->r8;  sv[S_R9]  = f->r9;  sv[S_R10] = f->r10; sv[S_R11] = f->r11;
        sv[S_R12] = f->r12; sv[S_R13] = f->r13; sv[S_R14] = f->r14; sv[S_R15] = f->r15;
        sv[S_RIP] = f->rip; sv[S_RFLAGS] = f->rflags; sv[S_RSP] = f->rsp;

        sp -= 8; *(uint64_t*)(uintptr_t)sp = (uint64_t)t->sig_restorer; /* ret addr */

        f->rsp = sp;
        f->rdi = (uint64_t)sig;                 /* amd64 SysV: arg0 in RDI      */
        f->rip = (uint64_t)h;                   /* iretq lands in the handler   */
        return;                                 /* one signal per return        */
    }
}

void signal_sigreturn(struct int_frame* f) {
    /* On entry (the trampoline's int 0x80) RSP points just above the return
     * address the handler consumed, i.e. straight at the saved context. */
    uintptr_t frame = (uintptr_t)f->rsp;
    /* The saved context lives on the UNTRUSTED user stack.  Validate it is
     * mapped + user-readable before touching it, so a crafted RSP can't fault
     * the kernel (→ halt); a bad frame kills the process instead. */
    if (!vmm_user_access_ok(frame, (uintptr_t)S_N * 8, 0)) {
        struct task* t = task_current();
        if (t && t->user_task) task_exit_code(139);   /* 128 + SIGSEGV */
        return;
    }
    uint64_t* sv = (uint64_t*)frame;
    f->rax = sv[S_RAX]; f->rbx = sv[S_RBX]; f->rcx = sv[S_RCX]; f->rdx = sv[S_RDX];
    f->rsi = sv[S_RSI]; f->rdi = sv[S_RDI]; f->rbp = sv[S_RBP];
    f->r8  = sv[S_R8];  f->r9  = sv[S_R9];  f->r10 = sv[S_R10]; f->r11 = sv[S_R11];
    f->r12 = sv[S_R12]; f->r13 = sv[S_R13]; f->r14 = sv[S_R14]; f->r15 = sv[S_R15];
    f->rip = sv[S_RIP]; f->rsp = sv[S_RSP];
    /* Restore ONLY the user-settable RFLAGS bits; keep IF/IOPL/etc. from the
     * kernel's current (safe) trapframe. */
    f->rflags = (f->rflags & ~RFLAGS_USER_MASK) | (sv[S_RFLAGS] & RFLAGS_USER_MASK);
}
