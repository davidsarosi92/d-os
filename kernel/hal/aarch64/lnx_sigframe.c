/* =============================================================================
 * lnx_sigframe.c — the Linux signal frame and rt_sigreturn, aarch64 (§M89
 * rung 2).  The x86_64 twin explains the split; this file is the LAYOUT.
 *
 * Linux/arm64 `struct rt_sigframe`, byte for byte (musl's ucontext_t and a
 * JVM's uc->uc_mcontext.pc are compiled against it):
 *
 *     sp ->   +0     siginfo (128)
 *             +128   ucontext: uc_flags, uc_link, uc_stack (24), uc_sigmask
 *                    (8 used of 128), then uc_mcontext at +176 of the ucontext
 *                    (16-aligned): fault_address, regs[31], sp, pc, pstate,
 *                    and a 4096-byte __reserved area holding an fpsimd_context
 *                    record (magic 0x46508001, 528 bytes: fpsr, fpcr, v0..v31)
 *                    and a zero terminator record
 *             after: a frame record {x29, x30} that x29 points at, so a
 *                    backtrace through the handler walks into the interrupted
 *                    code
 *
 * The handler is entered with x0 = sig, x1 = &info, x2 = &uc, x30 = the
 * sa_restorer trampoline (musl always supplies SA_RESTORER) and sp = the frame.
 * At rt_sigreturn (#139) sp is back at the frame — nothing was pushed, the
 * return address travelled in x30 — so the ucontext is at sp + 128.
 *
 * SP_EL0 is NOT in the trapframe (it is banked; §A1's lesson), so it is read
 * and written with mrs/msr here as in the native signal.c.
 * ============================================================================= */

#include "lnx_signal.h"
#include "lock.h"
#include "task.h"
#include "percpu.h"
#include "vmm.h"
#include "hal_api.h"
#include "syscall.h"
#include "fd.h"
#include "printf.h"
#include <stdint.h>

struct trapframe {
    uint64_t x[31];
    uint64_t _pad;
    uint64_t elr;
    uint64_t spsr;
};

#define INFO_OFF      0
#define UC_OFF        128
#define UC_STACK      16
#define UC_SIGMASK    40
#define UC_MCTX       176
#define MC_FAULT      0
#define MC_REGS       8
#define MC_SP         (8 + 31 * 8)
#define MC_PC         (MC_SP + 8)
#define MC_PSTATE     (MC_PC + 8)
#define MC_RESERVED   288
#define FPSIMD_MAGIC  0x46508001u
#define FPSIMD_SIZE   528
#define UC_SIZE       (UC_MCTX + MC_RESERVED + 4096)
#define FRAME_SIZE    (UC_OFF + UC_SIZE)
#define REC_SIZE      16

#define SPSR_USER_MASK 0xF0000000ull              /* N Z C V */

static inline uint64_t read_sp_el0(void) {
    uint64_t v; __asm__ volatile ("mrs %0, sp_el0" : "=r"(v)); return v;
}
static inline void write_sp_el0(uint64_t v) {
    __asm__ volatile ("msr sp_el0, %0" :: "r"(v));
}
static uint8_t* fpu_area(void* blob) {
    return (uint8_t*)(((uintptr_t)blob + 15u) & ~(uintptr_t)15u);
}
static void wr64(uintptr_t a, uint64_t v) { *(volatile uint64_t*)a = v; }
static void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t*)a = v; }
static uint64_t rd64(uintptr_t a) { return *(volatile uint64_t*)a; }
static uint32_t rd32(uintptr_t a) { return *(volatile uint32_t*)a; }

static int build(struct trapframe* f, struct task* t, const struct lnx_delivery* d, uint64_t fault_addr) {
    if (!d->act.restorer) return -1;
    uintptr_t sp = d->sp & ~(uintptr_t)15;
    uintptr_t rec = sp - REC_SIZE;
    uintptr_t fr = (rec - FRAME_SIZE) & ~(uintptr_t)15;
    if (!vmm_user_access_ok(fr, sp - fr, 1)) return -1;

    for (uintptr_t p = fr; p < fr + FRAME_SIZE; p += 8) wr64(p, 0);

    uintptr_t si = fr + INFO_OFF;
    wr32(si + 0, (uint32_t)d->info.signo);
    wr32(si + 8, (uint32_t)d->info.code);
    if (d->info.code == LNX_SI_USER || d->info.code == LNX_SI_TKILL) {
        wr32(si + 16, (uint32_t)d->info.pid);
        wr32(si + 20, (uint32_t)d->info.uid);
    } else {
        wr64(si + 16, d->info.addr);
    }

    uintptr_t uc = fr + UC_OFF;
    uintptr_t ss_sp, ss_size; int ss_flags;
    lnx_sig_altstack(t, &ss_sp, &ss_size, &ss_flags);
    wr64(uc + UC_STACK, ss_sp);
    wr32(uc + UC_STACK + 8, (uint32_t)(d->on_altstack ? LNX_SS_ONSTACK : ss_flags));
    wr64(uc + UC_STACK + 16, ss_size);
    wr64(uc + UC_SIGMASK, d->old_mask);

    uintptr_t mc = uc + UC_MCTX;
    wr64(mc + MC_FAULT, fault_addr);
    for (int i = 0; i < 31; i++) wr64(mc + MC_REGS + (uintptr_t)i * 8, f->x[i]);
    wr64(mc + MC_SP, read_sp_el0());
    wr64(mc + MC_PC, f->elr);
    wr64(mc + MC_PSTATE, f->spsr);

    /* The FP/SIMD registers are still the task's own. */
    hal_fpu_save(t->fpu_state);
    const uint8_t* fa = fpu_area(t->fpu_state);
    uintptr_t fp = mc + MC_RESERVED;
    wr32(fp + 0, FPSIMD_MAGIC);
    wr32(fp + 4, FPSIMD_SIZE);
    wr32(fp + 8,  *(const uint32_t*)(fa + 520));          /* fpsr */
    wr32(fp + 12, *(const uint32_t*)(fa + 512));          /* fpcr */
    for (int i = 0; i < 512; i++) ((volatile uint8_t*)(fp + 16))[i] = fa[i];
    /* terminator record {0, 0} follows (already zeroed) */

    wr64(rec, f->x[29]);
    wr64(rec + 8, f->x[30]);

    write_sp_el0(fr);
    f->x[0] = (uint64_t)d->sig;
    f->x[1] = si;
    f->x[2] = uc;
    f->x[29] = rec;
    f->x[30] = (uint64_t)d->act.restorer;
    f->elr = (uint64_t)d->act.handler;
    lnx_sig_entered(t, d);
    return 0;
}

void lnx_signal_deliver(void* frame) {
    struct trapframe* f = (struct trapframe*)frame;
    if ((f->spsr & 0xF) != 0) return;
    struct task* t = task_current();
    if (!t || !lnx_sig_deliverable(t)) return;
    struct lnx_delivery d;
    if (!lnx_sig_next(t, (uintptr_t)read_sp_el0(), &d)) return;
    if (build(f, t, &d, 0) != 0) {
        kprintf("signal: pid %d cannot take signal %d (no usable stack or restorer) - killed\n",
                t->pid, d.sig);
        fd_close_all();
        task_exit_code(128 + 11);
    }
}

/* Called for an EL0 abort / undefined instruction.  `sig` is decided by the
 * caller from the ESR; the code follows it. */
int lnx_fault_deliver(void* frame, int sig, uintptr_t addr) {
    struct trapframe* f = (struct trapframe*)frame;
    struct task* t = task_current();
    if (!t || !t->linux_abi || (f->spsr & 0xF) != 0) return 0;
    uint64_t esr;
    __asm__ volatile ("mrs %0, esr_el1" : "=r"(esr));
    uint64_t ec = esr >> 26;
    int code = LNX_SI_KERNEL;
    if (sig == 11) {
        /* DFSC/IFSC 0b0001LL = translation (not mapped), 0b0011LL = permission */
        code = ((esr & 0x3C) == 0x0C) ? LNX_SEGV_ACCERR : LNX_SEGV_MAPERR;
    } else if (sig == 4) {
        code = LNX_ILL_ILLOPN; addr = (uintptr_t)f->elr;
    } else if (sig == 7) {
        code = LNX_BUS_ADRERR;
    } else if (sig == 5) {
        code = LNX_TRAP_BRKPT; addr = (uintptr_t)f->elr;
    }
    (void)ec;
    struct lnx_delivery d;
    if (!lnx_sig_fault(t, sig, code, addr, (uintptr_t)read_sp_el0(), &d)) return 0;
    return build(f, t, &d, sig == 11 || sig == 7 ? addr : 0) == 0;
}

void lnx_rt_sigreturn(void* frame) {
    struct trapframe* f = (struct trapframe*)frame;
    struct task* t = task_current();
    uintptr_t fr = (uintptr_t)read_sp_el0();
    uintptr_t uc = fr + UC_OFF;
    if (!t || !vmm_user_access_ok(fr, FRAME_SIZE, 0)) {
        fd_close_all();
        task_exit_code(128 + 11);
    }
    uintptr_t mc = uc + UC_MCTX;
    for (int i = 0; i < 31; i++) f->x[i] = rd64(mc + MC_REGS + (uintptr_t)i * 8);
    write_sp_el0(rd64(mc + MC_SP));
    f->elr = rd64(mc + MC_PC);
    f->spsr = (f->spsr & ~SPSR_USER_MASK) | (rd64(mc + MC_PSTATE) & SPSR_USER_MASK);
    lnx_sig_restore_mask(t, rd64(uc + UC_SIGMASK));
    uintptr_t fp = mc + MC_RESERVED;
    if (rd32(fp) == FPSIMD_MAGIC && rd32(fp + 4) == FPSIMD_SIZE) {
        uint8_t* fa = fpu_area(t->fpu_state);
        for (int i = 0; i < 512; i++) fa[i] = ((volatile const uint8_t*)(fp + 16))[i];
        *(uint32_t*)(fa + 520) = rd32(fp + 8);            /* fpsr */
        *(uint32_t*)(fa + 512) = rd32(fp + 12);           /* fpcr */
        hal_fpu_restore(t->fpu_state);
    }
}

/* §M89 — the INTERRUPT return path: a thread spinning in user mode gets its
 * signal here instead of at a system call it may never make.
 *
 * THIS IS A PREEMPTION POINT, SO IT MAY SLEEP (2026-09-29).  Every caller
 * runs it after the EOI and after schedule_check — the task can already be
 * switched out right here, and the §M46 force-kill ends tasks right here — and
 * the interrupted context is USER mode, so no kernel lock is held.  That is
 * the state Linux's exit-to-user loop handles signals in, with interrupts
 * back on.  The first version instead raised the preemption count so a frame
 * needing a stack page brought in was not built and the signal went back to
 * pending.  On aarch64 the frame is ~4.7 KB (the 4 KiB FP/SIMD reserve), so it
 * reached below the pages a thread had ever touched on EVERY delivery: the
 * signal was re-posted forever to a thread that never makes a system call —
 * `sigmusl` test 7 failed on every aarch64 run.  Now the ordinary delivery
 * runs here (prefault allowed; an unusable stack kills, exactly as at a
 * syscall).  The old non-sleeping shape stays for the one case where this
 * is NOT a preemption point: a raised preemption count. */
void lnx_signal_deliver_irq(void* frame) {
    struct trapframe* f = (struct trapframe*)frame;
    if (!((f->spsr & 0xF) == 0)) return;
    struct task* t = task_current();
    if (!t || !t->linux_abi || !lnx_sig_deliverable(t)) return;
    if (preempt_count() == 0) {
        hal_intr_enable();
        lnx_signal_deliver(frame);
        hal_intr_disable();
        return;
    }
    struct lnx_delivery d;
    if (!lnx_sig_next(t, (uintptr_t)(read_sp_el0()), &d)) return;
    this_cpu()->preempt_count++;
    int rc = build(f, t, &d, 0);
    this_cpu()->preempt_count--;
    if (rc != 0) lnx_sig_repost(t, d.sig);
}
