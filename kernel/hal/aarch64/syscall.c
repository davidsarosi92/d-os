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
 * that aarch64_enter_user saved, via aarch64_user_exit() — same idea as the x86
 * SYS_EXIT teleport.
 * ============================================================================= */

#include "syscall.h"
#include "console.h"
#include "printf.h"
#include "pmm.h"
#include "usermode.h"
#include "task.h"
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
void aarch64_enter_user(uint64_t entry_va, uint64_t user_sp);
void aarch64_user_exit(void);

/* vmm.c — per-process address spaces + EL0 mappings. */
struct vmm_space* aarch64_vmm_create(void);
int  aarch64_vmm_map_user(struct vmm_space* s, uint64_t va, uint64_t pa,
                          uint64_t size, int exec);
void aarch64_vmm_switch(struct vmm_space* s);
void aarch64_vmm_kernel_switch(void);

/* SVC dispatcher.  Called from aarch64_exception_handler for EC == 0x15. */
void aarch64_syscall(struct trapframe* tf) {
    uint64_t num = tf->x[8];
    switch (num) {
        case SYS_PRINT: {
            const char* s = (const char*)(uintptr_t)tf->x[0];
            console_write(s);           /* the user page is EL1-readable */
            tf->x[0] = 0;               /* return value */
            break;
        }
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
            aarch64_user_exit();        /* excursion self-tests: teleport back */
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

    uint32_t code_pa = pmm_alloc_frame();
    uint32_t stk_pa  = pmm_alloc_frame();
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
    aarch64_enter_user(USER_CODE_VA, USER_STACK_VA + 4096);   /* returns via SYS_EXIT */
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
void enter_user_mode_wrap(uintptr_t ip, uintptr_t sp) {
    aarch64_enter_user((uint64_t)ip, (uint64_t)sp);
}
