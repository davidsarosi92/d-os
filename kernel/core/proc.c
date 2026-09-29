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
#include "kmap.h"
#include "task.h"
#include "usermode.h"
#include "printf.h"
#include "syscall.h"
#include "kmalloc.h"
#include "hal_api.h"
#include "vfs.h"
#include "fd.h"
#include "random.h"
#include "config.h"
#include "settings.h"
#include "cred.h"
#include "users.h"
#include "lnx_signal.h"
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
#define PROC_MAX_ARGV      64            /* §M89: was 16 — a real program passes more */
#define PROC_MAX_ENV       64

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

/* §1.1 — copy a NUL-terminated string from the USER pointer `us` into `dst`
 * (≤ max, always NUL-terminated), validating each page so a bad execve argv
 * string can't fault the kernel.  Returns the length, or -1 on a bad pointer. */
static int u_strcopy(char* dst, const char* us, uint32_t max) {
    if (max == 0) return -1;
    uintptr_t base = (uintptr_t)us, last = ~(uintptr_t)0;
    for (uint32_t i = 0; i + 1 < max; i++) {
        uintptr_t pg = (base + i) & ~(uintptr_t)0xFFF;
        if (pg != last) { if (!vmm_user_access_ok(pg, 1, 0)) return -1; last = pg; }
        char c = *(const volatile char*)(uintptr_t)(base + i);
        dst[i] = c;
        if (c == 0) return (int)i;
    }
    dst[max - 1] = 0;
    return (int)(max - 1);
}

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
#define AT_UID     11      /* §M89 — without these four musl runs SECURE      */
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
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

/* See proc.h.  Stored on the task, consumed by the next build_initial_stack. */
void proc_set_exec_env(const char* kv) {
    struct task* t = task_current();
    if (!t) return;
    if (!kv || !kv[0]) {                     /* NULL/"" clears the whole set */
        for (int s = 0; s < TASK_EXEC_ENV_MAX; s++) t->exec_extra_env[s][0] = '\0';
        return;
    }
    for (int s = 0; s < TASK_EXEC_ENV_MAX; s++) {
        if (t->exec_extra_env[s][0]) continue;          /* occupied */
        int i = 0;
        while (kv[i] && i < (int)sizeof t->exec_extra_env[s] - 1) {
            t->exec_extra_env[s][i] = kv[i]; i++;
        }
        t->exec_extra_env[s][i] = '\0';
        return;
    }
}


/* ---------------------------------------------------------------------------
 * §M82 — the environment a program starts with.
 *
 * PATH IS A SETTING WITH TWO LAYERS, and the rule between them is decided here
 * rather than left to folklore: an account's `env.PATH` REPLACES the machine's,
 * except that a literal `$PATH` inside it expands to the machine's value — so
 * `/home/bob/bin:$PATH` PREPENDS and `/opt/x` REPLACES, and the difference is
 * visible in the value itself.  This is the shell convention, which is why it
 * was chosen over "always prepend": a user who wants the system's programs
 * GONE (a kiosk account) has a way to say so.
 * ------------------------------------------------------------------------- */
CONFIG_KEY(ck_env_path) = {
    .key = "env.PATH", .group = "System", .type = CFG_STRING, .def = "/bin",
    .help = "program search path; an account's value replaces the machine's, "
            "and $PATH inside it stands for the machine's value",
    .scope = CFG_SCOPE_USER,
};

static void env_put(char* out, int cap, int* n, const char* s) {
    while (*s && *n < cap - 1) out[(*n)++] = *s++;
    out[*n] = 0;
}

static void env_path_for_exec(char* out, int cap) {
    const char* mine = config_get("env.PATH", "/bin");
    const char* mach = config_get_machine("env.PATH", "/bin");
    int n = 0;
    out[0] = 0;
    env_put(out, cap, &n, "PATH=");
    for (const char* p = mine; *p && n < cap - 1; ) {
        if (p[0] == '$' && p[1] == 'P' && p[2] == 'A' && p[3] == 'T' && p[4] == 'H') {
            env_put(out, cap, &n, mach);
            p += 5;
        } else {
            out[n++] = *p++;
            out[n] = 0;
        }
    }
}

/* §M89 — the PATH this task sees, $PATH expanded: the ONE interpretation, used
 * by exec's environment and by the shell's program lookup alike.  The shell's
 * first version read the raw setting and did not expand `$PATH`, so for a
 * signed-in user whose PATH is "<home>/bin:$PATH" an installed `java` in /bin
 * was "unknown" — while the same lookup as the system worked. */
void proc_path_value(char* out, int cap) {
    char buf[200];
    env_path_for_exec(buf, sizeof buf);
    int i = 0;
    for (const char* p = buf + 5; *p && i < cap - 1; p++) out[i++] = *p;   /* past "PATH=" */
    out[i] = 0;
}

static void env_home_for_exec(char* out, int cap) {
    const struct cred* c = cred_current();
    const struct user_account* u = c->owner == TASK_OWNER_USER ? user_by_uid(c->uid) : NULL;
    int n = 0;
    out[0] = 0;
    env_put(out, cap, &n, "HOME=");
    env_put(out, cap, &n, u ? user_home(u) : "/");
}

/* `envc`/`envp`: the environment a caller of execve passed (Linux's
 * semantics: exactly those strings), or envc < 0 for the default one (PATH,
 * HOME, TERM, plus proc_set_exec_env's extra).
 *
 * §M89 — EVERYTHING (strings, AT_RANDOM, the pointer table, the auxv) lives on
 * this ONE page, and nothing checked that it fit: a long enough argv would have
 * written below the page, into whatever frame preceded it.  The size is now
 * computed first and an oversized request refused (0 → the caller's E2BIG).
 * An execve that dropped envp is how a JRE's launcher looped forever: it sets
 * LD_LIBRARY_PATH, re-executes itself, and the new image — never seeing the
 * variable — does it again. */
static uintptr_t build_initial_stack(pmm_phys_t frame_phys, uintptr_t stack_va,
                                     int argc, const char* const argv[],
                                     int envc, const char* const envp[],
                                     const struct loaded_prog* lp) {
    if (argc < 0) argc = 0;
    if (argc > PROC_MAX_ARGV) return 0;
    if (envc > PROC_MAX_ENV) return 0;

    /* The environment first (it decides the size). */
    char path_var[160], home_var[96];
    const char* env[PROC_MAX_ENV + TASK_EXEC_ENV_MAX];
    int nenv = 0;
    struct task* me = task_current();
    if (envc >= 0) {
        for (int i = 0; i < envc; i++) env[nenv++] = envp[i];
    } else {
        /* §M82 — PATH and HOME come from the session, not from a table: PATH
         * is the `env.PATH` setting as THIS task sees it (its account's value
         * over the machine's — config.c's layers), and HOME is its account's
         * real home. */
        env_path_for_exec(path_var, sizeof path_var);
        env_home_for_exec(home_var, sizeof home_var);
        env[nenv++] = path_var;
        env[nenv++] = home_var;
        env[nenv++] = "TERM=d-os";
        /* §M40 — plus the caller-supplied variables for this exec (see
         * proc_set_exec_env): per-launch data such as WAYLAND_SOCKET=<fd>. */
        if (me)
            for (int s = 0; s < TASK_EXEC_ENV_MAX; s++)
                if (me->exec_extra_env[s][0]) env[nenv++] = me->exec_extra_env[s];
    }
    {
        uintptr_t need = 16 + 16;                        /* AT_RANDOM + alignment */
        for (int i = 0; i < argc; i++) need += u_strlen(argv[i]) + 1;
        for (int i = 0; i < nenv; i++) need += u_strlen(env[i]) + 1;
        need += (1 + (uintptr_t)argc + 1 + (uintptr_t)nenv + 1 + 14 * 2) * sizeof(uintptr_t);
        if (need > PAGE_SIZE) {
            kprintf("exec: arguments + environment need %u bytes, the initial stack page holds %u - refused (E2BIG)\n",
                    (unsigned)need, (unsigned)PAGE_SIZE);
            return 0;
        }
    }

    /* §M86 — the stack is a USER page and may be highmem: written through a
     * kmap, released at the single return below.  Nothing in between sleeps
     * (random_bytes takes a spinlock), which the kmap contract requires. */
    uint8_t* base = (uint8_t*)kmap_frame(frame_phys);
    if (!base) return 0;
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

    /* 1a. The environment strings below the args. */
    uintptr_t env_uva[PROC_MAX_ENV + TASK_EXEC_ENV_MAX];
    for (int i = nenv - 1; i >= 0; i--) {
        uint32_t l = u_strlen(env[i]) + 1;
        koff -= l;
        for (uint32_t j = 0; j < l; j++) base[koff + j] = (uint8_t)env[i][j];
        env_uva[i] = stack_va + koff;
    }

    if (me && envc < 0)                         /* one exec only — consumed */
        for (int s = 0; s < TASK_EXEC_ENV_MAX; s++) me->exec_extra_env[s][0] = '\0';

    /* 1b. Reserve + fill the 16 AT_RANDOM bytes just below the strings and
     *     record their user VA (kept 4-byte aligned). */
    koff -= 16;
    koff &= ~(uintptr_t)0x3;
    fill_at_random(base + koff, frame_phys, stack_va);
    uintptr_t at_random_uva = stack_va + koff;

    /* 2. Lay out the pointer table below the strings, keeping the final SP
     *    16-byte aligned.  Slots: argc + argv[argc] + argv-NULL + envp[nenv] +
     *    envp-NULL + auxv{ PAGESZ, CLKTCK, RANDOM, SECURE,
     *    PHDR, PHENT, PHNUM, BASE, ENTRY, UID, EUID, GID, EGID, NULL } = 14
     *    pairs. */
    uintptr_t slot = sizeof(uintptr_t);
    uintptr_t nslots = 1 + (uintptr_t)argc + 1 + (uintptr_t)nenv + 1 + (14 * 2);
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
    /* §M89 — musl decides whether a process is "secure" (setuid-like) as
     * `(aux[0] & 0x7800) != 0x7800 || uid != euid || gid != egid ||
     * AT_SECURE`: bits 11-14 of aux[0] say which of AT_UID..AT_EGID were
     * PRESENT.  Leaving them out therefore made EVERY dynamic musl program
     * secure, so its loader ignored LD_LIBRARY_PATH and refused `$ORIGIN` —
     * which is how a JRE finds libjli.so ("Error loading shared library
     * libjli.so", with the file right there).  Real and effective ids are the
     * same here: nothing on this system is setuid.  The values are the
     * EXEC-ing task's, which is the process's own for execve; a task spawned
     * by another and given a new identity afterwards (ctr) sees its parent's
     * here and its own from getuid — equal pairs either way, which is the
     * part the loader reads. */
    {
        struct task* ct = task_current();
        int u = ct ? cred_uid(&ct->cred) : 0, g = ct ? cred_gid(&ct->cred) : 0;
        if (u < 0) u = 0;
        if (g < 0) g = 0;
        w[k++] = AT_UID;  w[k++] = (uintptr_t)u;
        w[k++] = AT_EUID; w[k++] = (uintptr_t)u;
        w[k++] = AT_GID;  w[k++] = (uintptr_t)g;
        w[k++] = AT_EGID; w[k++] = (uintptr_t)g;
    }
    w[k++] = AT_NULL;  w[k++] = 0;                     /* auxv terminator       */
    kunmap_frame(base);
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
        /* §M32 — the INTERPRETER is required to be READABLE, not executable.
         * ld.so is loaded the way a shared library is: the kernel maps it on
         * behalf of a program that has already passed the x check above.
         * Demanding x here would mean every `.so` in a closure needed one too,
         * and then the bit would mean "is a file" rather than "may be run". */
        struct file* f = vfs_open(lp->main.interp, VFS_RDONLY);
        if (!f) return ELF_ENOLOAD;                   /* interpreter missing   */
        size_t isz = f->inode ? (size_t)f->inode->size : 0;
        if (isz == 0 || isz > (16u << 20)) { vfs_close(f); return ELF_ENOLOAD; }
        uint8_t* iimg = (uint8_t*)kmalloc(isz);
        if (!iimg) { vfs_close(f); return ELF_ENOMEM; }
        ssize_t ird = vfs_read(f, iimg, isz);
        if (ird < (ssize_t)isz) { vfs_close(f); kfree(iimg); return ELF_ENOLOAD; }

        /* §M74 — with the file, so ld.so (which in musl IS libc.so) is shared
         * through the page cache by every dynamic program instead of copied
         * into each.  The file stays open until the load is done. */
        struct elf_load_info ii;
        rc = elf_load_ex_file(s, iimg, isz,
                              vmm_user_base() + PROC_INTERP_OFFSET, &ii, f);
        vfs_close(f);
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
static pmm_phys_t map_user_stack(struct vmm_space* s, uintptr_t* stack_va_out) {
    uintptr_t top_page = vmm_user_base() + PROC_STACK_TOP - PAGE_SIZE;
    pmm_phys_t top_frame = 0;
    for (uint32_t i = 0; i < PROC_STACK_PAGES; i++) {
        uintptr_t va = top_page - (uintptr_t)i * PAGE_SIZE;
        pmm_phys_t fr = pmm_alloc_frame_user();          /* §M86 — may be highmem */
        if (!fr) return 0;
        kmap_zero_frame(fr);
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
    pmm_phys_t stk = map_user_stack(s, &stack_va);
    if (!stk) { vmm_space_destroy(s); return -1; }
    uintptr_t user_sp = build_initial_stack(stk, stack_va, argc, argv, -1, NULL, &lp);
    if (!user_sp) { vmm_space_destroy(s); return -1; }

    /* Bind the space to this task so the scheduler maintains CR3/TTBR0 across
     * any preemption during the excursion, activate it, then drop to user
     * mode.  Control returns here when the program issues SYS_EXIT. */
    struct task* me = task_current();
    struct vmm_space* prev = me ? me->mm : NULL;
    if (me) task_swap_mm(me, s);     /* a fresh space carries a fresh cursor */
    vmm_space_switch(s);

    if (me) { me->exc_fault = 0; me->exc_code = 0; }
    enter_user_mode_wrap(lp.entry, user_sp);

    /* §1.1/§M71 — the excursion left ring 3 through the SYS_EXIT teleport, which
     * does NOT unwind the dispatcher.  One route now (proc.h), because the two
     * other excursion paths never got this line. */
    user_excursion_end();

    int fault = me ? me->exc_fault : 0;
    if (me) me->exc_fault = 0;
    fd_close_all();                    /* reclaim any fds the program opened */
    vmm_space_switch(prev);
    if (me) task_swap_mm(me, prev);
    vmm_space_destroy(s);
    int code = me ? me->exc_code : 0;
    if (me) me->exc_code = 0;
    return fault ? -(128 + fault) : code;
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
/* ONE page of strings for argv and envp together: they end up on one page of
 * the new stack (build_initial_stack), so a larger buffer could only defer
 * the refusal. */
#define EXECVE_ARGBUF 4096u
#define E2BIG_RC      (-7)

/* Copy a NULL-terminated user vector of strings into `buf` at *off.  Returns
 * the count, -1 for a bad pointer, E2BIG_RC when it does not fit — never a
 * silently shortened vector (§M73's argv lesson: a truncated argument list is
 * a different program). */
static int marshal_vec(char* const uvec[], const char** out, int max, char* buf, uint32_t* off) {
    if (!uvec) return 0;
    int n = 0;
    for (;; n++) {
        if (!vmm_user_access_ok((uintptr_t)&uvec[n], sizeof(char*), 0)) return -1;
        const char* s = uvec[n];
        if (!s) return n;
        if (n >= max || *off >= EXECVE_ARGBUF) return E2BIG_RC;
        int l = u_strcopy(&buf[*off], s, EXECVE_ARGBUF - *off);
        if (l < 0) return -1;
        if (*off + (uint32_t)l + 1 >= EXECVE_ARGBUF) return E2BIG_RC;   /* cut short */
        out[n] = &buf[*off];
        *off += (uint32_t)l + 1;
    }
}

int proc_execve(const char* path, char* const uargv[]) {
    return proc_execve_env(path, uargv, NULL);
}

int proc_execve_env(const char* path, char* const uargv[], char* const uenvp[]) {
    struct task* me = task_current();
    if (!me || !me->mm) return -1;           /* only a user process can exec  */

    /* 1. Marshal argv (and envp) into kernel memory while the OLD space is
     *    still active (user pointers valid). */
    const char* kargv[PROC_MAX_ARGV];
    const char* kenvp[PROC_MAX_ENV];
    char* strbuf = (char*)kmalloc(EXECVE_ARGBUF);
    if (!strbuf) return -1;
    uint32_t soff = 0;
    int argc = marshal_vec(uargv, kargv, PROC_MAX_ARGV, strbuf, &soff);
    int envc = uenvp ? marshal_vec(uenvp, kenvp, PROC_MAX_ENV, strbuf, &soff) : -1;
    if (argc < 0 || (uenvp && envc < 0)) {
        kfree(strbuf);
        return (argc == E2BIG_RC || envc == E2BIG_RC) ? E2BIG_RC : -1;
    }

    /* 2. Read the ELF file into a kernel buffer.  §1.1 — `path` is the calling
     *    program's pointer, so copy it in (validated) before the VFS sees it. */
    char kpath[256];
    if (u_strcopy(kpath, path, sizeof kpath) < 0) { kfree(strbuf); return -1; }
    struct file* f = vfs_open(kpath, VFS_RDONLY);
    if (!f) { kfree(strbuf); return -1; }
    /* §M89 — what /proc/self/exe will name (vfs_canonical joins the cwd and
     * resolves "..", within this task's root).  Computed now, COMMITTED at the
     * point of no return below, so a failed exec keeps the old name. */
    char new_exe[sizeof me->cred.exe];
    /* §M89 — the RESOLVED path: started through /bin/java, a JDK must see its
     * real home, or it looks for its libraries under /bin/../lib. */
    if (vfs_realpath(kpath, new_exe, sizeof new_exe) != 0) new_exe[0] = 0;

    /* §M32 stage 6 — THE EXECUTE BIT, AND THIS IS THE ONLY PLACE IT CAN LIVE.
     *
     * A program is opened for READING in order to be run, so an open-time
     * check has no way to distinguish "may read this file" from "may run it":
     * `vfs_open` above has already granted read, and it was right to.  The x
     * bit is a statement about EXECUTION, and the loader is the only code that
     * knows that is what is happening.
     *
     * It is also why it could not be inherited from stage 5's list of
     * enforcement points, all of which are in vfs.c. */
    if (f->inode && !vfs_permitted(f->inode, VFS_PERM_EXEC)) {
        kprintf("exec: %s: not executable\n", kpath);
        vfs_close(f);
        kfree(strbuf);
        return -1;
    }
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
    pmm_phys_t stk = map_user_stack(ns, &stack_va);
    if (!stk) { vmm_space_destroy(ns); kfree(img); kfree(strbuf); return -1; }
    uintptr_t user_sp = build_initial_stack(stk, stack_va, argc, kargv, envc, kenvp, &lp);
    if (!user_sp) { vmm_space_destroy(ns); kfree(img); kfree(strbuf); return E2BIG_RC; }

    /* 4. Commit: swap to the new space, free the old one + scratch.  execve
     *    resets signal dispositions to default (custom handlers pointed into
     *    the old image); the restorer is re-registered by the new program. */
    for (int i = 0; i < NSIG; i++) me->sig_handler[i] = SIG_DFL;
    me->sig_pending = 0;
    for (unsigned i = 0; i < sizeof me->cred.exe; i++) me->cred.exe[i] = new_exe[i];
    lnx_sig_exec(me);                         /* §M89 — handlers pointed into the old image */
    struct vmm_space* old = task_swap_mm(me, ns);   /* under the walkers' lock */
    vmm_space_switch(ns);
    /* §M74 — a pressure eviction may be inside the OLD space; it pinned this
     * task and releases in a bounded batch. */
    while (__atomic_load_n(&me->swap_busy, __ATOMIC_ACQUIRE)) task_msleep(2);
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
    int               linux_abi;   /* run under the Linux-ABI personality?     */
};

static void user_task_bootstrap(void) {
    struct user_boot* b = (struct user_boot*)task_start_arg();
    struct task* me = task_current();
    uintptr_t entry = b->entry, sp = b->user_sp;

    me->mm          = b->space;
    me->user_task   = 1;
    me->linux_abi   = b->linux_abi;   /* a musl/Linux-ABI package (e.g. NetSurf) */
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
    struct fdtable*   fdt;           /* §M89 — the shared descriptor table */
};

static void clone_bootstrap(void) {
    struct clone_boot* b = (struct clone_boot*)task_start_arg();
    struct task* me = task_current();

    me->mm          = b->space;      /* SHARED with the creator */
    me->mm_shared   = 1;
    me->user_task   = 1;
    fdtable_adopt(me, b->fdt);

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
    b->fdt         = fdtable_share(parent);
    if (!b->fdt) { kfree(b); return -1; }

    struct task* t = task_spawn_arg_held("thread", clone_bootstrap, b);
    if (!t) {
        fdtable_put(b->fdt);
        kfree(b);
        return -1;
    }
    t->mm_shared = 1;               /* set early too (before it may run) */
    task_set_reap_owned(t, 1);      /* the creator joins it with waitpid() */
    task_release(t);                /* §4.96: built completely, now it may run */
    return t->pid;
}

/* Full form: run an ELF as an independent, preemptible user task with an
 * argv and an optional Linux-ABI personality.  This is the vehicle a GUI
 * "package" (e.g. NetSurf) launches through — NOT the synchronous excursion
 * (proc_exec_*), which nests the ring-3 run inside the caller's kernel task
 * and is reserved for the self-tests.  Running as a real user_task is what
 * makes a faulting/wedged package terminate cleanly (isr_handler kills the
 * *task*, the reaper frees its space) and force-killable (M46), instead of
 * taking down the shell/desktop task the excursion would have run on. */
int proc_spawn_argv_under(const char* name, const void* image, size_t len,
                          int argc, const char* const argv[], int linux_abi,
                          int ppid) {
    struct vmm_space* s = vmm_space_create();
    if (!s) return -1;

    struct loaded_prog lp;
    int rc = load_program(s, image, len, &lp);
    if (rc != ELF_OK) { vmm_space_destroy(s); return rc; }

    uintptr_t stack_va;
    pmm_phys_t stk = map_user_stack(s, &stack_va);
    if (!stk) { vmm_space_destroy(s); return -1; }

    struct user_boot* b = (struct user_boot*)kmalloc(sizeof *b);
    if (!b) { vmm_space_destroy(s); return -1; }
    b->space     = s;
    b->entry     = lp.entry;
    b->user_sp   = build_initial_stack(stk, stack_va, argc, argv, -1, NULL, &lp);
    if (!b->user_sp) { kfree(b); vmm_space_destroy(s); return -1; }
    b->linux_abi = linux_abi;

    /* ppid >= 0 parents the package explicitly (a GUI launcher passes the
     * desktop pid so the browser groups under the GUI session, not init);
     * ppid < 0 falls back to the caller.
     *
     * §M89 — the program writes to, and reads from, the TERMINAL it was
     * started from: the caller's console binding goes to the child AT
     * CREATION (the task may run on another core before this returns).  It
     * used to be NULL, so a program started from a GUI terminal — `java`,
     * `ctr run` — printed to the machine's suppressed console and read no
     * keys: reported from use as "java's output is not the shell it was
     * started from".  Closing the terminal kills its subtree, so the binding
     * cannot outlive the window. */
    struct task* me = task_current();
    struct task* t = task_spawn_arg_console(name, user_task_bootstrap, b, ppid,
                                            me ? me->out_console : NULL);
    if (!t) { kfree(b); vmm_space_destroy(s); return -1; }
    return t->pid;
}

int proc_spawn_argv(const char* name, const void* image, size_t len,
                    int argc, const char* const argv[], int linux_abi) {
    return proc_spawn_argv_under(name, image, len, argc, argv, linux_abi, -1);
}

int proc_spawn(const char* name, const void* image, size_t len) {
    return proc_spawn_argv_under(name, image, len, 0, NULL, 0, -1);
}

/* §M71 — see proc.h for why this exists and what it costs when it is missed. */
void user_excursion_end(void) {
    struct task* me = task_current();
    if (me) me->in_user_syscall = 0;
    /* §M89 — the program's signal state belonged to the PROGRAM, and its
     * handlers point into an image that is gone: the hosting task must not
     * carry them (or a mask the program set) into whatever it runs next. */
    if (me) {
        lnx_sig_free(me);
        me->sig_blocked = 0;
        me->sig_pending = 0;
        for (int i = 0; i < NSIG; i++) me->sig_handler[i] = SIG_DFL;
    }
}

/* ---------------------------------------------------------------------------
 * #11 (2026-09-26) — THE EXCURSION'S STATE LIVES ON THE TASK.
 *
 * A synchronous excursion (proc_exec_elf, the self-tests) drops to ring 3 from
 * the middle of a kernel call and comes back at SYS_EXIT.  Two things have to
 * survive the trip, and both used to be shared:
 *   - WHERE TO COME BACK TO was two globals (saved_esp/eip, x86 and ARM alike),
 *     so two excursions at once overwrote each other's — whichever exited
 *     second resumed on the other's stack;
 *   - WHERE RING 3 TRAPS TO was, on x86, a per-CPU fixed stack (tss.c).  An
 *     excursion that BLOCKS in a syscall (forktest's waitpid) left its frames
 *     there, and the next excursion trapping on that CPU overwrote them; a
 *     migration made it another CPU's stack outright.  The `!! KSTACK` check
 *     named it: "task 'serial-cmd' is not on its own kernel stack".
 * Now the resume point is the task's `exc_resume`, and traps land on the
 * task's OWN kernel stack, 512 bytes below the frame that started the trip
 * (the arch pushes its saved registers just below the caller, and that must
 * stay intact for the return).  The scheduler re-installs `exc_kstack` on
 * every switch-in, so a migrated or preempted excursion keeps it.  aarch64
 * already trapped onto the task's own stack (SP_EL1 at eret); only the
 * resume point was shared there. */
void enter_user_mode_wrap(uintptr_t user_ip, uintptr_t user_sp) {
    struct task* me = task_current();
    if (!me) return;
    uintptr_t prev_k = me->exc_kstack;
    me->exc_kstack = ((uintptr_t)__builtin_frame_address(0) - 512u) & ~(uintptr_t)15;
    hal_set_kernel_stack(me->exc_kstack);
    arch_enter_user_wrap(user_ip, user_sp, me->exc_resume);
    me->exc_kstack = prev_k;
    hal_set_kernel_stack(prev_k);        /* 0 = the arch default */
}

void user_excursion_teleport(void) {
    struct task* me = task_current();
    /* The resume point is ordinary kernel code on an ordinary task, which
     * runs with interrupts ON.  The trap that brings us here may have masked
     * them (an x86 interrupt gate, an aarch64 exception entry), and the
     * resume path restores registers but not the flags — so a task could come
     * back from an excursion and carry on with the timer masked. */
    hal_intr_enable();
    hal_syscall_exit_to_kernel(me->exc_resume[0], me->exc_resume[1]);
    for (;;) { }                         /* not reached */
}

void user_excursion_exit(int code) {
    struct task* me = task_current();
    if (me) me->exc_code = code & 0xFF;          /* the 8 bits a status carries */
    user_excursion_teleport();
}

void user_excursion_fault(int sig) {
    struct task* me = task_current();
    if (!me || me->user_task || !me->exc_kstack) return;   /* a real process */
    me->exc_fault = sig ? sig : 11;
    user_excursion_teleport();
}
