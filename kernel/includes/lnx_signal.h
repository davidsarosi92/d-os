/* =============================================================================
 * lnx_signal.h — Linux signals, delivered (§M89 rung 2).
 *
 * The native §M34 signals are a small in-house convention: a handler gets the
 * signal number and nothing else, and a fault simply kills the process.  A
 * Linux program expects much more, and a JVM treats it as part of its own
 * machinery:
 *
 *   - SA_SIGINFO handlers get (sig, siginfo_t*, ucontext_t*) and may READ and
 *     WRITE the interrupted registers through the ucontext — HotSpot turns a
 *     fault at a known instruction into a Java exception by rewriting the
 *     saved program counter, then returns;
 *   - a FAULT (SIGSEGV with si_addr, SIGILL, SIGFPE, SIGBUS) is delivered to
 *     the handler instead of killing the process — implicit null checks and
 *     stack-overflow detection are faults on purpose;
 *   - sa_mask, SA_NODEFER, SA_RESETHAND, SA_ONSTACK + sigaltstack (a handler
 *     for a stack overflow cannot run on the stack that overflowed);
 *   - tkill/tgkill: a signal to one THREAD (pthread_kill);
 *   - the right DEFAULT actions (SIGPIPE, SIGABRT, SIGBUS terminate; SIGCHLD,
 *     SIGWINCH, SIGURG are ignored).
 *
 * THE SPLIT.  What a disposition IS and which signal is next are decided here,
 * once.  The FRAME — siginfo + ucontext with the machine context in the
 * layout each guest's libc was compiled against — and rt_sigreturn are per
 * architecture (hal/<arch>/lnx_sigframe.c), because that layout is the one
 * thing that genuinely differs.
 *
 * Signal numbers are Linux's (1..31) and this kernel keeps 32; real-time
 * signals are refused at sigaction rather than accepted and never delivered.
 * ============================================================================= */
#ifndef DOS_LNX_SIGNAL_H
#define DOS_LNX_SIGNAL_H

#include <stdint.h>
#include <stddef.h>

struct task;
struct abi_ctx;

#define LNX_NSIG 32

/* sa_flags — the same values on all three guests. */
#define LNX_SA_NOCLDSTOP 0x00000001u
#define LNX_SA_SIGINFO   0x00000004u
#define LNX_SA_ONSTACK   0x08000000u
#define LNX_SA_RESTORER  0x04000000u
#define LNX_SA_RESTART   0x10000000u
#define LNX_SA_NODEFER   0x40000000u
#define LNX_SA_RESETHAND 0x80000000u

/* sigaltstack ss_flags */
#define LNX_SS_ONSTACK   1
#define LNX_SS_DISABLE   2

/* si_code values used here */
#define LNX_SI_USER      0
#define LNX_SI_TKILL     (-6)
#define LNX_SI_KERNEL    0x80
#define LNX_SEGV_MAPERR  1
#define LNX_SEGV_ACCERR  2
#define LNX_ILL_ILLOPN   2
#define LNX_FPE_INTDIV   1
#define LNX_BUS_ADRERR   2
#define LNX_TRAP_BRKPT   1

struct lnx_sigaction {
    uintptr_t handler;          /* 0 = SIG_DFL, 1 = SIG_IGN, else an address */
    uintptr_t restorer;         /* the libc's rt_sigreturn trampoline        */
    uint32_t  flags;            /* LNX_SA_*                                  */
    uint32_t  mask;             /* blocked during the handler (kernel bits)  */
};

/* What siginfo_t will carry.  Filled when the signal is POSTED — a fault's
 * address is known then and nowhere later. */
struct lnx_siginfo {
    int       signo;
    int       code;
    int       pid, uid;         /* sender, for SI_USER / SI_TKILL            */
    uintptr_t addr;             /* fault address (SEGV, BUS, ILL, FPE)       */
};

/* A signal picked for delivery. */
struct lnx_delivery {
    int                  sig;
    struct lnx_sigaction act;
    struct lnx_siginfo   info;
    uint32_t             old_blocked;   /* goes into uc_sigmask              */
    uintptr_t            sp;            /* the stack to build the frame on   */
    int                  on_altstack;
};

/* Post `sig` to `t` with `info` (NULL = SI_USER from the caller).  Returns 0,
 * or -1 for a bad signal number. */
int  lnx_sig_post(struct task* t, int sig, const struct lnx_siginfo* info);

/* On the way back to user mode: is there a signal to run a HANDLER for?
 * Pending signals whose disposition is to ignore are dropped here, and a
 * default-fatal one terminates the task (this does not return then).
 * Returns 1 and fills `d` when the arch must build a frame; `user_sp` is the
 * interrupted stack pointer, used to decide between it and the altstack. */
int  lnx_sig_next(struct task* t, uintptr_t user_sp, struct lnx_delivery* d);

/* After the arch built the frame: block sa_mask (and the signal itself unless
 * SA_NODEFER), and reset the disposition for SA_RESETHAND. */
void lnx_sig_entered(struct task* t, const struct lnx_delivery* d);

/* A synchronous fault in user mode.  If the program handles `sig` (and has
 * not blocked it), fill `d` for immediate delivery and return 1; else 0 and
 * the caller kills the process as before.  A fault on a blocked or ignored
 * signal kills — continuing would re-execute the faulting instruction forever
 * (Linux forces the default action for the same reason). */
int  lnx_sig_fault(struct task* t, int sig, int code, uintptr_t addr,
                   uintptr_t user_sp, struct lnx_delivery* d);

/* rt_sigreturn: the mask saved in the frame comes back (SIGKILL/SIGSTOP can
 * never be blocked, whatever the frame says). */
void lnx_sig_restore_mask(struct task* t, uint32_t kernel_mask);

/* The altstack's state for a ucontext's uc_stack. */
void lnx_sig_altstack(struct task* t, uintptr_t* sp, uintptr_t* size, int* flags);

/* Lifetime: a fork copies the dispositions, a thread SHARES them
 * (CLONE_SIGHAND), exec resets handlers to default (ignored stay ignored),
 * the reaper frees. */
void lnx_sig_fork(struct task* parent, struct task* child);
void lnx_sig_thread(struct task* parent, struct task* child);
void lnx_sig_exec(struct task* t);
void lnx_sig_free(struct task* t);

/* The engine's handlers (abi_engine.c registers them). */
long lnx_h_sigaction(struct abi_ctx* c);
long lnx_h_sigaltstack(struct abi_ctx* c);
long lnx_h_tkill(struct abi_ctx* c);
long lnx_h_tgkill(struct abi_ctx* c);
long lnx_h_kill(struct abi_ctx* c);

/* Per arch (hal/<arch>/lnx_sigframe.c).  x86 passes its int_frame; aarch64
 * its trapframe — hence `void*` here and the real type in each file. */
void lnx_signal_deliver(void* frame);            /* on the way back to user mode */
int  lnx_fault_deliver(void* frame, int sig, uintptr_t addr);   /* 1 = handled */
void lnx_rt_sigreturn(void* frame);

#endif
