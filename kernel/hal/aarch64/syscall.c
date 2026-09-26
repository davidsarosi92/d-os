/* =============================================================================
 * syscall.c — AArch64 SVC syscall dispatcher + EL0 self-test (M21 Phase L).
 *
 * The ARM counterpart of kernel/hal/x86/syscall.c.  The syscall NUMBERS are
 * shared (kernel/includes/syscall.h); only the dispatch is arch-specific
 * because it reads the trapframe's registers.
 *
 * AArch64 syscall ABI (mirrors the Linux convention, the shape a future libc
 * expects):  x8 = syscall number, x0..x5 = arguments, return value in x0,
 * trigger = `svc #0`.  `exceptions.c` decodes ESR_EL1.EC == 0x15 (SVC from
 * AArch64) on the EL0 synchronous vector and calls aarch64_syscall(tf).
 *
 * SYS_EXIT is special: instead of returning to EL0 (which the normal
 * RESTORE_TRAPFRAME/eret path would do) it teleports back to the kernel context
 * that aarch64_enter_user saved, via user_excursion_teleport() — same idea as the x86
 * SYS_EXIT teleport.
 * ============================================================================= */

#include "syscall.h"
#include "console.h"
#include "printf.h"
#include "pmm.h"
#include "usermode.h"
#include "task.h"
#include "proc.h"
#include "vmm.h"        /* copy_str_from_user */
#include "dosgui.h"     /* §M76 — the ring-3 GUI bridge */
#include "drvuser.h"    /* §M76 — the ring-3 driver runtime */
#include "hal_api.h"    /* hal_set_tls_base */
#include "usermode.h"
#include <stdint.h>
#include <stddef.h>

/* Matches the trapframe laid down by vectors.S (see exceptions.c). */
struct trapframe {
    uint64_t x[31];
    uint64_t _pad;
    uint64_t elr;
    uint64_t spsr;
};

/* usermode.S — EL0 entry + the SYS_EXIT teleport. */
void aarch64_enter_user(uint64_t entry_va, uint64_t user_sp, uint64_t* resume);
#include "usermode.h"
#include "task.h"

/* vmm.c — per-process address spaces + EL0 mappings. */
struct vmm_space* aarch64_vmm_create(void);
int  aarch64_vmm_map_user(struct vmm_space* s, uint64_t va, uint64_t pa,
                          uint64_t size, int exec);
void aarch64_vmm_switch(struct vmm_space* s);
void aarch64_vmm_kernel_switch(void);

/* SVC dispatcher.  Called from aarch64_exception_handler for EC == 0x15. */
static void aarch64_syscall_body(struct trapframe* tf);

/* signal.c — §A1 */
void signal_sigreturn(struct trapframe* tf);

/* §1.1 — see the x86 twins: flag the task while servicing an SVC from EL0 so
 * usyscall.c gates the frame's pointer arguments as USER pointers. */
void aarch64_syscall(struct trapframe* tf) {
    struct task* me = task_current();
    int prev = me ? me->in_user_syscall : 0;
    if (me) me->in_user_syscall = 1;
    aarch64_syscall_body(tf);
    if (me) me->in_user_syscall = prev;
}

void linux_syscall_dispatch(struct trapframe* tf);   /* linux_abi.c (A2) */

static void aarch64_syscall_body(struct trapframe* tf) {
    /* A2 — personality routing, the same one-line branch the x86 dispatchers
     * carry: a task exec'd from a package that declares a Linux ABI is serviced
     * by the Linux personality, everything else by the native numbers below. */
    {
        struct task* cur = task_current();
        if (cur && cur->linux_abi) { linux_syscall_dispatch(tf); return; }
    }
    uint64_t num = tf->x[8];
    switch (num) {
        case SYS_PRINT:
            /* x0 = const char* user pointer.  §1.1 — validated copy-in (see
             * sys_print); an unmapped/kernel address returns -1 instead of
             * taking an EL1 data abort. */
            tf->x[0] = (uint64_t)sys_print((const char*)(uintptr_t)tf->x[0]);
            break;
        case SYS_EXIT: {
            /* Tier B — an independent user task ends for good: close fds (still
             * current) + task_exit(); init reaps it (frees its address space).
             * SP_EL1 is this task's own kernel stack, so no TSS-equivalent
             * plumbing is needed — context_switch already tracks it. */
            struct task* cur = task_current();
            if (cur && cur->user_task) {
                fd_close_all();
                task_exit_code((int)tf->x[0]);
            }
            user_excursion_teleport();  /* excursion self-tests: teleport back */
            break;                      /* unreachable */
        }

        case SYS_GETPID:
            tf->x[0] = (uint64_t)(task_current() ? task_current()->pid : -1);
            break;

        /* M25 stage 3 — fd syscalls.  x0/x1/x2 = arg0/arg1/arg2. */
        case SYS_WRITE:
            tf->x[0] = (uint64_t)sys_write((int)tf->x[0],
                          (const void*)(uintptr_t)tf->x[1], (size_t)tf->x[2]);
            break;
        case SYS_READ:
            tf->x[0] = (uint64_t)sys_read((int)tf->x[0],
                          (void*)(uintptr_t)tf->x[1], (size_t)tf->x[2]);
            break;
        case SYS_OPEN:
            tf->x[0] = (uint64_t)sys_open((const char*)(uintptr_t)tf->x[0],
                          (int)tf->x[1]);
            break;
        case SYS_CLOSE:
            tf->x[0] = (uint64_t)sys_close((int)tf->x[0]);
            break;
        case SYS_LSEEK:
            tf->x[0] = (uint64_t)sys_lseek((int)tf->x[0], (long)tf->x[1],
                          (int)tf->x[2]);
            break;
        case SYS_MMAP:
            tf->x[0] = (uint64_t)sys_mmap((size_t)tf->x[0], (int)tf->x[1]);
            break;
        case SYS_MEMFD:
            tf->x[0] = (uint64_t)sys_memfd((size_t)tf->x[0]);
            break;
        case SYS_SOCKETPAIR:
            tf->x[0] = (uint64_t)sys_socketpair((int*)(uintptr_t)tf->x[0]);
            break;
        case SYS_SEND:
            tf->x[0] = (uint64_t)sys_send((int)tf->x[0], (const void*)(uintptr_t)tf->x[1],
                          (size_t)tf->x[2], (int)tf->x[3]);
            break;
        case SYS_RECV:
            tf->x[0] = (uint64_t)sys_recv((int)tf->x[0], (void*)(uintptr_t)tf->x[1],
                          (size_t)tf->x[2], (int*)(uintptr_t)tf->x[3]);
            break;
        case SYS_POLL:
            tf->x[0] = (uint64_t)sys_poll((struct pollfd*)(uintptr_t)tf->x[0],
                          (int)tf->x[1], (int)tf->x[2]);
            break;

        /* ---- §A1: POSIX process model (fork / wait / exec / pipes) -------- */
        case SYS_FORK: {
            /* Build the child's resume state from the trapframe, plus the one
             * register the trapframe does not hold.  Taking an exception from
             * EL0 switches the CPU to SP_EL1 and leaves SP_EL0 banked, so
             * vectors.S never saved it — but the child resumes on that stack
             * in its own address space, so read it here. */
            struct user_regs r;
            for (int i = 0; i < 31; i++) r.x[i] = tf->x[i];
            r.x[0]   = 0;                     /* the child sees fork() == 0 */
            __asm__ volatile ("mrs %0, sp_el0" : "=r"(r.user_sp));
            r.pc     = tf->elr;               /* the instruction after `svc` */
            r.pstate = tf->spsr;
            tf->x[0] = (uint64_t)proc_fork(&r);
            break;
        }
        case SYS_WAITPID: {
            int status = 0;
            int pid = task_wait((int)tf->x[0], &status);
            if (tf->x[1]) *(int*)(uintptr_t)tf->x[1] = status;
            tf->x[0] = (uint64_t)pid;
            break;
        }
        case SYS_EXECVE:      /* on success it does not return */
            tf->x[0] = (uint64_t)proc_execve((const char*)(uintptr_t)tf->x[0],
                                             (char* const*)(uintptr_t)tf->x[1]);
            break;
        case SYS_PIPE:
            tf->x[0] = (uint64_t)sys_pipe((int*)(uintptr_t)tf->x[0]);
            break;
        case SYS_DUP2:
            tf->x[0] = (uint64_t)sys_dup2((int)tf->x[0], (int)tf->x[1]);
            break;

        /* ---- §A1: signals ------------------------------------------------ */
        case SYS_TIMERFD_CREATE:
            tf->x[0] = (uint64_t)sys_timerfd_create();
            break;
        case SYS_TIMERFD_SETTIME:
            tf->x[0] = (uint64_t)sys_timerfd_settime_u((int)tf->x[0], (int)tf->x[1],
                                                       (const uint64_t*)tf->x[2]);
            break;
        case SYS_TIMERFD_GETTIME:
            tf->x[0] = (uint64_t)sys_timerfd_gettime((int)tf->x[0],
                                                     (uint64_t*)tf->x[1]);
            break;
        case SYS_SETITIMER:
            tf->x[0] = (uint64_t)sys_setitimer_u((const uint64_t*)tf->x[0]);
            break;
        case SYS_KILL:
            tf->x[0] = (uint64_t)sys_kill((int)tf->x[0], (int)tf->x[1]);
            break;
        case SYS_SIGACTION:
            tf->x[0] = (uint64_t)sys_sigaction((int)tf->x[0], (long)tf->x[1],
                                               (long)tf->x[2]);
            break;
        case SYS_SIGRETURN:
            /* Restores the pre-handler context; do NOT assign tf->x[0]
             * afterwards — signal_sigreturn already set it to the interrupted
             * syscall's result. */
            signal_sigreturn(tf);
            return;

        /* §M75 — SYS_NANOSLEEP was missing here while both x86 dispatchers had
         * it, so an in-tree-libc program that sleeps did not sleep on ARM: it
         * got -1 back and span at full speed, printing "unknown number 35" on
         * every iteration.  That is not a quiet degradation — a 60-second run
         * of §M75's own memory falsifier produced 55 000 lines of it and buried
         * the output the run existed to read.
         *
         * IT IS THE SMALL END OF A MUCH LARGER GAP, MEASURED AND WRITTEN DOWN
         * RATHER THAN PATCHED OVER: this dispatcher answers 26 of the 60 cases
         * i386 answers.  Sockets, stat/fstat/getdents, clone/futex/set_tls,
         * getrandom and uname are all absent, which means every native program
         * that uses them is silently x86-only — §M70's finding about shell
         * commands, one layer down and never swept.  Closing that is its own
         * piece of work (see PLAN.md); this fixes the one case that was
         * actively drowning the log. */
        case SYS_NANOSLEEP:
            tf->x[0] = (uint64_t)sys_nanosleep((unsigned)tf->x[0]);
            break;

        /* =====================================================================
         * §M76 — THE SWEEP.
         *
         * Before this, the list below did not exist here: `sys_*` cores that
         * are entirely portable had no way in from a native ring-3 program on
         * this architecture, so every program using one was silently x86-only.
         * The failure is a log line and a -1, never a link error, which is why
         * it survived from §M25 to §M75 without anybody noticing.
         *
         * Each of these is a thin argument shuffle over a core that already
         * compiles here — x8 = number, x0..x5 = args, result in x0 — so the
         * meaning of every one of them is defined in exactly one place, the
         * same place i386 calls.  Nothing below is an aarch64 IMPLEMENTATION
         * of anything; it is a doorway to one.
         * =================================================================== */

        /* --- M36 POSIX breadth ------------------------------------------- */
        case SYS_STAT:
            tf->x[0] = (uint64_t)sys_stat((const char*)tf->x[0], (struct kstat*)tf->x[1]);
            break;
        case SYS_FSTAT:
            tf->x[0] = (uint64_t)sys_fstat((int)tf->x[0], (struct kstat*)tf->x[1]);
            break;
        case SYS_GETDENTS:
            tf->x[0] = (uint64_t)sys_getdents((int)tf->x[0], (void*)tf->x[1],
                                              (uint32_t)tf->x[2]);
            break;
        case SYS_UNAME:
            tf->x[0] = (uint64_t)sys_uname((struct kutsname*)tf->x[0]);
            break;
        case SYS_CLOCK_GETTIME:
            tf->x[0] = (uint64_t)sys_clock_gettime((int)tf->x[0],
                                                   (struct ktimespec*)tf->x[1]);
            break;
        case SYS_GETRANDOM:
            tf->x[0] = (uint64_t)sys_getrandom((void*)tf->x[0], (uint32_t)tf->x[1],
                                               (unsigned)tf->x[2]);
            break;

        /* --- M24 stage 6 sockets ------------------------------------------
         * The stack above the transport is arch-independent (net.c), so these
         * are the same nine doorways i386 has.  Their absence is why nothing
         * written against the NATIVE socket API could run on ARM, while musl
         * programs — which arrive through §M50's Linux-ABI engine — could. */
        case SYS_SOCKET:
            tf->x[0] = (uint64_t)sys_socket((int)tf->x[0], (int)tf->x[1], (int)tf->x[2]);
            break;
        case SYS_BIND:
            tf->x[0] = (uint64_t)sys_bind((int)tf->x[0], (uint32_t)tf->x[1], (int)tf->x[2]);
            break;
        case SYS_CONNECT:
            tf->x[0] = (uint64_t)sys_connect((int)tf->x[0], (uint32_t)tf->x[1], (int)tf->x[2]);
            break;
        case SYS_LISTEN:
            tf->x[0] = (uint64_t)sys_listen((int)tf->x[0], (int)tf->x[1]);
            break;
        case SYS_ACCEPT:
            tf->x[0] = (uint64_t)sys_accept((int)tf->x[0], (uint32_t*)tf->x[1],
                                            (int*)tf->x[2]);
            break;
        case SYS_SENDTO:
            tf->x[0] = (uint64_t)sys_sendto((int)tf->x[0], (const void*)tf->x[1],
                                            (uint32_t)tf->x[2], (uint32_t)tf->x[3],
                                            (int)tf->x[4]);
            break;
        case SYS_RECVFROM:
            tf->x[0] = (uint64_t)sys_recvfrom((int)tf->x[0], (void*)tf->x[1],
                                              (uint32_t)tf->x[2], (uint32_t*)tf->x[3],
                                              (int*)tf->x[4]);
            break;
        case SYS_GETSOCKNAME:
            tf->x[0] = (uint64_t)sys_getsockname((int)tf->x[0], (uint32_t*)tf->x[1],
                                                 (int*)tf->x[2]);
            break;
        case SYS_GETPEERNAME:
            tf->x[0] = (uint64_t)sys_getpeername((int)tf->x[0], (uint32_t*)tf->x[1],
                                                 (int*)tf->x[2]);
            break;

        /* --- M35 threads --------------------------------------------------
         * `proc_clone` and the futex are portable; SET_TLS is NOT, and the
         * difference is the interesting part.  On x86 a thread pointer is a
         * SEGMENT DESCRIPTOR, so the call allocates a per-CPU GDT slot, pins
         * the thread to that CPU and returns a SELECTOR.  On aarch64 it is a
         * register — `TPIDR_EL0` — with no table, no pinning and no selector
         * to return, which is also why §A3 found that forgetting to save it on
         * a context switch was invisible until a forked musl child died. */
        case SYS_CLONE:
            tf->x[0] = (uint64_t)proc_clone((uintptr_t)tf->x[0], (uintptr_t)tf->x[1]);
            break;
        case SYS_FUTEX:
            tf->x[0] = (uint64_t)sys_futex((int*)tf->x[0], (int)tf->x[1], (int)tf->x[2]);
            break;
        case SYS_SET_TLS: {
            struct task* t = task_current();
            if (!t) { tf->x[0] = 0; break; }
            t->tls_base = (uintptr_t)tf->x[0];
            t->has_tls  = 1;
            hal_set_tls_base(t->tls_base);
            /* Zero, not a selector: there is nothing here for a caller to load
             * into a segment register.  The in-tree libc ignores the result on
             * this arch; returning a plausible non-zero number would invite it
             * not to. */
            tf->x[0] = 0;
            break;
        }

        /* --- §M42 the dosgui bridge ---------------------------------------
         * These are what §M76's own test client needed: `uidemo` builds a
         * toolkit interface from ring 3, and on this architecture its first
         * call returned `unknown number 53328` and the program exited with
         * "no window (is the GUI running?)" — a message about the GUI, from a
         * program whose syscall had simply not been wired.  *The honest report
         * of a missing doorway looks exactly like a broken subsystem.* */
        case SYS_DOSGUI_CREATE: {
            char title[64];
            if (copy_str_from_user(title, tf->x[2], sizeof title) < 0) title[0] = 0;
            tf->x[0] = (uint64_t)dosgui_create((int)tf->x[0], (int)tf->x[1], title);
            break;
        }
        case SYS_DOSGUI_PRESENT:
            tf->x[0] = (uint64_t)dosgui_present((int)tf->x[0], (const uint32_t*)tf->x[1],
                                                (int)tf->x[2], (int)tf->x[3], (int)tf->x[4]);
            break;
        case SYS_DOSGUI_POLL:
            tf->x[0] = (uint64_t)dosgui_poll((int)tf->x[0], (struct dosgui_event*)tf->x[1]);
            break;
        case SYS_DOSGUI_DESTROY:
            dosgui_destroy((int)tf->x[0]);
            tf->x[0] = 0;
            break;
        case SYS_DOSGUI_UI_BUILD:
            tf->x[0] = (uint64_t)dosgui_ui_build((int)tf->x[0], (const void*)tf->x[1],
                                                 (int)tf->x[2]);
            break;

        /* --- §M33 the ring-3 driver runtime -------------------------------
         * The last ten, and they do NOT all get a doorway — which is the whole
         * of §M33's honesty gate applied to an architecture instead of to a
         * placement: *a boundary you believe in and do not have is worse than
         * one you know you lack.*
         *
         * Seven of them are portable requests about memory, interrupts and
         * devices, and they are wired.  **THREE ARE PORT I/O, AND THIS
         * ARCHITECTURE HAS NO PORT I/O AT ALL** — there is no instruction, no
         * address space and no bitmap for one.  They are REFUSED WITH A REASON
         * rather than stubbed to success, because a driver that asked for a
         * port window and was told "granted" would go on to fault at its first
         * access, arbitrarily far from the call that lied to it. */
        case SYS_DRV_MMIO:
            tf->x[0] = (uint64_t)drvuser_sys_mmio((uint64_t)tf->x[0], (uint64_t)tf->x[1]);
            break;
        case SYS_DRV_DMA: {
            uint64_t dev = 0;
            long r = drvuser_sys_dma((int)tf->x[0], (int)tf->x[1], &dev);
            if (r >= 0 && tf->x[2])
                if (copy_to_user((uintptr_t)tf->x[2], &dev, sizeof dev) != 0) r = -1;
            tf->x[0] = (uint64_t)r;
            break;
        }
        case SYS_DRV_IRQ:
            tf->x[0] = (uint64_t)drvuser_sys_irq((int)tf->x[0]);
            break;
        case SYS_DRV_IRQ_WAIT:
            tf->x[0] = (uint64_t)drvuser_sys_irq_wait((int)tf->x[0], (int)tf->x[1]);
            break;
        case SYS_DRV_LOG: {
            char msg[128];
            int n = copy_str_from_user(msg, tf->x[0], sizeof msg);
            tf->x[0] = (uint64_t)drvuser_sys_log(n < 0 ? NULL : msg);
            break;
        }
        case SYS_DRV_WINDOW: {
            uint64_t phys = 0, len = 0;
            long r = drvuser_sys_window((int)tf->x[0], &phys, &len);
            if (r == 0) {
                uint64_t out[2] = { phys, len };
                if (copy_to_user((uintptr_t)tf->x[1], out, sizeof out) != 0) r = -1;
            }
            tf->x[0] = (uint64_t)r;
            break;
        }
        case SYS_DRV_INPUT:
            tf->x[0] = (uint64_t)drvuser_sys_input((int)tf->x[0], (int)tf->x[1],
                                                   (unsigned)tf->x[2], (int)tf->x[3]);
            break;

        case SYS_DRV_PORTS:
        case SYS_DRV_PORTS_LOCK:
        case SYS_DRV_PORTS_UNLOCK:
            /* Named in the message, so the driver author learns WHICH request
             * this machine cannot serve rather than that "a syscall failed". */
            kprintf("drv: port I/O is not available on aarch64 — this "
                    "architecture has no I/O address space (request %lu)\n",
                    (unsigned long)num);
            tf->x[0] = (uint64_t)-1;
            break;

        default:
            kprintf("syscall: unknown number %lu\n", (unsigned long)num);
            tf->x[0] = (uint64_t)-1;
            break;
    }
}

/* -----------------------------------------------------------------------------
 * EL0 self-test — the AArch64 analogue of the x86 `ringtest` shell command.
 *
 * Creates a private address space, maps a code page (EL0-RX) + stack page
 * (EL0-RW) at VA >= 4 GiB, copies the position-independent user_stub into the
 * code page, switches TTBR0 to the new space, and drops to EL0.  The stub
 * SYS_PRINTs a message then SYS_EXITs, which teleports back here.  Proves the
 * full path: per-process VMM → EL0 entry → SVC → syscall → EL0 exit — the
 * substrate M25 builds real user processes on.
 * --------------------------------------------------------------------------- */
#define USER_CODE_VA  0x0000000100000000ULL   /* 4 GiB — L1 index 4 (kernel = 0..3) */
#define USER_STACK_VA 0x0000000100010000ULL

int aarch64_usertest(void) {
    extern char user_stub_start[], user_stub_end[];
    uint64_t stub_sz = (uint64_t)(user_stub_end - user_stub_start);

    struct vmm_space* sp = aarch64_vmm_create();
    if (!sp) { kprintf("usertest: vmm_create failed\n"); return -1; }

    pmm_phys_t code_pa = pmm_alloc_frame();
    pmm_phys_t stk_pa  = pmm_alloc_frame();
    if (code_pa == PMM_ALLOC_FAIL || stk_pa == PMM_ALLOC_FAIL) {
        kprintf("usertest: pmm OOM\n"); return -1;
    }

    /* Copy the stub into the code frame (identity map: PA == kernel VA). */
    uint8_t* code = (uint8_t*)(uintptr_t)code_pa;
    for (uint64_t i = 0; i < stub_sz; i++) code[i] = ((uint8_t*)user_stub_start)[i];

    if (aarch64_vmm_map_user(sp, USER_CODE_VA,  code_pa, 4096, 1) != 0 ||
        aarch64_vmm_map_user(sp, USER_STACK_VA, stk_pa,  4096, 0) != 0) {
        kprintf("usertest: map_user failed\n"); return -1;
    }

    kprintf("usertest: dropping to EL0 at %p...\n", (void*)USER_CODE_VA);
    aarch64_vmm_switch(sp);
    enter_user_mode_wrap(USER_CODE_VA, USER_STACK_VA + 4096);  /* returns via SYS_EXIT */
    /* §M71 — the teleport does not unwind aarch64_syscall, so without this the
     * hosting task keeps the ring-3 pointer gate.  This one runs from
     * main_entry at BOOT, so pid 0 carried it on every ARM machine until
     * `audit ring3-boundary` reported it.  See proc.h. */
    user_excursion_end();
    aarch64_vmm_kernel_switch();
    kprintf("usertest: back at EL1 (SYS_EXIT teleport OK)\n");
    return 0;
}

/* Portable `ringtest` shell-command hook (usermode.h) — aarch64 drops to EL0. */
int arch_ringtest(void) { return aarch64_usertest(); }

/* usermode.h — arch hook for the portable M25 exec path (proc.c).  The EL0
 * hello program is the position-independent `user_stub` blob (usermode.S);
 * copy it verbatim (its message is embedded + PC-relative), so `base` is
 * irrelevant.  Entry is at offset 0 → e_entry = base. */
size_t arch_user_hello(uint8_t* buf, size_t cap, uintptr_t base) {
    (void)base;
    extern char user_stub_start[], user_stub_end[];
    size_t sz = (size_t)(user_stub_end - user_stub_start);
    if (cap < sz) return 0;
    for (size_t i = 0; i < sz; i++) buf[i] = ((uint8_t*)user_stub_start)[i];
    return sz;
}

/* Portable ring-3/EL0 entry name (usermode.h): x86 provides its own
 * enter_user_mode_wrap; on aarch64 it maps onto the EL0 drop. */
void arch_enter_user_wrap(uintptr_t ip, uintptr_t sp, uintptr_t* resume) {
    aarch64_enter_user((uint64_t)ip, (uint64_t)sp, (uint64_t*)resume);
}
