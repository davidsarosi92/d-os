/* =============================================================================
 * vma.c — reservations, demand-zero pages, true PROT_NONE, lazy file mappings
 * (§M89 rung 1).  The contract and the reasons are in vma.h.
 *
 * THE LIST.  A sorted singly linked list of non-overlapping ranges per address
 * space.  A tree would be the textbook answer; a list is the honest one at the
 * sizes this system sees (a JVM makes a few hundred mappings), and adjacent
 * anonymous ranges with the same protection are MERGED so a program that
 * commits a reservation page by page does not grow it without bound.
 *
 * EVERY OPERATION IS "SPLIT, THEN WORK ON WHOLE RANGES".  mprotect and munmap
 * first make sure no vma straddles either end of the target (split_at), after
 * which each affected vma is either entirely inside or entirely outside — the
 * three-way overlap arithmetic is written once, in split_at, instead of in
 * every caller.
 *
 * LOCKING.  One kmutex per set.  A demand fault may read a file page from disk
 * (pcache_map_page), so a spinlock is impossible; and it is held across the
 * probe-then-map in vma_fault, which is what stops two threads of one process
 * faulting on the same page from mapping it twice (the second mapping would
 * silently discard what the first thread wrote).
 * ============================================================================= */

#include "vma.h"
#include "vmm.h"
#include "vmm_flags.h"
#include "pmm.h"
#include "kmap.h"
#include "kmalloc.h"
#include "kmutex.h"
#include "lock.h"
#include "task.h"
#include "percpu.h"
#include "fd.h"
#include "vfs.h"
#include "pcache.h"
#include "printf.h"
#include "shellcmd.h"
#include <stdint.h>
#include <stddef.h>

#define PG             4096u
#define MMAP_BASE_OFF  0x08000000u     /* above image / interpreter / stack (proc.c) */
#define EAGER_MAX      (256u << 20)    /* an EAGER mapping allocates up front: bound it */

#define E_NOMEM  12
#define E_INVAL  22
#define E_EXIST  17
#define E_BADF    9
#define E_ACCES  13
#define E_NODEV  19

enum { VMA_ANON = 1, VMA_FILE = 2, VMA_EAGER = 3 };

struct vma {
    uintptr_t     start, end;     /* [start, end), page aligned                     */
    uint32_t      vf;             /* VMM_USER|WRITABLE|EXEC; 0 = PROT_NONE          */
    uint8_t       kind;
    struct ofile* file;           /* VMA_FILE: held reference (keeps the file open) */
    uint64_t      off;            /* VMA_FILE: file offset of `start`               */
    struct vma*   next;
};

struct vma_set {
    struct kmutex lock;
    struct vma*   head;
    uint32_t      faults;         /* demand pages provided (diagnostics)             */
};

static spinlock_t g_create = SPINLOCK_INIT;

static uint32_t prot_to_vf(int prot) {
    if (!(prot & (VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC))) return 0;   /* PROT_NONE */
    uint32_t vf = VMM_USER;
    if (prot & VMA_PROT_WRITE) vf |= VMM_WRITABLE;
    if (prot & VMA_PROT_EXEC)  vf |= VMM_EXEC;
    return vf;
}

/* The set of `mm`, created on first use.  Creation races between two threads
 * of one process calling mmap for the first time, hence the spinlock around
 * the publish (the allocation happens outside it). */
static struct vma_set* set_of(struct vmm_space* mm, int create) {
    if (!mm) return NULL;
    struct vma_set* s = (struct vma_set*)vmm_space_vma(mm);
    if (s || !create) return s;
    struct vma_set* n = (struct vma_set*)kcalloc(1, sizeof *n);
    if (!n) return NULL;
    kmutex_init(&n->lock, "vma");
    uint32_t fl = spin_lock_irqsave(&g_create);
    s = (struct vma_set*)vmm_space_vma(mm);
    if (!s) { vmm_space_set_vma(mm, n); s = n; n = NULL; }
    spin_unlock_irqrestore(&g_create, fl);
    if (n) kfree(n);
    return s;
}

static struct vma* vma_new(uintptr_t a, uintptr_t b, uint32_t vf, int kind,
                           struct ofile* f, uint64_t off) {
    struct vma* v = (struct vma*)kcalloc(1, sizeof *v);
    if (!v) return NULL;
    v->start = a; v->end = b; v->vf = vf; v->kind = (uint8_t)kind;
    v->file = f ? ofile_ref(f) : NULL;
    v->off = off;
    return v;
}

static void vma_free(struct vma* v) {
    if (v->file) ofile_unref(v->file);
    kfree(v);
}

static struct vma* find(struct vma_set* s, uintptr_t va) {
    for (struct vma* v = s->head; v && v->start <= va; v = v->next)
        if (va < v->end) return v;
    return NULL;
}

static int overlaps(struct vma_set* s, uintptr_t a, uintptr_t b) {
    for (struct vma* v = s->head; v && v->start < b; v = v->next)
        if (v->end > a) return 1;
    return 0;
}

static void insert(struct vma_set* s, struct vma* n) {
    struct vma** pp = &s->head;
    while (*pp && (*pp)->start < n->start) pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;
}

/* Make sure no vma straddles `at`.  Returns -1 only when the split needs
 * memory and there is none — the caller then refuses the whole operation, so
 * a half-applied mprotect is impossible. */
static int split_at(struct vma_set* s, uintptr_t at) {
    struct vma* v = find(s, at);
    if (!v || v->start == at) return 0;
    struct vma* r = vma_new(at, v->end, v->vf, v->kind, v->file,
                            v->off + (uint64_t)(at - v->start));
    if (!r) return -1;
    v->end = at;
    r->next = v->next;
    v->next = r;
    return 0;
}

/* Join neighbours that are indistinguishable: two anonymous ranges with the
 * same protection.  File ranges are left alone (their offsets would have to
 * line up, and they are few). */
static void merge(struct vma_set* s) {
    struct vma* v = s->head;
    while (v && v->next) {
        struct vma* n = v->next;
        if (v->kind == VMA_ANON && n->kind == VMA_ANON && v->end == n->start && v->vf == n->vf) {
            v->end = n->end;
            v->next = n->next;
            vma_free(n);
        } else v = n;
    }
}

/* Remove every vma inside [a, b) (after the caller split at both ends). */
static void remove_range(struct vma_set* s, uintptr_t a, uintptr_t b) {
    struct vma** pp = &s->head;
    while (*pp) {
        struct vma* v = *pp;
        if (v->start >= a && v->end <= b) { *pp = v->next; vma_free(v); }
        else pp = &v->next;
    }
}

/* A PROT_NONE page is present but kernel-only, and vmm_space_unmap (rightly)
 * refuses to remove an entry that does not look like a user page — so it is
 * given back its user bit first.  Safe because every caller has already
 * refused ranges the kernel's own tables cover. */
static void unmap_pages(struct vmm_space* mm, uintptr_t a, uintptr_t b) {
    for (uintptr_t va = a; va < b; va += PG) {
        int p = vmm_space_probe(mm, va);
        if (p == 1) vmm_space_protect(mm, va, VMM_USER);
        if (p) vmm_space_unmap(mm, va);
    }
}

/* The allocation window: above the image, the interpreter and the stack, and
 * below what this arch's user page tables can express. */
static void window(uintptr_t* lo, uintptr_t* hi) {
    *lo = vmm_user_base() + MMAP_BASE_OFF;
    *hi = vmm_user_limit();
}

/* Is [a, a+len) available: inside the window, clear of every vma, and clear
 * of every mapping the vma list does not know about (vmm_space_range_free)? */
static int usable(struct vma_set* s, struct vmm_space* mm, uintptr_t a, size_t len) {
    uintptr_t lo, hi;
    window(&lo, &hi);
    if (a < lo || a + len < a || a + len > hi) return 0;
    if (overlaps(s, a, a + len)) return 0;
    return vmm_space_range_state(mm, a, a + len) == 0;
}

/* First fit, low to high.  Low addresses first on purpose: a JVM wants its
 * heap below 32 GiB (compressed pointers) and asks for it by hint, and a
 * search that started at the top would make every hint miss. */
static uintptr_t find_free(struct vma_set* s, struct vmm_space* mm, size_t len, uintptr_t hint) {
    if (hint && !(hint & (PG - 1)) && usable(s, mm, hint, len)) return hint;
    uintptr_t lo, hi;
    window(&lo, &hi);
    uintptr_t cand = lo;
    const uintptr_t STEP = 2u << 20;    /* past an unknown mapping, move a table's worth */
    while (cand + len > cand && cand + len <= hi) {
        struct vma* blocker = NULL;
        for (struct vma* v = s->head; v && v->start < cand + len; v = v->next)
            if (v->end > cand) { blocker = v; break; }
        if (blocker) { cand = blocker->end; continue; }
        if (vmm_space_range_state(mm, cand, cand + len) == 0) return cand;
        cand = (cand + STEP) & ~(STEP - 1);
    }
    return 0;
}

/* ---- eager backings (what mmap did before §M89, kept for the cases that
 *      genuinely need frames up front) ---------------------------------------- */

static int map_shm(struct vmm_space* mm, uintptr_t va, size_t n, struct shm* sh,
                   uint64_t off, uint32_t vf) {
    size_t first = (size_t)(off / PG);
    for (size_t i = 0; i < n; i++) {
        size_t idx = first + i;
        if (idx >= (size_t)sh->nframes) break;          /* short object: stop here */
        /* VMM_SHARED: the shm object owns these frames — teardown must not
         * free them.  A memfd is shared by definition, so it stays writable. */
        if (vmm_space_map(mm, va + i * PG, sh->frames[idx],
                          (vf ? vf : VMM_USER) | VMM_WRITABLE | VMM_SHARED) != 0)
            return -1;
    }
    return 0;
}

/* A file range copied up front: MAP_SHARED (not coherent with the file — the
 * same limit as before §M89, now stated), or the page cache switched off. */
static int map_file_copy(struct vmm_space* mm, uintptr_t va, size_t n, struct file* file,
                         uint64_t off, uint32_t vf) {
    uint8_t* bounce = (uint8_t*)kmalloc(PG);
    if (!bounce) return -1;
    for (size_t i = 0; i < n; i++) {
        /* §M86 — a USER page may be highmem.  Read into a kernel bounce page
         * first: vfs_read can sleep, and a kmap must not be held across a
         * sleep (kmap.h). */
        pmm_phys_t fr = pmm_alloc_frame_user();
        if (!fr) { kfree(bounce); return -1; }
        for (unsigned b = 0; b < PG; b++) bounce[b] = 0;
        uint64_t save = file->pos;                       /* musl owns the cursor */
        file->pos = off + (uint64_t)i * PG;
        vfs_read(file, bounce, PG);                      /* short tail stays zero */
        file->pos = save;
        uint8_t* p = (uint8_t*)kmap_frame(fr);
        if (p) { for (unsigned b = 0; b < PG; b++) p[b] = bounce[b]; kunmap_frame(p); }
        if (vmm_space_map(mm, va + i * PG, fr, vf ? vf : VMM_USER) != 0) {
            pmm_free_frame(fr);
            kfree(bounce);
            return -1;
        }
        if (!vf) vmm_space_protect(mm, va + i * PG, 0);  /* PROT_NONE: kernel-only */
    }
    kfree(bounce);
    return 0;
}

/* ---- the syscalls ---------------------------------------------------------- */

long vma_mmap(uintptr_t addr, size_t len, int prot, int flags, int fd, uint64_t off) {
    struct task* t = task_current();
    if (!t || !t->mm) return -E_NOMEM;
    if (len == 0) return -E_INVAL;
    size_t size = (len + PG - 1) & ~(size_t)(PG - 1);
    if (size < len) return -E_NOMEM;
    size_t n = size / PG;
    uint32_t vf = prot_to_vf(prot);
    int fixed = (flags & (VMA_MAP_FIXED | VMA_MAP_FIXED_NOREPLACE)) != 0;
    if (fixed && (addr & (PG - 1))) return -E_INVAL;

    /* What backs the range. */
    int kind = VMA_ANON;
    struct ofile* o = NULL;
    if (!(flags & VMA_MAP_ANONYMOUS) && fd >= 0) {
        o = fd_lookup(fd);
        if (!o) return -E_BADF;
        if (off & (PG - 1)) return -E_INVAL;
        if (o->kind == FD_SHM && o->shm) kind = VMA_EAGER;
        else if (o->kind == FD_VFS && o->file) {
            /* §M74 rung 2's rule, now LAZY: a private file mapping shares the
             * page cache's frames copy-on-write, filled on first touch.  A
             * 101 MB file mapped whole costs nothing until it is read. */
            /* §M90 — a READ-ONLY shared mapping is served from the page cache
             * too: since writes update the cached pages in place
             * (pcache_update) it sees the file as it changes, which is the
             * whole of MAP_SHARED for a reader — and the shape bbolt uses
             * (mmap PROT_READ + pwrite; the eager copy below handed it stale
             * pages and containerd's metadata "page 2 already freed").  A
             * WRITABLE shared mapping stays an eager copy: stores through it
             * would need writing back to the file, which nothing does yet —
             * not coherent, as before §M89, and said so at map_file_copy. */
            int ro_shared = (flags & VMA_MAP_SHARED) && !(vf & VMM_WRITABLE);
            kind = ((!(flags & VMA_MAP_SHARED) || ro_shared) && pcache_enabled()) ? VMA_FILE : VMA_EAGER;
        } else return -E_NODEV;
    }
    if (kind == VMA_EAGER && size > EAGER_MAX) return -E_NOMEM;

    struct vmm_space* mm = t->mm;
    struct vma_set* s = set_of(mm, 1);
    if (!s) return -E_NOMEM;
    kmutex_lock(&s->lock);

    uintptr_t va;
    if (fixed) {
        va = addr;
        uintptr_t lo, hi;
        window(&lo, &hi);
        /* MAP_FIXED may land anywhere in the user range the program owns —
         * including below the mmap window (ld.so maps a library's segments
         * over its own reservation), so the bound is the user base, not lo. */
        int st = (va < vmm_user_min() || va + size < va || va + size > hi)
                 ? VMA_RS_KERNEL : vmm_space_range_state(mm, va, va + size);
        if (st & VMA_RS_KERNEL) {    /* the kernel's tables are never the program's */
            kmutex_unlock(&s->lock);
            return -E_INVAL;
        }
        if (flags & VMA_MAP_FIXED_NOREPLACE) {
            if (overlaps(s, va, va + size) || st) {
                kmutex_unlock(&s->lock);
                return -E_EXIST;
            }
        } else {
            /* Replace whatever was there: vmas first (split so only the
             * target goes), then the pages. */
            if (split_at(s, va) || split_at(s, va + size)) {
                kmutex_unlock(&s->lock);
                return -E_NOMEM;
            }
            remove_range(s, va, va + size);
            unmap_pages(mm, va, va + size);
        }
    } else {
        va = find_free(s, mm, size, addr & ~(uintptr_t)(PG - 1));
        if (!va) { kmutex_unlock(&s->lock); return -E_NOMEM; }
    }

    struct vma* v = vma_new(va, va + size, vf, kind, kind == VMA_FILE ? o : NULL, off);
    if (!v) { kmutex_unlock(&s->lock); return -E_NOMEM; }
    insert(s, v);

    int rc = 0;
    if (kind == VMA_EAGER)
        rc = (o->kind == FD_SHM) ? map_shm(mm, va, n, o->shm, off, vf)
                                 : map_file_copy(mm, va, n, o->file, off, vf);
    if (rc != 0) {
        remove_range(s, va, va + size);
        unmap_pages(mm, va, va + size);
        kmutex_unlock(&s->lock);
        return -E_NOMEM;
    }
    merge(s);
    kmutex_unlock(&s->lock);
    return (long)va;
}

long vma_munmap(uintptr_t addr, size_t len) {
    struct task* t = task_current();
    if (!t || !t->mm) return -E_INVAL;
    if ((addr & (PG - 1)) || len == 0) return -E_INVAL;
    uintptr_t end = addr + ((len + PG - 1) & ~(uintptr_t)(PG - 1));
    if (end < addr || addr < vmm_user_min() || end > vmm_user_limit()) return -E_INVAL;
    if (vmm_space_range_state(t->mm, addr, end) & VMA_RS_KERNEL) return -E_INVAL;
    struct vma_set* s = set_of(t->mm, 0);
    if (s) {
        kmutex_lock(&s->lock);
        if (split_at(s, addr) || split_at(s, end)) { kmutex_unlock(&s->lock); return -E_NOMEM; }
        remove_range(s, addr, end);
        unmap_pages(t->mm, addr, end);
        kmutex_unlock(&s->lock);
    } else {
        unmap_pages(t->mm, addr, end);
    }
    return 0;
}

long vma_mprotect(uintptr_t addr, size_t len, int prot) {
    struct task* t = task_current();
    if (!t || !t->mm) return -E_INVAL;
    if (addr & (PG - 1)) return -E_INVAL;
    uintptr_t end = (addr + len + PG - 1) & ~(uintptr_t)(PG - 1);
    if (end < addr || addr < vmm_user_min() || end > vmm_user_limit()) return -E_INVAL;
    if (vmm_space_range_state(t->mm, addr, end) & VMA_RS_KERNEL) return -E_INVAL;
    uint32_t vf = prot_to_vf(prot);
    struct vma_set* s = set_of(t->mm, 0);
    if (s) {
        kmutex_lock(&s->lock);
        if (split_at(s, addr) || split_at(s, end)) { kmutex_unlock(&s->lock); return -E_NOMEM; }
        for (struct vma* v = s->head; v && v->start < end; v = v->next)
            if (v->start >= addr && v->end <= end) v->vf = vf;
        merge(s);
    }
    /* The pages already there — in a vma or not (the ELF image, RELRO) —
     * take the new protection now; PROT_NONE makes them kernel-only, which
     * is what makes a guard page FAULT. */
    for (uintptr_t va = addr; va < end; va += PG)
        vmm_space_protect(t->mm, va, vf);
    if (s) kmutex_unlock(&s->lock);
    return 0;
}

long vma_madvise_dontneed(uintptr_t addr, size_t len) {
    struct task* t = task_current();
    if (!t || !t->mm || (addr & (PG - 1))) return -E_INVAL;
    uintptr_t end = addr + ((len + PG - 1) & ~(uintptr_t)(PG - 1));
    if (end < addr) return -E_INVAL;
    struct vma_set* s = set_of(t->mm, 0);
    if (!s) return 0;
    kmutex_lock(&s->lock);
    for (struct vma* v = s->head; v && v->start < end; v = v->next) {
        if (v->end <= addr || v->kind == VMA_EAGER) continue;   /* eager: its owner's pages */
        uintptr_t a = v->start > addr ? v->start : addr;
        uintptr_t b = v->end < end ? v->end : end;
        unmap_pages(t->mm, a, b);
    }
    kmutex_unlock(&s->lock);
    return 0;
}

long vma_mincore(uintptr_t addr, size_t len, uint8_t* vec) {
    struct task* t = task_current();
    if (!t || !t->mm || (addr & (PG - 1))) return -E_INVAL;
    size_t pages = (len + PG - 1) / PG;
    struct vma_set* s = set_of(t->mm, 0);
    if (s) kmutex_lock(&s->lock);
    long rc = 0;
    for (size_t i = 0; i < pages; i++) {
        uintptr_t va = addr + i * PG;
        int p = vmm_space_probe(t->mm, va);
        if (p == 0 && !(s && find(s, va))) { rc = -E_NOMEM; break; }
        vec[i] = (p == 1) ? 1 : 0;
    }
    if (s) kmutex_unlock(&s->lock);
    return rc;
}

uintptr_t vma_reserve_eager(struct vmm_space* mm, size_t npages, int prot) {
    struct vma_set* s = set_of(mm, 1);
    if (!s || npages == 0) return 0;
    kmutex_lock(&s->lock);
    uintptr_t va = find_free(s, mm, npages * PG, 0);
    if (va) {
        struct vma* v = vma_new(va, va + npages * PG, prot_to_vf(prot), VMA_EAGER, NULL, 0);
        if (v) insert(s, v); else va = 0;
    }
    kmutex_unlock(&s->lock);
    return va;
}

/* ---- faults ---------------------------------------------------------------- */

static int fault_locked(struct vma_set* s, struct vmm_space* mm, uintptr_t page, int is_write) {
    struct vma* v = find(s, page);
    if (!v || v->kind == VMA_EAGER || !(v->vf & VMM_USER)) return 0;
    if (is_write && !(v->vf & VMM_WRITABLE)) return 0;

    int p = vmm_space_probe(mm, page);
    if (p == 1) return 1;            /* another thread provided it meanwhile: retry */
    if (p != 0) return 0;            /* an evicted page is swap's, not ours          */

    pmm_phys_t fr;
    if (v->kind == VMA_ANON) {
        fr = pmm_alloc_frame_user();
        if (!fr) { kprintf("vma: out of memory for a demand-zero page at %p\n", (void*)page); return 0; }
        kmap_zero_frame(fr);
        if (vmm_space_map(mm, page, fr, v->vf) != 0) { pmm_free_frame(fr); return 0; }
    } else {                         /* VMA_FILE: share the cached page, copy on write */
        uint64_t idx = (v->off + (uint64_t)(page - v->start)) / PG;
        if (pcache_map_page(v->file->file, idx, &fr) != 0) return 0;
        if (vmm_space_map(mm, page, fr, (v->vf & ~(uint32_t)VMM_WRITABLE) | VMM_COW) != 0) {
            if (vmm_frame_unshare(fr)) pmm_free_frame(fr);
            return 0;
        }
        /* A write now retries into the COW fault, which copies first. */
    }
    s->faults++;
    return 1;
}

int vma_fault(uintptr_t va, int is_write, int can_sleep) {
    struct task* t = task_current();
    if (!t || !t->mm || !can_sleep) return 0;
    struct vma_set* s = set_of(t->mm, 0);
    if (!s) return 0;
    kmutex_lock(&s->lock);
    int r = fault_locked(s, t->mm, va & ~(uintptr_t)(PG - 1), is_write);
    kmutex_unlock(&s->lock);
    return r;
}

int vma_prefault(uintptr_t va, uintptr_t len, int is_write) {
    struct task* t = task_current();
    if (!t || !t->mm || len == 0) return 0;
    if (this_cpu()->preempt_count != 0) return 0;      /* may not sleep here */
    uintptr_t a = va & ~(uintptr_t)(PG - 1);
    uintptr_t b = va + len;
    if (b < va) return 0;
    /* §M90 — a page that is PRESENT but copy-on-write (every page of a
     * process that has forked) is read-only until somebody writes it.  A
     * program's own store resolves that through the fault; a KERNEL write
     * checked with vmm_user_access_ok(…, write) — a signal frame, a syscall's
     * result — was refused instead, and the loop below skipped the page as
     * "already there".  dockerd forks containerd, so from then on its next
     * SIGURG found its signal stack "unusable" and the daemon was killed.
     * Resolve the COW here, exactly as the write fault would; a page that is
     * not COW is left alone (vmm_cow_fault answers 0). */
    if (is_write)
        for (uintptr_t p = a; p < b; p += PG)
            if (vmm_space_probe(t->mm, p) == 1) vmm_cow_fault(p);
    struct vma_set* s = set_of(t->mm, 0);
    if (!s) return 1;                    /* the caller re-checks the walk */
    kmutex_lock(&s->lock);
    int ok = 1;
    for (uintptr_t p = a; p < b; p += PG) {
        if (vmm_space_probe(t->mm, p) == 1) continue;
        if (!fault_locked(s, t->mm, p, is_write)) { ok = 0; break; }
    }
    kmutex_unlock(&s->lock);
    return ok;
}

/* ---- lifetime -------------------------------------------------------------- */

void vma_clone(struct vmm_space* parent, struct vmm_space* child) {
    struct vma_set* ps = set_of(parent, 0);
    if (!ps || !child) return;
    struct vma_set* cs = set_of(child, 1);
    if (!cs) return;
    kmutex_lock(&ps->lock);
    struct vma** tail = &cs->head;
    for (struct vma* v = ps->head; v; v = v->next) {
        struct vma* c = vma_new(v->start, v->end, v->vf, v->kind, v->file, v->off);
        if (!c) break;               /* the child then lacks a reservation, not memory */
        *tail = c;
        tail = &c->next;
    }
    kmutex_unlock(&ps->lock);
}

void vma_destroy(struct vmm_space* mm) {
    struct vma_set* s = set_of(mm, 0);
    if (!s) return;
    vmm_space_set_vma(mm, NULL);
    struct vma* v = s->head;
    while (v) { struct vma* n = v->next; vma_free(v); v = n; }
    kfree(s);
}

/* ---- diagnostics ------------------------------------------------------------ */

/* §M90 — what covers `va` in the current task, and what its page-table entry
 * says: for a refusal report ("no usable stack") to name the cause — no
 * reservation, a PROT_NONE one, an eager one, or a page that is simply absent
 * where it should not be.  Takes the set's mutex: task context only. */
void vma_explain(uintptr_t va) {
    struct task* t = task_current();
    if (!t || !t->mm) { kprintf("vma:   %p: no address space\n", (void*)va); return; }
    int probe = vmm_space_probe(t->mm, va & ~(uintptr_t)(PG - 1));
    struct vma_set* s = set_of(t->mm, 0);
    if (!s) { kprintf("vma:   %p: no reservation set (pte probe %d)\n", (void*)va, probe); return; }
    kmutex_lock(&s->lock);
    struct vma* v = find(s, va);
    if (!v) kprintf("vma:   %p: NO reservation covers it (pte probe %d)\n", (void*)va, probe);
    else kprintf("vma:   %p: in %p-%p %c%c %s (pte probe %d, preempt %d)\n", (void*)va,
                 (void*)v->start, (void*)v->end,
                 (v->vf & VMM_USER) ? 'r' : '-', (v->vf & VMM_WRITABLE) ? 'w' : '-',
                 v->kind == VMA_ANON ? "anon" : v->kind == VMA_FILE ? "file" : "eager",
                 probe, (int)this_cpu()->preempt_count);
    kmutex_unlock(&s->lock);
}

void vma_dump(struct vmm_space* mm) {
    struct vma_set* s = set_of(mm, 0);
    if (!s) { kprintf("  (no reservations)\n"); return; }
    kmutex_lock(&s->lock);
    int n = 0;
    uint64_t total = 0;
    for (struct vma* v = s->head; v; v = v->next, n++) {
        const char* k = v->kind == VMA_ANON ? "anon" : v->kind == VMA_FILE ? "file" : "eager";
        kprintf("  %p-%p %c%c%c %s %u KiB\n", (void*)v->start, (void*)v->end,
                (v->vf & VMM_USER) ? 'r' : '-', (v->vf & VMM_WRITABLE) ? 'w' : '-',
                (v->vf & VMM_EXEC) ? 'x' : '-', k, (unsigned)((v->end - v->start) >> 10));
        total += v->end - v->start;
    }
    kprintf("  %d ranges, %u MiB reserved, %u demand pages provided\n",
            n, (unsigned)(total >> 20), s->faults);
    kmutex_unlock(&s->lock);
}

/* `maps <pid>` — the reservations of a process.  The same question
 * /proc/<pid>/maps answers for a program; this is the kernel's view. */
static void cmd_maps(const char* args) {
    int pid = 0;
    while (*args == ' ') args++;
    while (*args >= '0' && *args <= '9') pid = pid * 10 + (*args++ - '0');
    struct task* t = pid ? task_find(pid) : NULL;
    if (!t || !t->mm) { kprintf("usage: maps <pid of a user process>\n"); return; }
    vma_dump(t->mm);
}
SHELL_CMD(maps) = { "maps", "<pid>", "a process's address-space reservations (§M89)",
                    SHELL_G_DEV, cmd_maps, SHELL_P_ANY };
