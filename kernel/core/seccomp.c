/* =============================================================================
 * seccomp.c — Linux's system-call filter (seccomp, mode 2 and mode 1), §M90.
 *
 * WHY IT IS ENFORCED AND NOT "ACCEPTED": runc loads Docker's default profile
 * into every container before exec'ing its init — some 300 allowed calls and
 * EPERM for everything else (keyctl, ptrace, mount without the capability…).
 * Answering "success" to that load and then not filtering would report a
 * sandbox that is not there: the worst kind of answer (the §M33 "isolation
 * theatre" rule).  Until this file, seccomp was ENOSYS and runc ran
 * containers WITHOUT the profile — honest, but unconfined.
 *
 * WHAT IS IMPLEMENTED, Linux's semantics throughout:
 *   - seccomp(SECCOMP_SET_MODE_FILTER, flags, prog) and prctl(PR_SET_SECCOMP,
 *     SECCOMP_MODE_FILTER, prog).  Requires no_new_privs or CAP_SYS_ADMIN
 *     (EACCES otherwise).  Flags: TSYNC (every thread of the process gets the
 *     chain; they must not have diverged — checked as "same chain or none"),
 *     LOG, SPEC_ALLOW (accepted: there is no speculation mitigation to turn
 *     off), TSYNC_ESRCH.  NEW_LISTENER (user notification) is refused: there
 *     is no listener side.
 *   - seccomp(SECCOMP_SET_MODE_STRICT) / prctl(PR_SET_SECCOMP, 1): read,
 *     write, exit, rt_sigreturn, everything else SIGKILL.
 *   - SECCOMP_GET_ACTION_AVAIL (what libseccomp asks before it builds a filter)
 *     and SECCOMP_GET_NOTIF_SIZES (EOPNOTSUPP — no notification support).
 *   - the filter: classic BPF over struct seccomp_data { int nr; u32 arch;
 *     u64 instruction_pointer; u64 args[6]; }, VALIDATED at load exactly as
 *     Linux's seccomp_check_filter does (only the instructions seccomp
 *     allows; loads aligned and inside the 64-byte record; jumps forward and
 *     in range; division by a zero constant refused; scratch memory indices
 *     < 16; the last instruction a return), at most 4096 instructions per
 *     filter and 32768 for the chain.
 *   - every filter of the chain runs (newest first) and the MOST RESTRICTIVE
 *     result wins, compared as Linux does (the action as a signed value):
 *       KILL_PROCESS > KILL_THREAD > TRAP > ERRNO > USER_NOTIF > TRACE > LOG > ALLOW
 *   - actions: ALLOW runs the call; LOG runs it and says so on the console;
 *     ERRNO returns -data (capped at 4095) without running it; TRAP posts
 *     SIGSYS (si_code SYS_SECCOMP, si_addr = the call's address) and returns
 *     -ENOSYS; TRACE and USER_NOTIF with no tracer/listener return -ENOSYS (as
 *     Linux does); KILL_THREAD ends the thread and KILL_PROCESS the process,
 *     killed by SIGSYS whatever its handler; an UNKNOWN action is KILL_PROCESS.
 *   - inheritance: the chain lives in the credentials, so fork, clone and every
 *     thread inherit it, and execve keeps it.  It can only grow.
 *   - /proc/<pid>/status reports "Seccomp:" and "NoNewPrivs:" (procfs.c).
 *
 * NOT here, said plainly: SIGSYS's si_syscall / si_arch / si_errno fields are
 * not filled (the signal and its address are); user-space notification;
 * ptrace (so TRACE never has a tracer); the x32 ABI (no such guest here).
 *
 * WHERE IT RUNS: at the top of each arch's Linux-ABI dispatcher, which tests
 * cred.seccomp first — a task without a filter pays one pointer comparison.
 * ============================================================================= */
#include "seccomp.h"
#include "task.h"
#include "cred.h"
#include "kmalloc.h"
#include "lock.h"
#include "printf.h"
#include "uaccess.h"
#include "lnx_signal.h"
#include "vmm.h"
#include <stdint.h>
#include <stddef.h>

struct sock_filter_ { uint16_t code; uint8_t jt, jf; uint32_t k; };

struct seccomp_filter {
    int                    refcount;
    struct seccomp_filter* prev;           /* the older filters (shared) */
    int                    log;            /* SECCOMP_FILTER_FLAG_LOG     */
    int                    strict;         /* mode 1                      */
    uint32_t               total;          /* instructions in the chain   */
    uint16_t               len;
    struct sock_filter_    insn[];
};

#define SECCOMP_RET_KILL_PROCESS 0x80000000u
#define SECCOMP_RET_KILL_THREAD  0x00000000u
#define SECCOMP_RET_TRAP         0x00030000u
#define SECCOMP_RET_ERRNO        0x00050000u
#define SECCOMP_RET_USER_NOTIF   0x7fc00000u
#define SECCOMP_RET_TRACE        0x7ff00000u
#define SECCOMP_RET_LOG          0x7ffc0000u
#define SECCOMP_RET_ALLOW        0x7fff0000u
#define SECCOMP_RET_ACTION_FULL  0xffff0000u
#define SECCOMP_RET_DATA         0x0000ffffu

#define BPF_MAXINSNS   4096
#define CHAIN_MAXINSNS 32768
#define SIGSYS_        31

/* ---- reference counting ---------------------------------------------------- */
void seccomp_get(struct seccomp_filter* f) {
    if (f) __atomic_add_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL);
}
void seccomp_put(struct seccomp_filter* f) {
    while (f) {
        if (__atomic_sub_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL) > 0) return;
        struct seccomp_filter* prev = f->prev;
        kfree(f);
        f = prev;                      /* we held the only reference to prev too */
    }
}

int seccomp_mode(const struct task* t) {
    if (!t || !t->cred.seccomp) return 0;
    return t->cred.seccomp->strict ? 1 : 2;
}

/* ---- validation (Linux's seccomp_check_filter + bpf_check_classic) --------- */
/* classic BPF encoding */
#define BPF_CLASS(c) ((c) & 0x07)
#define BPF_SIZE(c)  ((c) & 0x18)
#define BPF_MODE(c)  ((c) & 0xe0)
#define BPF_OP(c)    ((c) & 0xf0)
#define BPF_SRC(c)   ((c) & 0x08)
enum { LD = 0, LDX = 1, ST = 2, STX = 3, ALU = 4, JMP = 5, RET = 6, MISC = 7 };
#define W_   0x00
#define IMM_ 0x00
#define ABS_ 0x20
#define MEM_ 0x60
#define LEN_ 0x80
#define K_   0x00
#define X_   0x08
#define A_   0x10

static int check_filter(struct sock_filter_* f, unsigned n) {
    if (n == 0 || n > BPF_MAXINSNS) return -22;
    for (unsigned pc = 0; pc < n; pc++) {
        struct sock_filter_* i = &f[pc];
        uint16_t c = i->code;
        switch (BPF_CLASS(c)) {
        case LD:
            if (c == (LD | W_ | ABS_)) {
                if ((i->k & 3) || i->k >= 64) return -22;    /* inside seccomp_data, aligned */
            } else if (c == (LD | W_ | LEN_)) {
                i->code = LD | IMM_; i->k = 64;              /* the record's length */
            } else if (c == (LD | IMM_) || c == (LD | MEM_)) {
                if (c == (LD | MEM_) && i->k >= 16) return -22;
            } else return -22;
            break;
        case LDX:
            if (c == (LDX | W_ | LEN_)) { i->code = LDX | IMM_; i->k = 64; }
            else if (c == (LDX | IMM_)) { }
            else if (c == (LDX | MEM_)) { if (i->k >= 16) return -22; }
            else return -22;
            break;
        case ST: case STX:
            if (c != ST && c != STX) return -22;
            if (i->k >= 16) return -22;
            break;
        case ALU: {
            unsigned op = BPF_OP(c);
            if (op > 0xa0) return -22;
            if ((op == 0x30 || op == 0x90) && BPF_SRC(c) == K_ && i->k == 0) return -22;  /* /0, %0 */
            if (op == 0x80 && BPF_SRC(c) != K_) return -22;  /* NEG takes no source */
            break;
        }
        case JMP: {
            unsigned op = BPF_OP(c);
            if (op == 0x00) {                                /* JA */
                if (BPF_SRC(c) != K_) return -22;
                if (i->k >= n - pc - 1) return -22;
            } else if (op == 0x10 || op == 0x20 || op == 0x30 || op == 0x40) {
                if (pc + 1u + i->jt >= n || pc + 1u + i->jf >= n) return -22;
            } else return -22;
            break;
        }
        case RET:
            if (c != (RET | K_) && c != (RET | A_)) return -22;
            break;
        case MISC:
            if (c != (MISC | 0x00) && c != (MISC | 0x80)) return -22;   /* TAX, TXA */
            break;
        }
    }
    if (BPF_CLASS(f[n - 1].code) != RET) return -22;
    return 0;
}

/* ---- the interpreter -------------------------------------------------------- */
static uint32_t run_filter(const struct seccomp_filter* f, const uint8_t* data) {
    uint32_t A = 0, X = 0, M[16];
    for (int i = 0; i < 16; i++) M[i] = 0;
    for (unsigned pc = 0; pc < f->len; pc++) {
        const struct sock_filter_* i = &f->insn[pc];
        uint16_t c = i->code;
        uint32_t src = BPF_SRC(c) == X_ ? X : i->k;
        switch (BPF_CLASS(c)) {
        case LD:
            if (BPF_MODE(c) == ABS_) {
                A = (uint32_t)data[i->k] | ((uint32_t)data[i->k + 1] << 8) |
                    ((uint32_t)data[i->k + 2] << 16) | ((uint32_t)data[i->k + 3] << 24);
            } else if (BPF_MODE(c) == MEM_) A = M[i->k];
            else A = i->k;
            break;
        case LDX: X = BPF_MODE(c) == MEM_ ? M[i->k] : i->k; break;
        case ST:  M[i->k] = A; break;
        case STX: M[i->k] = X; break;
        case ALU:
            switch (BPF_OP(c)) {
            case 0x00: A += src; break;
            case 0x10: A -= src; break;
            case 0x20: A *= src; break;
            case 0x30: if (!src) return SECCOMP_RET_KILL_PROCESS; A /= src; break;
            case 0x40: A |= src; break;
            case 0x50: A &= src; break;
            case 0x60: A = src >= 32 ? 0 : A << src; break;
            case 0x70: A = src >= 32 ? 0 : A >> src; break;
            case 0x80: A = (uint32_t)-(int32_t)A; break;
            case 0x90: if (!src) return SECCOMP_RET_KILL_PROCESS; A %= src; break;
            case 0xa0: A ^= src; break;
            }
            break;
        case JMP: {
            int take;
            switch (BPF_OP(c)) {
            case 0x00: pc += i->k; continue;
            case 0x10: take = (A == src); break;
            case 0x20: take = (A >  src); break;
            case 0x30: take = (A >= src); break;
            default:   take = (A & src) != 0; break;
            }
            pc += take ? i->jt : i->jf;
            break;
        }
        case RET:  return BPF_SRC(c) == A_ ? A : i->k;
        case MISC: if (c == (MISC | 0x00)) X = A; else A = X; break;
        }
    }
    return SECCOMP_RET_KILL_PROCESS;        /* unreachable: validated to end in RET */
}

/* Is `a` at least as restrictive as `b`?  Linux compares the action part as a
 * SIGNED 32-bit number: lower is more restrictive (KILL_PROCESS is negative). */
static int more_restrictive(uint32_t a, uint32_t b) {
    return (int32_t)(a & SECCOMP_RET_ACTION_FULL) < (int32_t)(b & SECCOMP_RET_ACTION_FULL);
}

/* Linux syscall numbers of read/write/exit/rt_sigreturn per guest, for strict
 * mode (mode 1 — the original seccomp). */
static int strict_allowed(uint32_t arch, unsigned long nr) {
    switch (arch) {
    case SECCOMP_ARCH_X86_64:  return nr == 0 || nr == 1 || nr == 60 || nr == 15;
    case SECCOMP_ARCH_I386:    return nr == 3 || nr == 4 || nr == 1 || nr == 173 || nr == 119;
    case SECCOMP_ARCH_AARCH64: return nr == 63 || nr == 64 || nr == 93 || nr == 139;
    }
    return 0;
}

int seccomp_enter(struct task* t, unsigned long nr, const unsigned long a[6],
                  uintptr_t ip, uint32_t arch, long* ret) {
    struct seccomp_filter* chain = t ? t->cred.seccomp : NULL;
    if (!chain) return 0;
    uint32_t res = SECCOMP_RET_ALLOW;
    int log = 0;
    if (chain->strict) {
        if (!strict_allowed(arch, nr)) res = SECCOMP_RET_KILL_PROCESS;
    } else {
        uint8_t data[64];
        uint32_t n32 = (uint32_t)nr;
        uint64_t ip64 = ip;
        for (int b = 0; b < 4; b++) { data[b] = (uint8_t)(n32 >> (8 * b)); data[4 + b] = (uint8_t)(arch >> (8 * b)); }
        for (int b = 0; b < 8; b++) data[8 + b] = (uint8_t)(ip64 >> (8 * b));
        for (int i = 0; i < 6; i++) {
            uint64_t v = (uint64_t)a[i];
            for (int b = 0; b < 8; b++) data[16 + i * 8 + b] = (uint8_t)(v >> (8 * b));
        }
        for (struct seccomp_filter* f = chain; f; f = f->prev) {
            uint32_t r = run_filter(f, data);
            if (more_restrictive(r, res)) { res = r; log = f->log; }
        }
    }
    uint32_t act = res & SECCOMP_RET_ACTION_FULL, d = res & SECCOMP_RET_DATA;
    switch (act) {
    case SECCOMP_RET_ALLOW:
        return 0;
    case SECCOMP_RET_LOG:
        kprintf("seccomp: pid %d '%s' syscall %lu logged\n", t->pid, t->name, nr);
        return 0;
    case SECCOMP_RET_ERRNO:
        if (log) kprintf("seccomp: pid %d syscall %lu -> errno %u\n", t->pid, nr, d);
        *ret = -(long)(d > 4095 ? 4095 : d);
        return 1;
    case SECCOMP_RET_TRAP: {
        struct lnx_siginfo si;
        si.signo = SIGSYS_; si.code = 1 /* SYS_SECCOMP */; si.pid = 0; si.uid = 0; si.addr = ip;
        lnx_sig_post(t, SIGSYS_, &si);
        *ret = -38;                                         /* ENOSYS */
        return 1;
    }
    case SECCOMP_RET_TRACE:
    case SECCOMP_RET_USER_NOTIF:
        *ret = -38;                                         /* no tracer / listener */
        return 1;
    case SECCOMP_RET_KILL_THREAD:
        kprintf("seccomp: pid %d '%s' killed (thread) at syscall %lu\n", t->pid, t->name, nr);
        task_exit_signal(SIGSYS_);
    default:                                                /* KILL_PROCESS and unknown */
        kprintf("seccomp: pid %d '%s' killed (process) at syscall %lu\n", t->pid, t->name, nr);
        task_exit_group_signal(SIGSYS_);
    }
}

/* ---- installing a filter ---------------------------------------------------- */
#define FLAG_TSYNC        1ul
#define FLAG_LOG          2ul
#define FLAG_SPEC_ALLOW   4ul
#define FLAG_NEW_LISTENER 8ul
#define FLAG_TSYNC_ESRCH  16ul
#define FLAG_WAIT_KILL    32ul

struct tsync_ctx { int tgid; struct seccomp_filter* old; int diverged; int self; };
static void tsync_check(const struct task* t, int is_current, void* vc) {
    (void)is_current;
    struct tsync_ctx* c = (struct tsync_ctx*)vc;
    if (t->state == TASK_DEAD || task_tgid(t) != c->tgid || t->pid == c->self) return;
    if (t->cred.seccomp != c->old && !c->diverged) c->diverged = t->pid;
}
struct tsync_set { int tgid; int self; struct seccomp_filter* nf; int moved; };
static void tsync_apply(const struct task* t, int is_current, void* vc) {
    (void)is_current;
    struct tsync_set* c = (struct tsync_set*)vc;
    if (t->state == TASK_DEAD || task_tgid(t) != c->tgid || t->pid == c->self) return;
    struct task* w = (struct task*)t;
    __atomic_add_fetch(&c->nf->refcount, 1, __ATOMIC_ACQ_REL);
    w->cred.seccomp = c->nf;
    c->moved++;            /* its reference to the old chain is dropped by the caller */
}

static long install(struct seccomp_filter* nf, unsigned long flags) {
    struct task* me = task_current();
    if (!me) { kfree(nf); return -22; }
    if (!me->cred.no_new_privs && !cred_capable(&me->cred, CAP_SYS_ADMIN_)) {
        kfree(nf);
        return -13;                                          /* EACCES */
    }
    struct seccomp_filter* old = me->cred.seccomp;
    if (old && old->strict) { kfree(nf); return -22; }       /* strict cannot be widened */
    nf->prev = old;                                          /* takes our reference */
    nf->total = nf->len + (old ? old->total : 0);
    if (nf->total > CHAIN_MAXINSNS) { nf->prev = NULL; kfree(nf); return -12; }   /* ENOMEM */
    nf->refcount = 1;
    if (flags & FLAG_TSYNC) {
        struct tsync_ctx c = { task_tgid(me), old, 0, me->pid };
        task_for_each(tsync_check, &c);
        if (c.diverged) {
            nf->prev = NULL; kfree(nf);
            return (flags & FLAG_TSYNC_ESRCH) ? -3 : c.diverged;   /* the offending tid */
        }
        /* Every other thread had exactly `old`: each gets the new chain, and
         * the reference each held to `old` is dropped AFTER the walk (no free
         * under the task list's lock).  `old` stays alive through nf->prev. */
        struct tsync_set st = { task_tgid(me), me->pid, nf, 0 };
        task_for_each(tsync_apply, &st);
        for (int i = 0; i < st.moved; i++) seccomp_put(old);
    }
    me->cred.seccomp = nf;
    return 0;
}

/* Copy and validate a struct sock_fprog { u16 len; <pad>; struct sock_filter* }
 * from the guest (the pointer at offset 4 on a 32-bit guest, 8 on 64-bit). */
static struct seccomp_filter* load_prog(uintptr_t uprog, unsigned wb, long* err) {
    uint8_t hdr[16];
    unsigned hl = wb == 4 ? 8 : 16;
    if (!uprog || !vmm_user_access_ok(uprog, hl, 0) || copy_from_user(hdr, uprog, hl) != 0) {
        *err = -14; return NULL;
    }
    unsigned len = (unsigned)hdr[0] | ((unsigned)hdr[1] << 8);
    uintptr_t ins = 0;
    for (int b = (int)wb - 1; b >= 0; b--) ins = (ins << 8) | hdr[wb + (unsigned)b];
    if (len == 0 || len > BPF_MAXINSNS) { *err = -22; return NULL; }
    size_t bytes = (size_t)len * sizeof(struct sock_filter_);
    struct seccomp_filter* f = (struct seccomp_filter*)kcalloc(1, sizeof *f + bytes);
    if (!f) { *err = -12; return NULL; }
    if (!vmm_user_access_ok(ins, bytes, 0) || copy_from_user(f->insn, ins, bytes) != 0) {
        kfree(f); *err = -14; return NULL;
    }
    f->len = (uint16_t)len;
    if (check_filter(f->insn, len) != 0) { kfree(f); *err = -22; return NULL; }
    return f;
}

static long set_strict(void) {
    struct seccomp_filter* f = (struct seccomp_filter*)kcalloc(1, sizeof *f);
    if (!f) return -12;
    f->strict = 1;
    struct task* me = task_current();
    if (!me) { kfree(f); return -22; }
    if (me->cred.seccomp) { kfree(f); return -22; }   /* already filtered: EINVAL */
    f->refcount = 1;
    me->cred.seccomp = f;
    return 0;
}

long seccomp_syscall(unsigned op, unsigned long flags, uintptr_t uargs, unsigned wb) {
    switch (op) {
    case 0:                                              /* SECCOMP_SET_MODE_STRICT */
        if (flags || uargs) return -22;
        return set_strict();
    case 1: {                                            /* SECCOMP_SET_MODE_FILTER */
        if (flags & ~(FLAG_TSYNC | FLAG_LOG | FLAG_SPEC_ALLOW | FLAG_NEW_LISTENER |
                      FLAG_TSYNC_ESRCH | FLAG_WAIT_KILL)) return -22;
        if (flags & FLAG_NEW_LISTENER) {
            /* libseccomp PROBES for it before every filter load: said once */
            static int said;
            if (!said++) kprintf("seccomp: user-space notification (NEW_LISTENER) is not supported\n");
            return -22;
        }
        long err = 0;
        struct seccomp_filter* f = load_prog(uargs, wb, &err);
        if (!f) return err;
        f->log = (flags & FLAG_LOG) ? 1 : 0;
        return install(f, flags);
    }
    case 2: {                                            /* SECCOMP_GET_ACTION_AVAIL */
        if (flags) return -22;
        uint32_t act = 0;
        if (!vmm_user_access_ok(uargs, 4, 0) || copy_from_user(&act, uargs, 4) != 0) return -14;
        switch (act) {
        case SECCOMP_RET_KILL_PROCESS: case SECCOMP_RET_KILL_THREAD: case SECCOMP_RET_TRAP:
        case SECCOMP_RET_ERRNO: case SECCOMP_RET_TRACE: case SECCOMP_RET_LOG:
        case SECCOMP_RET_ALLOW:
            return 0;
        }
        return -95;                                      /* EOPNOTSUPP (USER_NOTIF too) */
    }
    case 3:                                              /* SECCOMP_GET_NOTIF_SIZES */
        return -95;
    }
    return -22;
}

long seccomp_prctl_set(unsigned long mode, uintptr_t uprog, unsigned wb) {
    if (mode == 1) return set_strict();
    if (mode == 2) {
        long err = 0;
        struct seccomp_filter* f = load_prog(uprog, wb, &err);
        if (!f) return err;
        return install(f, 0);
    }
    return -22;
}
