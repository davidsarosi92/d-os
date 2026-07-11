/* =============================================================================
 * signal.c — POSIX signal delivery on the return-to-user path (M34 slice E), i386.
 *
 * kill()/sigaction() (portable, usyscall.c) just set per-task state:
 *   task->sig_pending      — bitmask of posted signals
 *   task->sig_handler[sig] — SIG_DFL(0) / SIG_IGN(1) / a ring-3 handler
 *   task->sig_restorer     — the libc trampoline that issues SYS_SIGRETURN
 *
 * The arch-specific half is *delivery*: `signal_deliver(f)` runs after every
 * syscall returns to ring 3 (called from idt.c).  If a pending unblocked
 * signal has a custom handler it rewrites the trapframe so the pending iret
 * lands in the handler, having pushed a signal frame on the user stack:
 *
 *     user_esp ->  [ trampoline addr ]   (handler's return address)
 *                  [ sig ]               (handler's int argument)
 *                  [ saved user context: eax..ebp, eip, eflags, esp ]
 *
 * The handler runs, `ret`s into the trampoline, which calls SYS_SIGRETURN;
 * `signal_sigreturn(f)` then restores the saved context into the trapframe so
 * the original iret target resumes.  Default action for the terminating
 * signals (INT/TERM/KILL/SEGV) with no handler is to end the task.
 *
 * We write the frame through the user VAs directly: delivery runs while the
 * target task's address space is active (its own syscall), so supervisor
 * writes to its user stack pages just work.
 * ============================================================================= */

#include "syscall.h"
#include "idt.h"
#include "task.h"
#include <stdint.h>

/* saved context slot order pushed below the sig arg. */
enum { S_EAX, S_EBX, S_ECX, S_EDX, S_ESI, S_EDI, S_EBP, S_EIP, S_EFLAGS, S_ESP, S_N };

void signal_deliver(struct int_frame* f) {
    if ((f->cs & 3) != 3) return;              /* only when returning to ring 3 */
    struct task* t = task_current();
    if (!t || !t->sig_pending) return;

    for (int sig = 1; sig < NSIG; sig++) {
        uint32_t bit = 1u << sig;
        if (!(t->sig_pending & bit)) continue;
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

        /* Build the signal frame on the user stack. */
        uint32_t sp = (uint32_t)f->user_esp;

        sp -= S_N * 4;                          /* saved user context           */
        uint32_t* sv = (uint32_t*)sp;
        sv[S_EAX] = f->eax; sv[S_EBX] = f->ebx; sv[S_ECX] = f->ecx; sv[S_EDX] = f->edx;
        sv[S_ESI] = f->esi; sv[S_EDI] = f->edi; sv[S_EBP] = f->ebp;
        sv[S_EIP] = f->eip; sv[S_EFLAGS] = f->eflags; sv[S_ESP] = (uint32_t)f->user_esp;

        sp -= 4; *(uint32_t*)sp = (uint32_t)sig;             /* handler argument */
        sp -= 4; *(uint32_t*)sp = (uint32_t)t->sig_restorer; /* return address   */

        f->user_esp = sp;
        f->eip      = (uint32_t)h;              /* iret lands in the handler    */
        return;                                 /* one signal per return        */
    }
}

void signal_sigreturn(struct int_frame* f) {
    /* On entry (the trampoline's int 0x80) user_esp points just below the sig
     * argument; the saved context sits right above it (see the layout above). */
    uint32_t* sv = (uint32_t*)((uint32_t)f->user_esp + 4);
    f->eax = sv[S_EAX]; f->ebx = sv[S_EBX]; f->ecx = sv[S_ECX]; f->edx = sv[S_EDX];
    f->esi = sv[S_ESI]; f->edi = sv[S_EDI]; f->ebp = sv[S_EBP];
    f->eip = sv[S_EIP]; f->eflags = sv[S_EFLAGS]; f->user_esp = sv[S_ESP];
}
