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
#include "random.h"
#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE          4096u
/* Per-process user address-space layout (offsets from vmm_user_base()).  Chosen
 * with generous, non-overlapping regions so large programs (a static mbedTLS
 * binary, a C++ program) fit and the DOWN-growing stack never runs into the
 * image (the M25 single-page stack at +1 MiB overflowed into the image — a real
 * TLS handshake needs far more than one page):
 *     +0            image (PIE bias / ET_EXEC vaddr)      up to ~64 MiB
 *     +0x04000000   interpreter (ld.so), dynamic only     ~16 MiB
 *     +0x06000000   stack TOP (grows DOWN), PROC_STACK_PAGES pages
 *     +0x08000000   mmap region (grows UP)                (see usyscall.c)
 */
#define PROC_INTERP_OFFSET 0x04000000u   /* §M37 dynamic linker load base      */
#define PROC_STACK_TOP     0x06000000u   /* one past the highest stack page    */
#define PROC_STACK_PAGES   256u          /* 1 MiB user stack                   */
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
 *     [ auxv pairs ] [ AT_NULL(0,0) ] (AT_PAGESZ/AT_CLKTCK/AT_RANDOM/AT_SECURE;
 *                                      AT_PHDR/AT_ENTRY come with §M37)
 *     ...
 *     [ 16 AT_RANDOM bytes ]          (near the top of the page)
 *     [ argument strings ]            (at the top of the page)
 *
 * The auxv is what a real libc's startup reads: musl needs AT_PAGESZ (its
 * page-size global) and AT_RANDOM (16 bytes seeding the stack-guard canary +
 * malloc) or it faults / runs with a zero page size.  A minimal-but-real auxv
 * is therefore a hard prerequisite for running an unmodified musl binary.
 *
 * The pointer values stored in argv[] are USER virtual addresses (into this
 * same stack page); we write through the frame's kernel (identity) mapping but
 * compute pointers relative to `stack_va`.  Slots are `uintptr_t`-wide, so the
 * *shape* is correct on all three arches; only the i386 crt0 reads argv today
 * (x86_64/aarch64 crt0 still call main() with no args — a valid stack either
 * way).  Returns the user-VA stack pointer to enter at.
 * --------------------------------------------------------------------------- */
static uint32_t u_strlen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }

/* SysV auxiliary-vector types.  The first group a static musl reads; the
 * second group (§M37) is what the DYNAMIC linker (ld.so) reads to find and
 * relocate the main object + itself. */
#define AT_NULL    0
#define AT_PHDR    3       /* program header table VA of the main object       */
#define AT_PHENT   4       /* size of one program header entry                 */
#define AT_PHNUM   5       /* number of program headers                        */
#define AT_PAGESZ  6
#define AT_BASE    7       /* load base of the interpreter (0 if none)         */
#define AT_ENTRY   9       /* entry point of the MAIN object (not the interp)  */
#define AT_CLKTCK  17
#define AT_SECURE  23
#define AT_RANDOM  25

/* §M37 — a loaded program image: the main object (for the auxv the dynamic
 * linker reads) plus, if PT_INTERP was present, the load base of the mapped
 * interpreter and the entry point to actually start at (the interpreter's). */
struct loaded_prog {
    uintptr_t             entry;        /* where to begin (interp entry if dyn) */
    struct elf_load_info  main;         /* the main object's load info          */
    uintptr_t             interp_base;  /* AT_BASE (0 if statically linked)     */
};

/* The 16 AT_RANDOM bytes musl reads at startup (stack-guard canary + malloc /
 * arc4random seed).  §M39: draw them from the kernel CSPRNG so every exec gets
 * cryptographically-strong, non-repeating values (was a weak xorshift). */
static void fill_at_random(uint8_t out[16], uint32_t frame_phys, uintptr_t stack_va) {
    (void)frame_phys; (void)stack_va;
    random_bytes(out, 16);
}

static uintptr_t build_initial_stack(uint32_t frame_phys, uintptr_t stack_va,
                                     int argc, const char* const argv[],
                                     const struct loaded_prog* lp) {
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

    /* 1a. Copy a minimal default ENVIRONMENT below the args (a real per-exec
     *     env is a follow-up; this makes getenv()/`env` meaningful).  Native
     *     crt0 ignores envp; musl reads it. */
    static const char* const default_env[] = { "PATH=/bin", "HOME=/", "TERM=d-os" };
    const int nenv = (int)(sizeof default_env / sizeof default_env[0]);
    uintptr_t env_uva[8];
    for (int i = nenv - 1; i >= 0; i--) {
        uint32_t l = u_strlen(default_env[i]) + 1;
        koff -= l;
        for (uint32_t j = 0; j < l; j++) base[koff + j] = (uint8_t)default_env[i][j];
        env_uva[i] = stack_va + koff;
    }

    /* 1b. Reserve + fill the 16 AT_RANDOM bytes just below the strings and
     *     record their user VA (kept 4-byte aligned). */
    koff -= 16;
    koff &= ~(uintptr_t)0x3;
    fill_at_random(base + koff, frame_phys, stack_va);
    uintptr_t at_random_uva = stack_va + koff;

    /* 2. Lay out the pointer table below the strings, keeping the final SP
     *    16-byte aligned.  Slots: argc + argv[argc] + argv-NULL + envp[nenv] +
     *    envp-NULL + auxv{ PAGESZ, CLKTCK, RANDOM, SECURE,
     *    PHDR, PHENT, PHNUM, BASE, ENTRY, NULL } = 10 pairs. */
    uintptr_t slot = sizeof(uintptr_t);
    uintptr_t nslots = 1 + (uintptr_t)argc + 1 + (uintptr_t)nenv + 1 + (10 * 2);
    koff -= nslots * slot;
    koff &= ~(uintptr_t)0xF;                          /* 16-byte align the SP  */

    uintptr_t* w = (uintptr_t*)(base + koff);
    uintptr_t k = 0;
    w[k++] = (uintptr_t)argc;
    for (int i = 0; i < argc; i++) w[k++] = argv_uva[i];
    w[k++] = 0;                                       /* argv terminator       */
    for (int i = 0; i < nenv; i++) w[k++] = env_uva[i];
    w[k++] = 0;                                       /* envp terminator       */
    w[k++] = AT_PAGESZ; w[k++] = PAGE_SIZE;           /* auxv: page size       */
    w[k++] = AT_CLKTCK; w[k++] = 100;                 /* auxv: HZ (100 ticks/s)*/
    w[k++] = AT_RANDOM; w[k++] = at_random_uva;       /* auxv: 16 random bytes */
    w[k++] = AT_SECURE; w[k++] = 0;                   /* auxv: not setuid      */
    /* §M37: what ld.so needs to locate + relocate the main object and itself.
     *  Harmless for static programs (their crt0 ignores these; AT_BASE=0). */
    w[k++] = AT_PHDR;  w[k++] = lp ? lp->main.phdr_uva  : 0;
    w[k++] = AT_PHENT; w[k++] = lp ? lp->main.phentsize : 0;
    w[k++] = AT_PHNUM; w[k++] = lp ? lp->main.phnum     : 0;
    w[k++] = AT_BASE;  w[k++] = lp ? lp->interp_base    : 0;
    w[k++] = AT_ENTRY; w[k++] = lp ? lp->main.entry     : 0;
    w[k++] = AT_NULL;  w[k++] = 0;                     /* auxv terminator       */
    return stack_va + koff;
}

/* ---------------------------------------------------------------------------
 * §M37 — load a program image into `s`: map the main object at the user base,
 * and — if it carries a PT_INTERP — read the named interpreter (musl's ld.so)
 * from the VFS and map it clear of the main object.  Returns where to begin
 * execution (the interpreter entry for a dynamic program, the main entry for a
 * static one) via lp->entry, plus the auxv info in lp->main / lp->interp_base.
 *
 * The kernel performs NO relocation or symbol resolution: for a dynamic binary
 * it simply hands control to ld.so (in ring 3) with a correct auxv, and ld.so
 * does the rest.  vmm_space_map works on the (possibly inactive) target space,
 * and the interpreter file is read through the global VFS into a kernel buffer,
 * so this is safe to call before switching to `s`.
 * --------------------------------------------------------------------------- */
static int load_program(struct vmm_space* s, const void* image, size_t len,
                        struct loaded_prog* lp) {
    int rc = elf_load_ex(s, image, len, vmm_user_base(), &lp->main);
    if (rc != ELF_OK) return rc;
    lp->entry       = lp->main.entry;
    lp->interp_base = 0;

    if (lp->main.has_interp) {
        struct file* f = vfs_open(lp->main.interp, VFS_RDONLY);
        if (!f) return ELF_ENOLOAD;                   /* interpreter missing   */
        size_t isz = f->inode ? (size_t)f->inode->size : 0;
        if (isz == 0 || isz > (16u << 20)) { vfs_close(f); return ELF_ENOLOAD; }
        uint8_t* iimg = (uint8_t*)kmalloc(isz);
        if (!iimg) { vfs_close(f); return ELF_ENOMEM; }
        ssize_t ird = vfs_read(f, iimg, isz);
        vfs_close(f);
        if (ird < (ssize_t)isz) { kfree(iimg); return ELF_ENOLOAD; }

        struct elf_load_info ii;
        rc = elf_load_ex(s, iimg, isz,
                         vmm_user_base() + PROC_INTERP_OFFSET, &ii);
        kfree(iimg);
        if (rc != ELF_OK) return rc;
        lp->interp_base = ii.load_bias;               /* AT_BASE               */
        lp->entry       = ii.entry;                   /* start in ld.so        */
    }
    return ELF_OK;
}

/* Map the PROC_STACK_PAGES-page user stack (grows down from PROC_STACK_TOP).
 * Returns the TOP page's frame (for build_initial_stack, which writes argc/
 * argv/envp/auxv there) and its VA via *stack_va_out; the pages below it are
 * zeroed scratch for stack growth.  Returns 0 on failure. */
static uint32_t map_user_stack(struct vmm_space* s, uintptr_t* stack_va_out) {
    uintptr_t top_page = vmm_user_base() + PROC_STACK_TOP - PAGE_SIZE;
    uint32_t top_frame = 0;
    for (uint32_t i = 0; i < PROC_STACK_PAGES; i++) {
        uintptr_t va = top_page - (uintptr_t)i * PAGE_SIZE;
        uint32_t fr = pmm_alloc_frame();
        if (!fr) return 0;
        uint8_t* p = (uint8_t*)(uintptr_t)fr;
        for (int b = 0; b < (int)PAGE_SIZE; b++) p[b] = 0;
        if (vmm_space_map(s, va, fr, VMM_USER | VMM_WRITABLE) != 0) {
            pmm_free_frame(fr);
            return 0;
        }
        if (i == 0) top_frame = fr;
    }
    *stack_va_out = top_page;
    return top_frame;
}

/* Shared exec path: load `image` into a fresh space, map a user stack carrying
 * the SysV initial stack (argc/argv/envp/auxv), and run it to SYS_EXIT as a
 * synchronous excursion on the calling task.  argc<=0 → an empty argv. */
static int proc_exec_common(const void* image, size_t len,
                            int argc, const char* const argv[]) {
    struct vmm_space* s = vmm_space_create();
    if (!s) return -1;

    struct loaded_prog lp;
    int rc = load_program(s, image, len, &lp);
    if (rc != ELF_OK) { vmm_space_destroy(s); return rc; }

    /* Multi-page user stack (grows down), clear of the loaded image. */
    uintptr_t stack_va;
    uint32_t stk = map_user_stack(s, &stack_va);
    if (!stk) { vmm_space_destroy(s); return -1; }
    uintptr_t user_sp = build_initial_stack(stk, stack_va, argc, argv, &lp);

    /* Bind the space to this task so the scheduler maintains CR3/TTBR0 across
     * any preemption during the excursion, activate it, then drop to user
     * mode.  Control returns here when the program issues SYS_EXIT. */
    struct task* me = task_current();
    struct vmm_space* prev = me ? me->mm : NULL;
    if (me) { me->mm = s; me->mmap_cursor = 0; }   /* fresh mmap region */
    vmm_space_switch(s);

    enter_user_mode_wrap(lp.entry, user_sp);

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
    struct loaded_prog lp;
    if (load_program(ns, img, sz, &lp) != ELF_OK) {
        vmm_space_destroy(ns); kfree(img); kfree(strbuf); return -1;
    }
    uintptr_t stack_va;
    uint32_t stk = map_user_stack(ns, &stack_va);
    if (!stk) { vmm_space_destroy(ns); kfree(img); kfree(strbuf); return -1; }
    uintptr_t user_sp = build_initial_stack(stk, stack_va, argc, kargv, &lp);

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

    /* 5. Resume in ring 3 at the new entry (one-way).  For a dynamic binary
     *    this is the interpreter's entry; ld.so then jumps to the program. */
    enter_user_mode(lp.entry, user_sp);
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

    struct loaded_prog lp;
    int rc = load_program(s, image, len, &lp);
    if (rc != ELF_OK) { vmm_space_destroy(s); return rc; }

    uintptr_t stack_va;
    uint32_t stk = map_user_stack(s, &stack_va);
    if (!stk) { vmm_space_destroy(s); return -1; }

    struct user_boot* b = (struct user_boot*)kmalloc(sizeof *b);
    if (!b) { vmm_space_destroy(s); return -1; }
    b->space   = s;
    b->entry   = lp.entry;
    b->user_sp = build_initial_stack(stk, stack_va, 0, NULL, &lp);  /* argc=0 */

    struct task* t = task_spawn_arg(name, user_task_bootstrap, b);
    if (!t) { kfree(b); vmm_space_destroy(s); return -1; }
    return t->pid;
}
