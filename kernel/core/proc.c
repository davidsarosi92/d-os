/* =============================================================================
 * proc.c — load + run a static ELF as a user program (M25 stage 2b).
 *
 * Portable across i386 / x86_64 / aarch64: everything arch-specific is behind
 * two seams already established by the ring-3/EL0 self-test —
 * `vmm_space_*` (per-process address spaces, vmm.h) and `enter_user_mode_wrap`
 * (the drop to ring 3 / EL0 that returns on SYS_EXIT, usermode.h).  See proc.h
 * for the excursion-model rationale.
 * ============================================================================= */

#include "proc.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "task.h"
#include "usermode.h"
#include "syscall.h"
#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE          4096u
#define PROC_STACK_OFFSET  0x00100000u   /* user stack 1 MiB above the image */

int proc_exec_elf(const void* image, size_t len) {
    struct vmm_space* s = vmm_space_create();
    if (!s) return -1;

    uintptr_t entry = 0;
    int rc = elf_load(s, image, len, &entry);
    if (rc != ELF_OK) { vmm_space_destroy(s); return rc; }

    /* One-page user stack, mapped clear of the loaded image. */
    uint32_t stk = pmm_alloc_frame();
    if (!stk) { vmm_space_destroy(s); return -1; }
    uintptr_t stack_va = vmm_user_base() + PROC_STACK_OFFSET;
    if (vmm_space_map(s, stack_va, stk, VMM_USER | VMM_WRITABLE) != 0) {
        pmm_free_frame(stk);
        vmm_space_destroy(s);
        return -1;
    }

    /* Bind the space to this task so the scheduler maintains CR3/TTBR0 across
     * any preemption during the excursion, activate it, then drop to user
     * mode.  Control returns here when the program issues SYS_EXIT. */
    struct task* me = task_current();
    struct vmm_space* prev = me ? me->mm : NULL;
    if (me) { me->mm = s; me->mmap_cursor = 0; }   /* fresh mmap region */
    vmm_space_switch(s);

    enter_user_mode_wrap(entry, stack_va + PAGE_SIZE);   /* stack grows down */

    fd_close_all();                    /* reclaim any fds the program opened */
    vmm_space_switch(prev);
    if (me) me->mm = prev;
    vmm_space_destroy(s);
    return 0;
}
