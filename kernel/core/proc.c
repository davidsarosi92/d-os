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
#include "kmalloc.h"
#include "hal_api.h"
#include "vfs.h"
#include "fd.h"
#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE          4096u
#define PROC_STACK_OFFSET  0x00100000u   /* user stack 1 MiB above the image */
#define PROC_MAX_ARGV      16

/* ---------------------------------------------------------------------------
 * M34 — build the System V initial process stack in a freshly-allocated user
 * stack frame.
 *
 * At program entry the stack pointer must point at `argc`, laid out (growing
 * UP from the SP) as:
 *
 *     [ argc ]                        <- returned user SP
 *     [ argv[0] ] .. [ argv[argc-1] ]
 *     [ NULL ]                        (argv terminator)
 *     [ envp... ] [ NULL ]            (empty env for now)
 *     [ auxv: AT_NULL(0,0) ]          (minimal; AT_PHDR/AT_RANDOM come with §M37/§M39)
 *     ...
 *     [ argument strings ]            (at the top of the page)
 *
 * The pointer values stored in argv[] are USER virtual addresses (into this
 * same stack page); we write through the frame's kernel (identity) mapping but
 * compute pointers relative to `stack_va`.  Slots are `uintptr_t`-wide, so the
 * *shape* is correct on all three arches; only the i386 crt0 reads argv today
 * (x86_64/aarch64 crt0 still call main() with no args — a valid stack either
 * way).  Returns the user-VA stack pointer to enter at.
 * --------------------------------------------------------------------------- */
static uint32_t u_strlen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }

static uintptr_t build_initial_stack(uint32_t frame_phys, uintptr_t stack_va,
                                     int argc, const char* const argv[]) {
    if (argc < 0) argc = 0;
    if (argc > PROC_MAX_ARGV) argc = PROC_MAX_ARGV;

    uint8_t* base = (uint8_t*)(uintptr_t)frame_phys;   /* kernel identity view  */
    for (uint32_t i = 0; i < PAGE_SIZE; i++) base[i] = 0;

    /* 1. Copy the argument strings to the top of the page, recording each
     *    string's USER virtual address. */
    uintptr_t argv_uva[PROC_MAX_ARGV];
    uintptr_t koff = PAGE_SIZE;
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t l = u_strlen(argv[i]) + 1;
        koff -= l;
        for (uint32_t j = 0; j < l; j++) base[koff + j] = (uint8_t)argv[i][j];
        argv_uva[i] = stack_va + koff;
    }

    /* 2. Lay out the pointer table below the strings, keeping the final SP
     *    16-byte aligned.  Slot count: argc + argv[argc] + NULL + envp-NULL +
     *    auxv{type,val}. */
    uintptr_t slot = sizeof(uintptr_t);
    uintptr_t nslots = 1 + (uintptr_t)argc + 1 + 1 + 2;
    koff -= nslots * slot;
    koff &= ~(uintptr_t)0xF;                          /* 16-byte align the SP  */

    uintptr_t* w = (uintptr_t*)(base + koff);
    uintptr_t k = 0;
    w[k++] = (uintptr_t)argc;
    for (int i = 0; i < argc; i++) w[k++] = argv_uva[i];
    w[k++] = 0;                                       /* argv terminator       */
    w[k++] = 0;                                       /* envp terminator       */
    w[k++] = 0;                                       /* auxv AT_NULL type     */
    w[k++] = 0;                                       /* auxv AT_NULL value    */
    return stack_va + koff;
}

/* Shared exec path: load `image` into a fresh space, map a user stack carrying
 * the SysV initial stack (argc/argv/envp/auxv), and run it to SYS_EXIT as a
 * synchronous excursion on the calling task.  argc<=0 → an empty argv. */
static int proc_exec_common(const void* image, size_t len,
                            int argc, const char* const argv[]) {
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
    uintptr_t user_sp = build_initial_stack(stk, stack_va, argc, argv);

    /* Bind the space to this task so the scheduler maintains CR3/TTBR0 across
     * any preemption during the excursion, activate it, then drop to user
     * mode.  Control returns here when the program issues SYS_EXIT. */
    struct task* me = task_current();
    struct vmm_space* prev = me ? me->mm : NULL;
    if (me) { me->mm = s; me->mmap_cursor = 0; }   /* fresh mmap region */
    vmm_space_switch(s);

    enter_user_mode_wrap(entry, user_sp);

    fd_close_all();                    /* reclaim any fds the program opened */
    vmm_space_switch(prev);
    if (me) me->mm = prev;
    vmm_space_destroy(s);
    return 0;
}

int proc_exec_elf(const void* image, size_t len) {
    return proc_exec_common(image, len, 0, NULL);
}

int proc_exec_elf_argv(const void* image, size_t len,
                       int argc, const char* const argv[]) {
    return proc_exec_common(image, len, argc, argv);
}

/* ---------------------------------------------------------------------------
 * M34 — execve(path, argv): replace the calling user process's image.
 *
 * Loads the ELF at `path` from the VFS into a *fresh* address space, builds a
 * new initial stack from `argv` (marshalled out of the OLD space first, since
 * the pointers die when we swap), atomically swaps the task's `mm` to the new
 * space (freeing the old), and resumes ring 3 at the new entry — one way, like
 * a freshly spawned process.  fds survive the exec (POSIX; no O_CLOEXEC yet).
 *
 * Returns -1 (image intact) on any failure up to the commit point; on success
 * it does not return (enter_user_mode).  Portable: only reached via the i386
 * syscall dispatcher today, but the body uses portable primitives.
 * --------------------------------------------------------------------------- */
#define EXECVE_ARGBUF 2048u

int proc_execve(const char* path, char* const uargv[]) {
    struct task* me = task_current();
    if (!me || !me->mm) return -1;           /* only a user process can exec  */

    /* 1. Marshal argv into kernel memory while the OLD space is still active
     *    (user pointers valid).  argv[i] strings are copied into strbuf. */
    const char* kargv[PROC_MAX_ARGV];
    char* strbuf = (char*)kmalloc(EXECVE_ARGBUF);
    if (!strbuf) return -1;
    int argc = 0; uint32_t soff = 0;
    if (uargv) {
        for (argc = 0; argc < PROC_MAX_ARGV && uargv[argc]; argc++) {
            const char* s = uargv[argc];
            uint32_t l = u_strlen(s) + 1;
            if (soff + l > EXECVE_ARGBUF) break;
            for (uint32_t j = 0; j < l; j++) strbuf[soff + j] = s[j];
            kargv[argc] = &strbuf[soff];
            soff += l;
        }
    }

    /* 2. Read the ELF file into a kernel buffer. */
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) { kfree(strbuf); return -1; }
    size_t sz = f->inode ? (size_t)f->inode->size : 0;
    if (sz == 0 || sz > (16u << 20)) { vfs_close(f); kfree(strbuf); return -1; }
    uint8_t* img = (uint8_t*)kmalloc(sz);
    if (!img) { vfs_close(f); kfree(strbuf); return -1; }
    ssize_t rd = vfs_read(f, img, sz);
    vfs_close(f);
    if (rd < (ssize_t)sz) { kfree(img); kfree(strbuf); return -1; }

    /* 3. Build the new address space + initial stack. */
    struct vmm_space* ns = vmm_space_create();
    if (!ns) { kfree(img); kfree(strbuf); return -1; }
    uintptr_t entry = 0;
    if (elf_load(ns, img, sz, &entry) != ELF_OK) {
        vmm_space_destroy(ns); kfree(img); kfree(strbuf); return -1;
    }
    uint32_t stk = pmm_alloc_frame();
    if (!stk) { vmm_space_destroy(ns); kfree(img); kfree(strbuf); return -1; }
    uintptr_t stack_va = vmm_user_base() + PROC_STACK_OFFSET;
    if (vmm_space_map(ns, stack_va, stk, VMM_USER | VMM_WRITABLE) != 0) {
        pmm_free_frame(stk); vmm_space_destroy(ns);
        kfree(img); kfree(strbuf); return -1;
    }
    uintptr_t user_sp = build_initial_stack(stk, stack_va, argc, kargv);

    /* 4. Commit: swap to the new space, free the old one + scratch.  execve
     *    resets signal dispositions to default (custom handlers pointed into
     *    the old image); the restorer is re-registered by the new program. */
    for (int i = 0; i < NSIG; i++) me->sig_handler[i] = SIG_DFL;
    me->sig_pending = 0;
    struct vmm_space* old = me->mm;
    me->mm = ns;
    me->mmap_cursor = 0;
    vmm_space_switch(ns);
    if (old) vmm_space_destroy(old);
    kfree(img);
    kfree(strbuf);

    /* 5. Resume in ring 3 at the new entry (one-way). */
    enter_user_mode(entry, user_sp);
    return 0;                                /* unreachable */
}

/* ---------------------------------------------------------------------------
 * Tier B — proc_spawn: run an ELF as an independent, preemptible user task.
 *
 * The image is loaded into a private space at spawn time (on the caller); the
 * new task's entry is a tiny bootstrap that binds the space, marks itself a
 * user task (so the scheduler routes ring-3→ring-0 to its own kernel stack and
 * SYS_EXIT ends the task), and drops one-way to ring 3.  The task then runs
 * concurrently until SYS_EXIT → task_exit; init reaps it, and task_reap frees
 * the address space.
 * --------------------------------------------------------------------------- */

struct user_boot {
    struct vmm_space* space;
    uintptr_t         entry;
    uintptr_t         user_sp;
};

static void user_task_bootstrap(void) {
    struct user_boot* b = (struct user_boot*)task_start_arg();
    struct task* me = task_current();
    uintptr_t entry = b->entry, sp = b->user_sp;

    me->mm          = b->space;
    me->mmap_cursor = 0;
    me->user_task   = 1;
    kfree(b);

    vmm_space_switch(me->mm);
    /* Point the CPU's ring-3→ring-0 stack at our OWN kernel stack top before
     * the first drop (the scheduler will maintain it on later switch-ins). */
    if (me->kstack_base)
        hal_set_kernel_stack((uintptr_t)me->kstack_base + TASK_KSTACK_SZ);

    enter_user_mode(entry, sp);        /* one-way; ends via SYS_EXIT → task_exit */
}

/* ---------------------------------------------------------------------------
 * M35 — proc_clone: create a THREAD (clone) that shares the caller's address
 * space and fd table, starting in ring 3 at `entry` with stack `stack`.
 *
 * Unlike fork, the address space is SHARED (not copied): both the caller and
 * the new thread see the same memory (that is what makes it a thread).  The
 * thread is marked mm_shared so its reap does not tear the space down; the
 * thread group's owner (the process that created the mm) frees it.  The thread
 * is a child of the caller, reap_owned, so the caller joins it with waitpid().
 *
 * `entry`/`stack` come from the libc (which mmaps the stack and lays out the
 * thread fn's argument + a return-to-exit trampoline before calling clone).
 * --------------------------------------------------------------------------- */
struct clone_boot {
    struct vmm_space* space;
    uintptr_t         entry;
    uintptr_t         user_sp;
    uintptr_t         mmap_cursor;
    struct ofile*     fds[TASK_MAX_FDS];
};

static void clone_bootstrap(void) {
    struct clone_boot* b = (struct clone_boot*)task_start_arg();
    struct task* me = task_current();

    me->mm          = b->space;      /* SHARED with the creator */
    me->mm_shared   = 1;
    me->user_task   = 1;
    me->mmap_cursor = b->mmap_cursor;
    for (int i = 0; i < TASK_MAX_FDS; i++) me->fds[i] = b->fds[i];

    uintptr_t entry = b->entry, sp = b->user_sp;
    kfree(b);

    vmm_space_switch(me->mm);
    if (me->kstack_base)
        hal_set_kernel_stack((uintptr_t)me->kstack_base + TASK_KSTACK_SZ);
    enter_user_mode(entry, sp);      /* → ring 3 at the thread fn; no return */
}

int proc_clone(uintptr_t entry, uintptr_t stack) {
    struct task* parent = task_current();
    if (!parent || !parent->mm) return -1;

    struct clone_boot* b = (struct clone_boot*)kmalloc(sizeof *b);
    if (!b) return -1;
    b->space       = parent->mm;    /* share, don't clone */
    b->entry       = entry;
    b->user_sp     = stack;
    b->mmap_cursor = parent->mmap_cursor;
    for (int i = 0; i < TASK_MAX_FDS; i++)
        b->fds[i] = parent->fds[i] ? ofile_ref(parent->fds[i]) : NULL;

    struct task* t = task_spawn_arg("thread", clone_bootstrap, b);
    if (!t) {
        for (int i = 0; i < TASK_MAX_FDS; i++) if (b->fds[i]) ofile_unref(b->fds[i]);
        kfree(b);
        return -1;
    }
    t->mm_shared = 1;               /* set early too (before it may run) */
    task_set_reap_owned(t, 1);      /* the creator joins it with waitpid() */
    return t->pid;
}

int proc_spawn(const char* name, const void* image, size_t len) {
    struct vmm_space* s = vmm_space_create();
    if (!s) return -1;

    uintptr_t entry = 0;
    int rc = elf_load(s, image, len, &entry);
    if (rc != ELF_OK) { vmm_space_destroy(s); return rc; }

    uint32_t stk = pmm_alloc_frame();
    if (!stk) { vmm_space_destroy(s); return -1; }
    uintptr_t stack_va = vmm_user_base() + PROC_STACK_OFFSET;
    if (vmm_space_map(s, stack_va, stk, VMM_USER | VMM_WRITABLE) != 0) {
        pmm_free_frame(stk);
        vmm_space_destroy(s);
        return -1;
    }

    struct user_boot* b = (struct user_boot*)kmalloc(sizeof *b);
    if (!b) { vmm_space_destroy(s); return -1; }
    b->space   = s;
    b->entry   = entry;
    b->user_sp = build_initial_stack(stk, stack_va, 0, NULL);  /* argc=0 stack */

    struct task* t = task_spawn_arg(name, user_task_bootstrap, b);
    if (!t) { kfree(b); vmm_space_destroy(s); return -1; }
    return t->pid;
}
