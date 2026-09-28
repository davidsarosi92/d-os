/* =============================================================================
 * lnx_sigframe.c — the Linux signal frame and rt_sigreturn, x86_64 (§M89
 * rung 2).  Which signal and what to do with it is lnx_signal.c's; this file
 * is only the LAYOUT, which is the one thing that differs per architecture.
 *
 * The frame is Linux's `struct rt_sigframe`, byte for byte, because the
 * handler's libc and the handler itself read it with their own compiled-in
 * offsets — musl's ucontext_t, and HotSpot's uc->uc_mcontext.gregs[REG_RIP]:
 *
 *     rsp ->  +0    pretcode        the sa_restorer trampoline (rt_sigreturn)
 *             +8    ucontext        uc_flags, uc_link, uc_stack (24),
 *                                   uc_mcontext = sigcontext (256: 23 gregs in
 *                                   REG_R8..REG_CR2 order, fpregs pointer,
 *                                   8 reserved words), uc_sigmask
 *             +432  siginfo (128)
 *             ...   fxsave image (512, 64-aligned), what fpregs points at
 *
 * Linux's ucontext has an 8-byte uc_sigmask; the libcs declare 128 bytes, so
 * 128 are reserved and the first 8 used.  At entry the stack pointer is
 * 8 mod 16, exactly as after a call instruction, and the direction flag is
 * clear — both are ABI requirements of the handler's first instruction.
 *
 * WHAT A HANDLER CHANGES COMES BACK.  rt_sigreturn restores every general
 * register, rip, rsp, the arithmetic flags and the FP state FROM THE FRAME, so
 * a handler that rewrites the saved rip resumes elsewhere — HotSpot's
 * implicit null checks are exactly that.  The privileged flag bits (IF, IOPL)
 * are never taken from the frame.
 * ============================================================================= */

#include "lnx_signal.h"
#include "idt.h"
#include "task.h"
#include "vmm.h"
#include "hal_api.h"
#include "fd.h"
#include "syscall.h"
#include "printf.h"
#include <stdint.h>

#define UC_OFF        8
#define UC_STACK      16
#define UC_MCTX       40
#define UC_FPREGS     (UC_MCTX + 23 * 8)
#define UC_SIGMASK    (UC_MCTX + 256)
#define UC_SIZE       (UC_SIGMASK + 128)
#define INFO_OFF      (UC_OFF + UC_SIZE)
#define FRAME_SIZE    (INFO_OFF + 128)
#define FP_SIZE       512

enum { G_R8, G_R9, G_R10, G_R11, G_R12, G_R13, G_R14, G_R15, G_RDI, G_RSI,
       G_RBP, G_RBX, G_RDX, G_RAX, G_RCX, G_RSP, G_RIP, G_EFL, G_CSGSFS,
       G_ERR, G_TRAPNO, G_OLDMASK, G_CR2, G_N };

#define RFLAGS_USER_MASK 0x0000000000000CD5ull   /* CF PF AF ZF SF DF OF */

static inline uint8_t* fxarea(void* blob) {
    return (uint8_t*)(((uintptr_t)blob + 15u) & ~(uintptr_t)15u);
}

static void wr64(uintptr_t a, uint64_t v) { *(volatile uint64_t*)a = v; }
static void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t*)a = v; }
static uint64_t rd64(uintptr_t a) { return *(volatile uint64_t*)a; }

/* Build the frame for `d` on the task's user stack and point `f` at the
 * handler.  0 on success; -1 when the stack cannot hold it (the caller then
 * kills the process — a handler with nowhere to run cannot run). */
static int build(struct int_frame* f, struct task* t, const struct lnx_delivery* d,
                 uint64_t trapno, uint64_t err, uint64_t cr2) {
    if (!d->act.restorer) return -1;               /* no trampoline: cannot return */
    uintptr_t sp = d->sp;
    if (!d->on_altstack) sp -= 128;                /* the interrupted code's red zone */
    uintptr_t fp = (sp - FP_SIZE) & ~(uintptr_t)63;
    uintptr_t fr = ((fp - FRAME_SIZE) & ~(uintptr_t)15) - 8;
    if (!vmm_user_access_ok(fr, (sp - fr), 1)) return -1;

    /* The FP registers are still the task's own — save them, copy them out. */
    hal_fpu_save(t->fpu_state);
    const uint8_t* fx = fxarea(t->fpu_state);
    for (int i = 0; i < FP_SIZE; i++) ((volatile uint8_t*)fp)[i] = fx[i];

    for (uintptr_t p = fr; p < fr + FRAME_SIZE; p += 8) wr64(p, 0);
    wr64(fr, (uint64_t)d->act.restorer);

    uintptr_t uc = fr + UC_OFF;
    uintptr_t ss_sp, ss_size; int ss_flags;
    lnx_sig_altstack(t, &ss_sp, &ss_size, &ss_flags);
    wr64(uc + UC_STACK, ss_sp);
    wr32(uc + UC_STACK + 8, (uint32_t)(d->on_altstack ? LNX_SS_ONSTACK : ss_flags));
    wr64(uc + UC_STACK + 16, ss_size);

    uintptr_t g = uc + UC_MCTX;
    uint64_t gr[G_N] = {0};
    gr[G_R8] = f->r8;   gr[G_R9] = f->r9;   gr[G_R10] = f->r10; gr[G_R11] = f->r11;
    gr[G_R12] = f->r12; gr[G_R13] = f->r13; gr[G_R14] = f->r14; gr[G_R15] = f->r15;
    gr[G_RDI] = f->rdi; gr[G_RSI] = f->rsi; gr[G_RBP] = f->rbp; gr[G_RBX] = f->rbx;
    gr[G_RDX] = f->rdx; gr[G_RAX] = f->rax; gr[G_RCX] = f->rcx; gr[G_RSP] = f->rsp;
    gr[G_RIP] = f->rip; gr[G_EFL] = f->rflags;
    gr[G_CSGSFS] = (f->cs & 0xFFFF) | ((uint64_t)(f->ss & 0xFFFF) << 48);
    gr[G_ERR] = err; gr[G_TRAPNO] = trapno;
    gr[G_OLDMASK] = (uint64_t)d->old_blocked >> 1;
    gr[G_CR2] = cr2;
    for (int i = 0; i < G_N; i++) wr64(g + (uintptr_t)i * 8, gr[i]);
    wr64(uc + UC_FPREGS, fp);
    wr64(uc + UC_SIGMASK, (uint64_t)d->old_blocked >> 1);   /* guest bit layout */

    uintptr_t si = fr + INFO_OFF;
    wr32(si + 0, (uint32_t)d->info.signo);
    wr32(si + 4, 0);                                          /* si_errno */
    wr32(si + 8, (uint32_t)d->info.code);
    if (d->info.code == LNX_SI_USER || d->info.code == LNX_SI_TKILL) {
        wr32(si + 16, (uint32_t)d->info.pid);
        wr32(si + 20, (uint32_t)d->info.uid);
    } else {
        wr64(si + 16, d->info.addr);                          /* si_addr */
    }

    f->rdi = (uint64_t)d->sig;
    f->rsi = si;
    f->rdx = uc;
    f->rax = 0;
    f->rsp = fr;
    f->rip = (uint64_t)d->act.handler;
    f->rflags &= ~(1ull << 10);                               /* DF clear */
    lnx_sig_entered(t, d);
    return 0;
}

/* On the way back to user mode (syscall return). */
void lnx_signal_deliver(void* frame) {
    struct int_frame* f = (struct int_frame*)frame;
    if ((f->cs & 3) != 3) return;
    struct task* t = task_current();
    if (!t || !t->sig_pending) return;
    struct lnx_delivery d;
    if (!lnx_sig_next(t, (uintptr_t)f->rsp, &d)) return;
    if (build(f, t, &d, 0, 0, 0) != 0) {
        kprintf("signal: pid %d cannot take signal %d (no usable stack or restorer) - killed\n",
                t->pid, d.sig);
        fd_close_all();
        task_exit_code(128 + 11);
    }
}

/* A synchronous fault from user mode: 1 = a handler will run (return to it),
 * 0 = not handled (the caller kills). */
int lnx_fault_deliver(void* frame, int sig, uintptr_t addr) {
    struct int_frame* f = (struct int_frame*)frame;
    struct task* t = task_current();
    if (!t || !t->linux_abi || (f->cs & 3) != 3) return 0;
    int code = LNX_SI_KERNEL;
    if (f->int_no == 14) code = (f->err_code & 1) ? LNX_SEGV_ACCERR : LNX_SEGV_MAPERR;
    else if (f->int_no == 6)  code = LNX_ILL_ILLOPN;
    else if (f->int_no == 0)  code = LNX_FPE_INTDIV;
    else if (f->int_no == 3)  code = LNX_TRAP_BRKPT;
    if (f->int_no != 14) addr = (uintptr_t)f->rip;
    if (f->int_no == 13) addr = 0;                  /* #GP has no address: SI_KERNEL */
    struct lnx_delivery d;
    if (!lnx_sig_fault(t, sig, code, addr, (uintptr_t)f->rsp, &d)) return 0;
    return build(f, t, &d, f->int_no, f->err_code, f->int_no == 14 ? addr : 0) == 0;
}

/* rt_sigreturn (x86_64 #15).  The handler's `ret` popped pretcode, so rsp
 * points at the ucontext. */
void lnx_rt_sigreturn(void* frame) {
    struct int_frame* f = (struct int_frame*)frame;
    struct task* t = task_current();
    uintptr_t uc = (uintptr_t)f->rsp;
    if (!t || !vmm_user_access_ok(uc, UC_SIZE, 0)) {
        fd_close_all();
        task_exit_code(128 + 11);
    }
    uintptr_t g = uc + UC_MCTX;
    f->r8  = rd64(g + G_R8 * 8);  f->r9  = rd64(g + G_R9 * 8);
    f->r10 = rd64(g + G_R10 * 8); f->r11 = rd64(g + G_R11 * 8);
    f->r12 = rd64(g + G_R12 * 8); f->r13 = rd64(g + G_R13 * 8);
    f->r14 = rd64(g + G_R14 * 8); f->r15 = rd64(g + G_R15 * 8);
    f->rdi = rd64(g + G_RDI * 8); f->rsi = rd64(g + G_RSI * 8);
    f->rbp = rd64(g + G_RBP * 8); f->rbx = rd64(g + G_RBX * 8);
    f->rdx = rd64(g + G_RDX * 8); f->rax = rd64(g + G_RAX * 8);
    f->rcx = rd64(g + G_RCX * 8); f->rsp = rd64(g + G_RSP * 8);
    f->rip = rd64(g + G_RIP * 8);
    f->rflags = (f->rflags & ~RFLAGS_USER_MASK) | (rd64(g + G_EFL * 8) & RFLAGS_USER_MASK);
    lnx_sig_restore_mask(t, (uint32_t)(rd64(uc + UC_SIGMASK) << 1));
    uintptr_t fp = (uintptr_t)rd64(uc + UC_FPREGS);
    if (fp && !(fp & 15) && vmm_user_access_ok(fp, FP_SIZE, 0)) {
        uint8_t* fx = fxarea(t->fpu_state);
        for (int i = 0; i < FP_SIZE; i++) fx[i] = ((volatile const uint8_t*)fp)[i];
        /* MXCSR's reserved bits would #GP the FXRSTOR in ring 0. */
        *(uint32_t*)(fx + 24) &= 0x0000FFFFu;
        hal_fpu_restore(t->fpu_state);
    }
}
