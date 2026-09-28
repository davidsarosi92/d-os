/* =============================================================================
 * lnx_sigframe.c — the Linux signal frame and rt_sigreturn, i386 (§M89
 * rung 2).  The x86_64 twin explains the split; this file is the LAYOUT.
 *
 * Linux/i386 `struct rt_sigframe`:
 *
 *     esp ->  +0    pretcode        sa_restorer (musl's __restore_rt)
 *             +4    sig             } the handler's three cdecl arguments —
 *             +8    &info           } they are ON THE STACK on i386, and
 *             +12   &uc             } also in eax/edx/ecx for regparm callers
 *             +16   siginfo (128)
 *             +144  ucontext: uc_flags, uc_link, uc_stack (12), sigcontext
 *                   (88: gs fs es ds edi esi ebp esp ebx edx ecx eax trapno
 *                   err eip cs eflags esp_at_signal ss fpstate oldmask cr2 —
 *                   musl's REG_GS..REG_SS order), uc_sigmask (8 of 128)
 *             ...   fxsave image (512, 16-aligned), what fpstate points at
 *
 * THE FP IMAGE IS FXSAVE, NOT Linux's `struct _fpstate_32` (which wraps it in
 * a legacy 112-byte header).  musl never reads it and this kernel's own
 * sigreturn is its only consumer; a program that parses fpstate would read
 * the wrong offsets — stated here rather than discovered.
 *
 * SEGMENTS ARE NEVER TAKEN FROM THE FRAME.  %gs is this task's TLS selector
 * (§M35); a handler rewriting it would make the return iret #GP in ring 0.
 * ============================================================================= */

#include "lnx_signal.h"
#include "idt.h"
#include "task.h"
#include "vmm.h"
#include "hal_api.h"
#include "syscall.h"
#include "fd.h"
#include "printf.h"
#include <stdint.h>

#define INFO_OFF      16
#define UC_OFF        144
#define UC_STACK      8
#define UC_MCTX       20
#define UC_SIGMASK    (UC_MCTX + 88)
#define UC_SIZE       (UC_SIGMASK + 128)
#define FRAME_SIZE    (UC_OFF + UC_SIZE)
#define FP_SIZE       512

/* The OLD frame (`struct sigframe`), used for a handler WITHOUT SA_SIGINFO —
 * Linux/i386 picks the layout by that flag, and musl's two trampolines
 * (__restore → sigreturn #119, __restore_rt → rt_sigreturn #173) expect the
 * matching one:
 *     +0 pretcode, +4 sig, +8 sigcontext (88), +96 an unused legacy
 *     _fpstate_32 (624, kept so extramask's offset never moves), +720
 *     extramask, +724 retcode[8]; the fxsave image follows. */
#define OLD_SC        8
#define OLD_EXTRAMASK (OLD_SC + 88 + 624)
#define OLD_SIZE      (OLD_EXTRAMASK + 4 + 8)

enum { G_GS, G_FS, G_ES, G_DS, G_EDI, G_ESI, G_EBP, G_ESP, G_EBX, G_EDX, G_ECX,
       G_EAX, G_TRAPNO, G_ERR, G_EIP, G_CS, G_EFL, G_UESP, G_SS, G_FPSTATE,
       G_OLDMASK, G_CR2, G_N };

#define EFLAGS_USER_MASK 0x00000CD5u              /* CF PF AF ZF SF DF OF */

static inline uint8_t* fxarea(void* blob) {
    return (uint8_t*)(((uintptr_t)blob + 15u) & ~(uintptr_t)15u);
}
static void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t*)a = v; }
static uint32_t rd32(uintptr_t a) { return *(volatile uint32_t*)a; }

static void put_sigcontext(uintptr_t sc, struct int_frame* f, const struct lnx_delivery* d,
                           uint32_t trapno, uint32_t err, uint32_t cr2, uintptr_t fp) {
    uint32_t g[G_N] = {0};
    g[G_GS] = f->gs; g[G_FS] = f->fs; g[G_ES] = f->es; g[G_DS] = f->ds;
    g[G_EDI] = f->edi; g[G_ESI] = f->esi; g[G_EBP] = f->ebp; g[G_ESP] = f->user_esp;
    g[G_EBX] = f->ebx; g[G_EDX] = f->edx; g[G_ECX] = f->ecx; g[G_EAX] = f->eax;
    g[G_TRAPNO] = trapno; g[G_ERR] = err; g[G_EIP] = f->eip; g[G_CS] = f->cs;
    g[G_EFL] = f->eflags; g[G_UESP] = f->user_esp; g[G_SS] = f->ss;
    g[G_FPSTATE] = (uint32_t)fp;
    g[G_OLDMASK] = d->old_blocked >> 1;
    g[G_CR2] = cr2;
    for (int i = 0; i < G_N; i++) wr32(sc + (uintptr_t)i * 4, g[i]);
}

/* The old frame for a plain handler(int). */
static int build_old(struct int_frame* f, struct task* t, const struct lnx_delivery* d,
                     uint32_t trapno, uint32_t err, uint32_t cr2) {
    uintptr_t sp = d->sp;
    uintptr_t fp = (sp - FP_SIZE) & ~(uintptr_t)15;
    uintptr_t fr = ((fp - OLD_SIZE) & ~(uintptr_t)15) - 4;
    if (!vmm_user_access_ok(fr, sp - fr, 1)) return -1;
    hal_fpu_save(t->fpu_state);
    const uint8_t* fx = fxarea(t->fpu_state);
    for (int i = 0; i < FP_SIZE; i++) ((volatile uint8_t*)fp)[i] = fx[i];
    for (uintptr_t p = fr; p < fr + OLD_SIZE; p += 4) wr32(p, 0);
    wr32(fr + 0, (uint32_t)d->act.restorer);
    wr32(fr + 4, (uint32_t)d->sig);
    put_sigcontext(fr + OLD_SC, f, d, trapno, err, cr2, fp);
    wr32(fr + OLD_EXTRAMASK, 0);                    /* signals 33..64: none here */
    f->user_esp = (uint32_t)fr;
    f->eip = (uint32_t)d->act.handler;
    f->eax = (uint32_t)d->sig;
    f->eflags &= ~(1u << 10);
    lnx_sig_entered(t, d);
    return 0;
}

static int build(struct int_frame* f, struct task* t, const struct lnx_delivery* d,
                 uint32_t trapno, uint32_t err, uint32_t cr2) {
    if (!d->act.restorer) return -1;
    if (!(d->act.flags & LNX_SA_SIGINFO)) return build_old(f, t, d, trapno, err, cr2);
    uintptr_t sp = d->sp;
    uintptr_t fp = (sp - FP_SIZE) & ~(uintptr_t)15;
    uintptr_t fr = ((fp - FRAME_SIZE) & ~(uintptr_t)15) - 4;
    if (!vmm_user_access_ok(fr, sp - fr, 1)) return -1;

    hal_fpu_save(t->fpu_state);
    const uint8_t* fx = fxarea(t->fpu_state);
    for (int i = 0; i < FP_SIZE; i++) ((volatile uint8_t*)fp)[i] = fx[i];

    for (uintptr_t p = fr; p < fr + FRAME_SIZE; p += 4) wr32(p, 0);
    uintptr_t si = fr + INFO_OFF, uc = fr + UC_OFF;
    wr32(fr + 0,  (uint32_t)d->act.restorer);
    wr32(fr + 4,  (uint32_t)d->sig);
    wr32(fr + 8,  (uint32_t)si);
    wr32(fr + 12, (uint32_t)uc);

    wr32(si + 0, (uint32_t)d->info.signo);
    wr32(si + 8, (uint32_t)d->info.code);
    if (d->info.code == LNX_SI_USER || d->info.code == LNX_SI_TKILL) {
        wr32(si + 12, (uint32_t)d->info.pid);
        wr32(si + 16, (uint32_t)d->info.uid);
    } else {
        wr32(si + 12, (uint32_t)d->info.addr);
    }

    uintptr_t ss_sp, ss_size; int ss_flags;
    lnx_sig_altstack(t, &ss_sp, &ss_size, &ss_flags);
    wr32(uc + UC_STACK + 0, (uint32_t)ss_sp);
    wr32(uc + UC_STACK + 4, (uint32_t)(d->on_altstack ? LNX_SS_ONSTACK : ss_flags));
    wr32(uc + UC_STACK + 8, (uint32_t)ss_size);

    put_sigcontext(uc + UC_MCTX, f, d, trapno, err, cr2, fp);
    wr32(uc + UC_SIGMASK, d->old_blocked >> 1);

    f->user_esp = (uint32_t)fr;
    f->eip = (uint32_t)d->act.handler;
    f->eax = (uint32_t)d->sig;                      /* regparm(3) convention too */
    f->edx = (uint32_t)si;
    f->ecx = (uint32_t)uc;
    f->eflags &= ~(1u << 10);                       /* DF clear */
    lnx_sig_entered(t, d);
    return 0;
}

void lnx_signal_deliver(void* frame) {
    struct int_frame* f = (struct int_frame*)frame;
    if ((f->cs & 3) != 3) return;
    struct task* t = task_current();
    if (!t || !t->sig_pending) return;
    struct lnx_delivery d;
    if (!lnx_sig_next(t, (uintptr_t)f->user_esp, &d)) return;
    if (build(f, t, &d, 0, 0, 0) != 0) {
        kprintf("signal: pid %d cannot take signal %d (no usable stack or restorer) - killed\n",
                t->pid, d.sig);
        fd_close_all();
        task_exit_code(128 + 11);
    }
}

int lnx_fault_deliver(void* frame, int sig, uintptr_t addr) {
    struct int_frame* f = (struct int_frame*)frame;
    struct task* t = task_current();
    if (!t || !t->linux_abi || (f->cs & 3) != 3) return 0;
    int code = LNX_SI_KERNEL;
    if (f->int_no == 14) code = (f->err_code & 1) ? LNX_SEGV_ACCERR : LNX_SEGV_MAPERR;
    else if (f->int_no == 6) code = LNX_ILL_ILLOPN;
    else if (f->int_no == 0) code = LNX_FPE_INTDIV;
    else if (f->int_no == 3) code = LNX_TRAP_BRKPT;
    if (f->int_no != 14) addr = (uintptr_t)f->eip;
    if (f->int_no == 13) addr = 0;
    struct lnx_delivery d;
    if (!lnx_sig_fault(t, sig, code, addr, (uintptr_t)f->user_esp, &d)) return 0;
    return build(f, t, &d, f->int_no, f->err_code, f->int_no == 14 ? (uint32_t)addr : 0) == 0;
}

/* Restore from a sigcontext at `sc`; the mask comes separately. */
static void get_sigcontext(struct int_frame* f, struct task* t, uintptr_t g) {
    f->edi = rd32(g + G_EDI * 4); f->esi = rd32(g + G_ESI * 4);
    f->ebp = rd32(g + G_EBP * 4); f->ebx = rd32(g + G_EBX * 4);
    f->edx = rd32(g + G_EDX * 4); f->ecx = rd32(g + G_ECX * 4);
    f->eax = rd32(g + G_EAX * 4); f->eip = rd32(g + G_EIP * 4);
    f->user_esp = rd32(g + G_ESP * 4);
    f->eflags = (f->eflags & ~EFLAGS_USER_MASK) | (rd32(g + G_EFL * 4) & EFLAGS_USER_MASK);
    uintptr_t fp = rd32(g + G_FPSTATE * 4);
    if (fp && !(fp & 15) && vmm_user_access_ok(fp, FP_SIZE, 0)) {
        uint8_t* fx = fxarea(t->fpu_state);
        for (int i = 0; i < FP_SIZE; i++) fx[i] = ((volatile const uint8_t*)fp)[i];
        *(uint32_t*)(fx + 24) &= 0x0000FFFFu;         /* MXCSR reserved bits */
        hal_fpu_restore(t->fpu_state);
    }
}

/* sigreturn (i386 #119), the old frame's: musl's __restore popped the signal
 * number off the stack first, so esp is the frame + 8. */
void lnx_sigreturn_old(void* frame) {
    struct int_frame* f = (struct int_frame*)frame;
    struct task* t = task_current();
    uintptr_t fr = (uintptr_t)f->user_esp - 8;
    if (!t || !vmm_user_access_ok(fr, OLD_SIZE, 0)) {
        fd_close_all();
        task_exit_code(128 + 11);
    }
    uintptr_t sc = fr + OLD_SC;
    lnx_sig_restore_mask(t, rd32(sc + G_OLDMASK * 4) << 1);
    get_sigcontext(f, t, sc);
}

/* rt_sigreturn (i386 #173): the handler's `ret` popped pretcode, so esp is
 * the frame + 4. */
void lnx_rt_sigreturn(void* frame) {
    struct int_frame* f = (struct int_frame*)frame;
    struct task* t = task_current();
    uintptr_t fr = (uintptr_t)f->user_esp - 4;
    uintptr_t uc = fr + UC_OFF;
    if (!t || !vmm_user_access_ok(fr, FRAME_SIZE, 0)) {
        fd_close_all();
        task_exit_code(128 + 11);
    }
    lnx_sig_restore_mask(t, rd32(uc + UC_SIGMASK) << 1);
    get_sigcontext(f, t, uc + UC_MCTX);
}
