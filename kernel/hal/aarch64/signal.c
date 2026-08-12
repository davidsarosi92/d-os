/* =============================================================================
 * signal.c — POSIX signal delivery + sigreturn, AArch64 (§A1).
 *
 * The ARM sibling of hal/x86/signal.c and hal/x86_64/signal.c.  Same shape:
 * when the kernel is about to return to EL0 and the task has a pending signal
 * with a handler, push the interrupted context onto the process's OWN stack,
 * point the CPU at the handler, and let the libc trampoline issue
 * SYS_SIGRETURN to unwind it.
 *
 * Two things differ from x86, and both matter:
 *
 *   1. THE RETURN ADDRESS IS A REGISTER, NOT A STACK SLOT.  x86 pushes the
 *      trampoline address so the handler's `ret` consumes it.  AArch64's `ret`
 *      branches to x30, so the trampoline goes in x30 and nothing is pushed.
 *      Consequently, on entry to SYS_SIGRETURN the user SP points exactly at
 *      the saved context — there is no return address above it to skip.
 *
 *   2. SP_EL0 IS NOT IN THE TRAPFRAME.  Taking an exception from EL0 switches
 *      the CPU to SP_EL1 and leaves SP_EL0 banked, so vectors.S never saved it.
 *      Delivery has to read and write it directly with mrs/msr — the frame
 *      restore in vectors.S will not do it for us.
 *
 * Everything is written to the user stack with the kernel's own stores, so a
 * bad or exhausted user SP would fault at EL1 (→ halt).  Both directions
 * therefore validate the range with vmm_user_access_ok first and kill the
 * process instead, exactly as the x86 twins do.
 * ============================================================================= */

#include "syscall.h"
#include "task.h"
#include "vmm.h"
#include "fd.h"
#include <stdint.h>

/* Matches the trapframe laid down by vectors.S (see exceptions.c/syscall.c). */
struct trapframe {
    uint64_t x[31];
    uint64_t _pad;
    uint64_t elr;
    uint64_t spsr;
};

/* Saved-context layout on the user stack: x0..x30, then SP, PC, PSTATE. */
enum { S_SP = 31, S_PC = 32, S_PSTATE = 33, S_N = 34 };

/* The ONLY PSTATE bits ring-3 may set via sigreturn: the condition flags
 * (N Z C V).  DAIF, EL/SP selection and everything else are kept from the
 * kernel's current frame, so a handler cannot return with interrupts masked or
 * at a higher exception level by crafting a PSTATE. */
#define SPSR_USER_MASK 0xF0000000ull

static inline uint64_t read_sp_el0(void) {
    uint64_t v; __asm__ volatile ("mrs %0, sp_el0" : "=r"(v)); return v;
}
static inline void write_sp_el0(uint64_t v) {
    __asm__ volatile ("msr sp_el0, %0" :: "r"(v));
}

void signal_deliver(struct trapframe* f) {
    if ((f->spsr & 0xF) != 0) return;          /* only when returning to EL0 */
    struct task* t = task_current();
    if (!t || !t->sig_pending) return;

    for (int sig = 1; sig < NSIG; sig++) {
        uint32_t bit = 1u << sig;
        if (!(t->sig_pending & bit)) continue;
        /* §M56.1 — a BLOCKED signal stays pending; it is not consumed and not
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
            }
            continue;
        }
        if (h == SIG_IGN)     continue;
        if (!t->sig_restorer) continue;         /* no trampoline → cannot deliver */

        uint64_t sp = read_sp_el0();
        sp &= ~(uint64_t)15;                    /* AArch64 requires SP 16-aligned */
        uint64_t frame = sp - (uint64_t)S_N * 8;

        if (!vmm_user_access_ok((uintptr_t)frame, (uintptr_t)S_N * 8, 1)) {
            if (t->user_task) { fd_close_all(); task_exit_code(128 + SIGSEGV); }
            continue;
        }

        uint64_t* sv = (uint64_t*)(uintptr_t)frame;
        for (int i = 0; i < 31; i++) sv[i] = f->x[i];
        sv[S_SP]     = read_sp_el0();
        sv[S_PC]     = f->elr;
        sv[S_PSTATE] = f->spsr;

        write_sp_el0(frame);
        f->x[0]  = (uint64_t)sig;               /* AAPCS64: arg0 in x0 */
        f->x[30] = (uint64_t)t->sig_restorer;   /* the handler's `ret` lands here */
        f->elr   = (uint64_t)h;                 /* eret enters the handler */
        return;                                 /* one signal per return */
    }
}

void signal_sigreturn(struct trapframe* f) {
    /* The trampoline's `svc` left SP_EL0 exactly at the saved context: unlike
     * x86 nothing was pushed above it, because the return address travelled in
     * x30. */
    uint64_t frame = read_sp_el0();
    if (!vmm_user_access_ok((uintptr_t)frame, (uintptr_t)S_N * 8, 0)) {
        struct task* t = task_current();
        if (t && t->user_task) task_exit_code(139);   /* 128 + SIGSEGV */
        return;
    }
    const uint64_t* sv = (const uint64_t*)(uintptr_t)frame;
    for (int i = 0; i < 31; i++) f->x[i] = sv[i];
    f->elr = sv[S_PC];
    write_sp_el0(sv[S_SP]);
    /* Restore ONLY the user-settable PSTATE bits; keep DAIF/EL from the
     * kernel's current (safe) frame. */
    f->spsr = (f->spsr & ~SPSR_USER_MASK) | (sv[S_PSTATE] & SPSR_USER_MASK);
}
