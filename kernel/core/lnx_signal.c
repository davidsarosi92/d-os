/* =============================================================================
 * lnx_signal.c — Linux signal dispositions, posting and picking (§M89 rung 2).
 * The contract is in lnx_signal.h; the frames are per arch.
 *
 * STORAGE.  Dispositions are a PROCESS property — threads share them (Linux's
 * CLONE_SIGHAND) — so they live in a reference-counted `lnx_sighand` every
 * thread of a process points at.  The pending set and the blocked mask stay
 * where §M34/§M56.1 put them (task->sig_pending / sig_blocked: per THREAD, as
 * Linux has them), with the siginfo of each pending signal and the altstack
 * beside them in a per-task `lnx_sigstate`, allocated on first use so a task
 * that never touches a Linux signal pays one pointer.
 *
 * BIT LAYOUT.  task->sig_pending / sig_blocked keep signal N at bit N (the
 * kernel's convention since §M34); a guest sigset keeps it at bit N-1.  The
 * conversion happens at the guest boundary only (abi_engine.c's two named
 * functions, and here for sa_mask), never in between.
 * ============================================================================= */

#include "lnx_signal.h"
#include "abi.h"
#include "task.h"
#include "syscall.h"
#include "cred.h"
#include "kmalloc.h"
#include "lock.h"
#include "vmm.h"
#include "fd.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

#define E_INVAL  22
#define E_FAULT  14
#define E_SRCH    3
#define E_PERM    1
#define E_NOMEM  12

struct lnx_sighand {
    int                  refs;
    spinlock_t           lock;
    struct lnx_sigaction a[LNX_NSIG];
};

struct lnx_sigstate {
    struct lnx_sighand* sh;
    uint64_t            rt_pending;          /* signals 32..64 at bit (sig-32) */
    uint64_t            rt_blocked;
    uintptr_t           ss_sp, ss_size;
    int                 ss_flags;            /* LNX_SS_DISABLE when none      */
    struct lnx_siginfo  info[LNX_NSIG];
};

/* Linux's default actions.  TERM and CORE both end the process here (there
 * are no core dumps); IGN drops the signal; STOP/CONT are §M72's, acted on at
 * the kill site and never reach the pending set. */
enum { D_TERM, D_IGN };
static int default_action(int sig) {
    switch (sig) {
    case 17: /* SIGCHLD  */ case 23: /* SIGURG */ case 28: /* SIGWINCH */
    case 18: /* SIGCONT  */
        return D_IGN;
    default:
        return D_TERM;
    }
}

static struct lnx_sigstate* state_of(struct task* t, int create) {
    if (!t) return NULL;
    if (t->lsig || !create) return t->lsig;
    struct lnx_sigstate* s = (struct lnx_sigstate*)kcalloc(1, sizeof *s);
    if (!s) return NULL;
    s->ss_flags = LNX_SS_DISABLE;
    struct lnx_sighand* h = (struct lnx_sighand*)kcalloc(1, sizeof *h);
    if (!h) { kfree(s); return NULL; }
    h->refs = 1;
    spin_lock_init(&h->lock);
    /* Seed from the native dispositions so a handler installed through the
     * §M34 interface is not forgotten when the Linux one is first used. */
    for (int i = 1; i < 32; i++) {
        h->a[i].handler  = t->sig_handler[i];
        h->a[i].restorer = t->sig_restorer;
    }
    s->sh = h;
    t->lsig = s;
    return s;
}

static void sighand_put(struct lnx_sighand* h) {
    if (h && __atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL) == 0) kfree(h);
}

/* ---- lifetime -------------------------------------------------------------- */

void lnx_sig_fork(struct task* parent, struct task* child) {
    /* §M90 — the mask of signals 1..31 lives on the task (sig_blocked), not in
     * the Linux state, and was never copied: a child or a new thread started
     * with NOTHING blocked.  Go blocks every signal around clone() so that a
     * new thread cannot take one before it has installed its signal stack;
     * ours could, and the first SIGURG to land in that window found no usable
     * stack and killed the thread (and with it dockerd's containerd start).
     * POSIX: a fork child and a new thread inherit the creator's mask. */
    if (parent && child) child->sig_blocked = parent->sig_blocked;
    struct lnx_sigstate* ps = parent ? parent->lsig : NULL;
    if (!ps || !child) return;
    struct lnx_sigstate* cs = state_of(child, 1);
    if (!cs) return;
    uint32_t fl = spin_lock_irqsave(&ps->sh->lock);
    for (int i = 0; i < LNX_NSIG; i++) cs->sh->a[i] = ps->sh->a[i];
    spin_unlock_irqrestore(&ps->sh->lock, fl);
    cs->ss_sp = ps->ss_sp; cs->ss_size = ps->ss_size; cs->ss_flags = ps->ss_flags;
    cs->rt_blocked = ps->rt_blocked;               /* a fork inherits the mask */
}

void lnx_sig_thread(struct task* parent, struct task* child) {
    if (!parent || !child) return;
    child->sig_blocked = parent->sig_blocked;  /* §M90 — see lnx_sig_fork */
    struct lnx_sigstate* ps = state_of(parent, 1);
    if (!ps) return;
    struct lnx_sigstate* cs = (struct lnx_sigstate*)kcalloc(1, sizeof *cs);
    if (!cs) return;
    cs->ss_flags = LNX_SS_DISABLE;             /* a new thread has no altstack */
    __atomic_add_fetch(&ps->sh->refs, 1, __ATOMIC_ACQ_REL);
    cs->sh = ps->sh;                           /* CLONE_SIGHAND               */
    cs->rt_blocked = ps->rt_blocked;           /* ...and the creator's mask   */
    child->lsig = cs;
}

void lnx_sig_exec(struct task* t) {
    struct lnx_sigstate* s = t ? t->lsig : NULL;
    if (!s) return;
    /* A handler pointed into the old image; an ignored signal stays ignored
     * (POSIX), everything else goes back to default.  A table SHARED with
     * threads that survive the exec is not ours to rewrite — take a copy. */
    struct lnx_sighand* h = (struct lnx_sighand*)kcalloc(1, sizeof *h);
    if (!h) return;
    h->refs = 1;
    spin_lock_init(&h->lock);
    for (int i = 0; i < LNX_NSIG; i++)
        if (s->sh->a[i].handler == SIG_IGN) h->a[i].handler = SIG_IGN;
    sighand_put(s->sh);
    s->sh = h;
    s->ss_flags = LNX_SS_DISABLE;
    s->ss_sp = s->ss_size = 0;
}

void lnx_sig_free(struct task* t) {
    struct lnx_sigstate* s = t ? t->lsig : NULL;
    if (!s) return;
    t->lsig = NULL;
    sighand_put(s->sh);
    kfree(s);
}

/* ---- posting and picking ------------------------------------------------------ */

int lnx_sig_post(struct task* t, int sig, const struct lnx_siginfo* info) {
    if (!t || sig <= 0 || sig >= LNX_NSIG) return -1;
    struct lnx_sigstate* s = state_of(t, 1);
    if (s) {
        struct lnx_siginfo si;
        if (info) si = *info;
        else {
            struct task* me = task_current();
            si.signo = sig; si.code = LNX_SI_USER; si.addr = 0;
            si.pid = me ? me->pid : 0;
            si.uid = me ? cred_uid(&me->cred) : 0;
            if (si.uid < 0) si.uid = 0;
        }
        si.signo = sig;
        s->info[sig] = si;
    }
    int blocked;
    if (sig < 32) {
        __atomic_or_fetch(&t->sig_pending, 1u << sig, __ATOMIC_ACQ_REL);
        blocked = (t->sig_blocked & (1u << sig)) != 0;
    } else {
        if (!s) return -1;
        __atomic_or_fetch(&s->rt_pending, 1ull << (sig - 32), __ATOMIC_ACQ_REL);
        blocked = (s->rt_blocked & (1ull << (sig - 32))) != 0;
    }
    /* A thread asleep in a futex or a sleep must notice now, not at its next
     * natural wake (a JVM handshake waits for exactly this). */
    if (!blocked && t != task_current()) task_signal_wake(t);
    return 0;
}

static uintptr_t pick_stack(struct lnx_sigstate* s, uint32_t flags, uintptr_t sp, int* on_alt) {
    *on_alt = 0;
    if (!(flags & LNX_SA_ONSTACK) || (s->ss_flags & LNX_SS_DISABLE) || !s->ss_size) return sp;
    /* Already on it (a handler interrupted by another): stay where we are. */
    if (sp > s->ss_sp && sp <= s->ss_sp + s->ss_size) { *on_alt = 1; return sp; }
    *on_alt = 1;
    return s->ss_sp + s->ss_size;
}

int lnx_sig_next(struct task* t, uintptr_t user_sp, struct lnx_delivery* d) {
    struct lnx_sigstate* s = state_of(t, 0);
    for (int sig = 1; sig < LNX_NSIG; sig++) {
        if (sig < 32) {
            uint32_t bit = 1u << sig;
            uint32_t pend = __atomic_load_n(&t->sig_pending, __ATOMIC_ACQUIRE);
            if (!(pend & bit)) continue;
            if ((t->sig_blocked & bit) && sig != SIGKILL) continue;
            __atomic_and_fetch(&t->sig_pending, ~bit, __ATOMIC_ACQ_REL);
        } else {
            if (!s) break;
            uint64_t bit = 1ull << (sig - 32);
            if (!(__atomic_load_n(&s->rt_pending, __ATOMIC_ACQUIRE) & bit)) continue;
            if (s->rt_blocked & bit) continue;
            __atomic_and_fetch(&s->rt_pending, ~bit, __ATOMIC_ACQ_REL);
        }

        struct lnx_sigaction act = { 0, 0, 0, 0 };
        if (s) {
            uint32_t fl = spin_lock_irqsave(&s->sh->lock);
            act = s->sh->a[sig];
            spin_unlock_irqrestore(&s->sh->lock, fl);
        } else {
            act.handler = sig < 32 ? t->sig_handler[sig] : SIG_DFL;
        }
        if (sig == SIGKILL) act.handler = SIG_DFL;

        if (act.handler == SIG_IGN) continue;
        if (act.handler == SIG_DFL) {
            if (default_action(sig) == D_IGN) continue;
            fd_close_all();
            task_exit_code(128 + sig);               /* does not return */
        }
        d->sig = sig;
        d->act = act;
        if (s) d->info = s->info[sig];
        else { d->info.signo = sig; d->info.code = LNX_SI_USER; d->info.pid = d->info.uid = 0; d->info.addr = 0; }
        d->info.signo = sig;
        d->old_mask = lnx_sig_blocked64(t);
        d->sp = s ? pick_stack(s, act.flags, user_sp, &d->on_altstack) : user_sp;
        if (!s) d->on_altstack = 0;
        return 1;
    }
    return 0;
}

void lnx_sig_entered(struct task* t, const struct lnx_delivery* d) {
    uint64_t add = d->act.mask;
    if (!(d->act.flags & LNX_SA_NODEFER)) add |= 1ull << (d->sig - 1);
    lnx_sig_set_blocked64(t, lnx_sig_blocked64(t) | add);
    if (d->act.flags & LNX_SA_RESETHAND) {
        struct lnx_sigstate* s = state_of(t, 0);
        if (s) {
            uint32_t fl = spin_lock_irqsave(&s->sh->lock);
            s->sh->a[d->sig].handler = SIG_DFL;
            s->sh->a[d->sig].flags &= ~LNX_SA_SIGINFO;
            spin_unlock_irqrestore(&s->sh->lock, fl);
        }
    }
}

int lnx_sig_fault(struct task* t, int sig, int code, uintptr_t addr,
                  uintptr_t user_sp, struct lnx_delivery* d) {
    struct lnx_sigstate* s = state_of(t, 0);
    if (!s || sig <= 0 || sig >= LNX_NSIG) return 0;
    struct lnx_sigaction act;
    uint32_t fl = spin_lock_irqsave(&s->sh->lock);
    act = s->sh->a[sig];
    spin_unlock_irqrestore(&s->sh->lock, fl);
    if (act.handler == SIG_DFL || act.handler == SIG_IGN) return 0;
    if (lnx_sig_blocked64(t) & (1ull << (sig - 1))) return 0;
    d->sig = sig;
    d->act = act;
    d->info.signo = sig;
    d->info.code = code;
    d->info.addr = addr;
    d->info.pid = d->info.uid = 0;
    d->old_mask = lnx_sig_blocked64(t);
    d->sp = pick_stack(s, act.flags, user_sp, &d->on_altstack);
    return 1;
}

void lnx_sig_restore_mask(struct task* t, uint64_t guest_mask) {
    lnx_sig_set_blocked64(t, guest_mask);
}

uint64_t lnx_sig_blocked64(struct task* t) {
    struct lnx_sigstate* s = state_of(t, 0);
    return ((uint64_t)t->sig_blocked >> 1) | (s ? (s->rt_blocked << 31) : 0);
}
void lnx_sig_set_blocked64(struct task* t, uint64_t g) {
    g &= ~((1ull << (SIGKILL - 1)) | (1ull << (SIGSTOP - 1)));   /* never blockable */
    t->sig_blocked = (uint32_t)(g << 1);
    uint64_t rt = g >> 31;
    struct lnx_sigstate* s = state_of(t, rt != 0);
    if (s) s->rt_blocked = rt;
}
uint64_t lnx_sig_pending64(struct task* t) {
    struct lnx_sigstate* s = state_of(t, 0);
    return ((uint64_t)t->sig_pending >> 1) | (s ? (s->rt_pending << 31) : 0);
}
int lnx_sig_deliverable(struct task* t) {
    if (t->sig_pending & ~t->sig_blocked) return 1;
    struct lnx_sigstate* s = state_of(t, 0);
    return s && (s->rt_pending & ~s->rt_blocked);
}
void lnx_sig_repost(struct task* t, int sig) {
    if (sig < 32) { __atomic_or_fetch(&t->sig_pending, 1u << sig, __ATOMIC_ACQ_REL); return; }
    struct lnx_sigstate* s = state_of(t, 0);
    if (s) __atomic_or_fetch(&s->rt_pending, 1ull << (sig - 32), __ATOMIC_ACQ_REL);
}

void lnx_sig_altstack(struct task* t, uintptr_t* sp, uintptr_t* size, int* flags) {
    struct lnx_sigstate* s = state_of(t, 0);
    *sp = s ? s->ss_sp : 0;
    *size = s ? s->ss_size : 0;
    *flags = s ? s->ss_flags : LNX_SS_DISABLE;
}

/* ---- the syscalls ------------------------------------------------------------ */

/* The guest's struct sigaction for rt_sigaction is the KERNEL's, the same
 * shape on every guest we speak — four fields, the first three a guest word
 * each: handler, flags, restorer, then the 8-byte mask. */
static int read_word(const struct abi_ctx* c, unsigned long p, int i, unsigned long* out) {
    unsigned w = c->map->word_bytes;
    if (!vmm_user_access_ok((uintptr_t)(p + (unsigned long)i * w), w, 0)) return -1;
    if (w == 8) *out = (unsigned long)*(const uint64_t*)(uintptr_t)(p + (unsigned long)i * w);
    else        *out = (unsigned long)*(const uint32_t*)(uintptr_t)(p + (unsigned long)i * w);
    return 0;
}
static int write_word(const struct abi_ctx* c, unsigned long p, int i, unsigned long v) {
    unsigned w = c->map->word_bytes;
    if (!vmm_user_access_ok((uintptr_t)(p + (unsigned long)i * w), w, 1)) return -1;
    if (w == 8) *(uint64_t*)(uintptr_t)(p + (unsigned long)i * w) = (uint64_t)v;
    else        *(uint32_t*)(uintptr_t)(p + (unsigned long)i * w) = (uint32_t)v;
    return 0;
}

long lnx_h_sigaction(struct abi_ctx* c) {
    struct task* t = task_current();
    int sig = (int)c->a[0];
    unsigned long actp = c->a[1], oldp = c->a[2];
    if (!t) return -E_INVAL;
    if (sig <= 0 || sig >= LNX_NSIG) return -E_INVAL;
    if ((sig == SIGKILL || sig == SIGSTOP) && actp) return -E_INVAL;
    struct lnx_sigstate* s = state_of(t, 1);
    if (!s) return -E_NOMEM;
    unsigned w = c->map->word_bytes;

    struct lnx_sigaction old;
    uint32_t fl = spin_lock_irqsave(&s->sh->lock);
    old = s->sh->a[sig];
    spin_unlock_irqrestore(&s->sh->lock, fl);

    if (actp) {
        unsigned long h, flags, rest;
        if (read_word(c, actp, 0, &h) || read_word(c, actp, 1, &flags) || read_word(c, actp, 2, &rest))
            return -E_FAULT;
        uintptr_t mp = (uintptr_t)(actp + 3 * w);
        if (!vmm_user_access_ok(mp, 8, 0)) return -E_FAULT;
        uint64_t gm = 0;
        for (int i = 0; i < 8; i++) gm |= (uint64_t)((const uint8_t*)mp)[i] << (8 * i);
        struct lnx_sigaction na;
        na.handler  = (uintptr_t)h;
        na.flags    = (uint32_t)flags;
        na.restorer = (flags & LNX_SA_RESTORER) ? (uintptr_t)rest : 0;
        na.mask     = gm & ~((1ull << (SIGKILL - 1)) | (1ull << (SIGSTOP - 1)));
        fl = spin_lock_irqsave(&s->sh->lock);
        s->sh->a[sig] = na;
        spin_unlock_irqrestore(&s->sh->lock, fl);
        /* Keep the native view in step: the §M34 delivery path (native tasks)
         * and everything that asks "is this signal handled" read it. */
        if (sig < 32) t->sig_handler[sig] = na.handler;
        /* Setting SIG_IGN discards a pending instance (POSIX). */
        if (na.handler == SIG_IGN) {
            if (sig < 32) __atomic_and_fetch(&t->sig_pending, ~(1u << sig), __ATOMIC_ACQ_REL);
            else          __atomic_and_fetch(&s->rt_pending, ~(1ull << (sig - 32)), __ATOMIC_ACQ_REL);
        }
    }
    if (oldp) {
        if (write_word(c, oldp, 0, old.handler) || write_word(c, oldp, 1, old.flags) ||
            write_word(c, oldp, 2, old.restorer))
            return -E_FAULT;
        uintptr_t mp = (uintptr_t)(oldp + 3 * w);
        if (!vmm_user_access_ok(mp, 8, 1)) return -E_FAULT;
        uint64_t gm = old.mask;
        for (int i = 0; i < 8; i++) ((uint8_t*)mp)[i] = (uint8_t)(gm >> (8 * i));
    }
    return 0;
}

/* stack_t: { void* ss_sp; int ss_flags; size_t ss_size } — the int is padded
 * to a word on 64-bit guests, so it is three guest words everywhere. */
long lnx_h_sigaltstack(struct abi_ctx* c) {
    struct task* t = task_current();
    struct lnx_sigstate* s = state_of(t, 1);
    if (!s) return -E_NOMEM;
    unsigned long ssp = c->a[0], oldp = c->a[1];
    if (oldp) {
        uintptr_t sp_now = 0;           /* on it now?  SS_ONSTACK tells the caller */
        (void)sp_now;
        if (write_word(c, oldp, 0, s->ss_sp) || write_word(c, oldp, 1, (unsigned long)(unsigned)s->ss_flags) ||
            write_word(c, oldp, 2, s->ss_size))
            return -E_FAULT;
    }
    if (ssp) {
        unsigned long sp, flags, size;
        if (read_word(c, ssp, 0, &sp) || read_word(c, ssp, 1, &flags) || read_word(c, ssp, 2, &size))
            return -E_FAULT;
        flags &= 0xFFFFFFFFul;
        if (flags & LNX_SS_DISABLE) {
            s->ss_flags = LNX_SS_DISABLE; s->ss_sp = 0; s->ss_size = 0;
        } else {
            if (flags & ~(unsigned long)LNX_SS_ONSTACK) return -E_INVAL;
            if (size < 2048) return -12;                     /* ENOMEM: MINSIGSTKSZ */
            s->ss_sp = (uintptr_t)sp; s->ss_size = (uintptr_t)size; s->ss_flags = 0;
        }
    }
    return 0;
}

/* Who may signal whom: the §audit#6 rule, plus the other THREADS of the
 * caller's own process (same address space).  pthread_kill is exactly that,
 * and the ancestry rule alone refused it. */
static int may_signal(struct task* me, struct task* t) {
    if (!me || !me->user_task) return 1;
    if (!t->user_task || t->pid == 0 || t->pid == task_reaper_pid()) return 0;
    if (t == me || (t->mm && t->mm == me->mm)) return 1;
    int p = t->ppid;
    for (int i = 0; i < 64 && p > 0; i++) {
        if (p == me->pid) return 1;
        struct task* pt = task_find(p);
        if (!pt) break;
        p = pt->ppid;
    }
    return 0;
}

static long send_to(int tid, int sig, int code) {
    struct task* t = task_find(tid);
    if (!t) return -E_SRCH;
    struct task* me = task_current();
    if (!may_signal(me, t)) return -E_PERM;
    if (sig == 0) return 0;                                  /* existence probe */
    if (sig < 0 || sig >= LNX_NSIG) return -E_INVAL;
    if (sig == SIGSTOP || sig == 20 /* SIGTSTP */ || sig == SIGCONT)
        return sys_kill(tid, sig) == 0 ? 0 : -E_PERM;        /* §M72 acts on these */
    struct lnx_siginfo si;
    si.signo = sig; si.code = code; si.addr = 0;
    si.pid = me ? me->pid : 0;
    si.uid = me ? cred_uid(&me->cred) : 0;
    if (si.uid < 0) si.uid = 0;
    return lnx_sig_post(t, sig, &si) == 0 ? 0 : -E_INVAL;
}

long lnx_h_kill(struct abi_ctx* c)   { return send_to((int)c->a[0], (int)c->a[1], LNX_SI_USER); }
long lnx_h_tkill(struct abi_ctx* c)  { return send_to((int)c->a[0], (int)c->a[1], LNX_SI_TKILL); }
long lnx_h_tgkill(struct abi_ctx* c) { return send_to((int)c->a[1], (int)c->a[2], LNX_SI_TKILL); }
