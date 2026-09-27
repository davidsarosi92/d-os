/* =============================================================================
 * swap.c — evicting a PAUSED program's memory, and bringing it back
 * (§M72 stage 3, 2026-09-27).
 *
 * WHAT THIS IS, NAMED PLAINLY: swap, aimed by a person at one process instead
 * of by page pressure at the whole machine.  It is the easier half of a swapper
 * in the one way that usually makes them hard: the process is STOPPED at a
 * ring-3 safe point (task.c), so its address space is quiescent for the whole
 * operation — nothing it owns changes under the writer.  §M74's demand pager
 * is the other half; this file is written as the mechanism both will use.
 *
 * THE CONTRACT (PLAN §M72): the program stops and stays RESUMABLE; freeing its
 * memory is an optimisation of that contract, never a condition of it.  So
 * every failure here DEGRADES to "paused, kept its memory", and says why:
 * no writable volume, the store is full, a write failed, nothing was evictable.
 *
 * WHAT IS NEVER EVICTED — each for its own reason:
 *   - VMM_SHARED (memfd/shm): another process is using the frame right now;
 *   - VMM_COW: the frame may still be mapped by the other side of a fork;
 *   - a frame the allocator does not manage (a device mapping — not memory);
 *   - a frame a device can DMA into, or inside an MMIO window (drvrt's grant
 *     table is the record): hardware that has never heard of eviction would
 *     keep writing into whatever the allocator hands out next;
 *   - the kernel stack: it is not in the user region the walk covers at all —
 *     it holds the frame the program resumes ONTO.
 *
 * RESUME IS EAGER.  Every evicted page is read back BEFORE the program may run
 * again (task_cont -> swap_restore_task), so the invariant "no runnable task
 * has an evicted page table entry" holds, and AUDIT(swap-runnable) checks it.
 * Bringing pages back ON FAULT is §M74's, and is what that milestone changes.
 * If the pages cannot all come back (memory is short — the reserve applies to
 * them like any other user page), the program STAYS PAUSED and the caller is
 * told; a resume that returned the wrong bytes, or half of them, would look
 * exactly like a resume that worked until the program used them.
 *
 * THE I/O RUNS ON A KWORKER, i.e. as SYSTEM.  The store holds other programs'
 * memory, so it is 0600 and root's; an ordinary account pausing its own program
 * must not therefore be unable to write it — nor able to read it.  The caller
 * waits for the work item.
 *
 * NOT THE MEMORY RESERVE.  §M74's rule, restated because it is where it would
 * be broken: the reserve is resident memory and is never measured against or
 * redeemed through this store.  Evicting is what the reserve buys time FOR.
 * ============================================================================= */

#include "swap.h"
#include "vmm.h"
#include "pmm.h"
#include "kmap.h"
#include "task.h"
#include "vfs.h"
#include "config.h"
#include "settings.h"
#include "workqueue.h"
#include "kmalloc.h"
#include "printf.h"
#include "klog.h"
#include "audit.h"
#include "drvrt.h"
#include "lock.h"
#include "kmutex.h"
#include "timer.h"
#include "pcache.h"
#include "hal_api.h"
#include <stdint.h>
#include <stddef.h>

#define SWAP_SLOTS      65536u                 /* 256 MiB of 4 KiB pages */
#define SWAP_BATCH      128

static uint32_t   slot_map[SWAP_SLOTS / 32];
static uint32_t   slots_used;
static spinlock_t slot_lock = SPINLOCK_INIT;

static struct file* g_store;
static int          g_slot0_taken;
static char         g_store_path[128];
static uint8_t      g_buf[4096];               /* one page; the work runs serialised */

/* ---- slots ---------------------------------------------------------------- */

static int slot_alloc(void) {
    uint32_t fl = spin_lock_irqsave(&slot_lock);
    if (!g_slot0_taken) { slot_map[0] |= 1u; g_slot0_taken = 1; }
    for (uint32_t w = 0; w < SWAP_SLOTS / 32; w++) {
        if (slot_map[w] == 0xFFFFFFFFu) continue;
        for (int b = 0; b < 32; b++) {
            if (slot_map[w] & (1u << b)) continue;
            slot_map[w] |= 1u << b;
            slots_used++;
            spin_unlock_irqrestore(&slot_lock, fl);
            /* Slot 0 is never handed out: an entry of all zeros must never
             * look like "page in slot 0" to anybody reading a table. */
            return (int)(w * 32 + (uint32_t)b);
        }
    }
    spin_unlock_irqrestore(&slot_lock, fl);
    return -1;
}

/* Called by the VMM when an evicted entry dies with its space, and here when
 * a page comes back.  A double release is refused and SAID: it would mean two
 * entries claimed one slot. */
void swap_slot_release(uint32_t slot) {
    if (slot == 0 || slot >= SWAP_SLOTS) return;
    uint32_t fl = spin_lock_irqsave(&slot_lock);
    uint32_t w = slot / 32, b = slot % 32;
    int was = (slot_map[w] >> b) & 1u;
    if (was) { slot_map[w] &= ~(1u << b); slots_used--; }
    spin_unlock_irqrestore(&slot_lock, fl);
    if (!was) klog(KLOG_WARN, "swap", "slot %u released twice\n", slot);
}

void swap_stats(uint32_t* used, uint32_t* total) {
    if (used) *used = slots_used;
    if (total) *total = SWAP_SLOTS;
}

/* ---- the store ------------------------------------------------------------ */

/* The file, opened once and kept.  Its absence is an HONEST DECLINE: a machine
 * with no writable volume still pauses, it just keeps the memory. */
static int store_open(const char** why) {
    if (g_store) return 0;
    const char* want = config_get("mem.suspend_store", "");
    if (want && want[0]) {
        int n = 0;
        while (want[n] && n < (int)sizeof g_store_path - 1) { g_store_path[n] = want[n]; n++; }
        g_store_path[n] = 0;
    } else {
        const char* pp = config_persist_path();        /* "<vol>/d-os.conf" */
        if (!pp) { *why = "no writable volume to put it on"; return -1; }
        int last = -1, n = 0;
        for (int i = 0; pp[i]; i++) if (pp[i] == '/') last = i;
        for (int i = 0; i < last && n < (int)sizeof g_store_path - 12; i++) g_store_path[n++] = pp[i];
        const char* leaf = "/swapfile";
        for (int i = 0; leaf[i]; i++) g_store_path[n++] = leaf[i];
        g_store_path[n] = 0;
    }
    g_store = vfs_open(g_store_path, VFS_RDWR | VFS_CREATE);
    if (!g_store) { *why = "the swap store could not be opened"; return -1; }
    /* Other programs' memory lives in this file: private to the system. */
    vfs_chmod(g_store_path, 0600);
    return 0;
}

/* ---- one request, run on a kworker ---------------------------------------- */

struct swap_req {
    struct work w;
    int         op;                 /* 1 = evict, 2 = restore, 3 = one page back */
    struct task* t;
    struct vmm_space* mm;           /* op 3 */
    uintptr_t   va;                 /* op 3 */
    struct swap_report rep;
    volatile int done;
};

struct cand { uintptr_t va; uint64_t phys; uint32_t flags; };
struct collect {
    struct cand c[SWAP_BATCH];
    int n;
    int first_pass;
    struct swap_report* rep;
};

static void collect_cb(void* ctx, uintptr_t va, uint64_t phys, uint32_t flags) {
    struct collect* col = (struct collect*)ctx;
    int skip = 0;
    /* Most specific reason first: a placed driver's DMA buffer is ALSO mapped
     * VMM_SHARED, and "a device writes here" is the reason that matters. */
    if (!pmm_frame_is_managed((pmm_phys_t)phys))
                                          { skip = 1; if (col->first_pass) col->rep->kept_device++; }
    else if (drv_res_phys_in_dma(phys))   { skip = 1; if (col->first_pass) col->rep->kept_dma++; }
    else if (flags & VMM_SHARED)          { skip = 1; if (col->first_pass) col->rep->kept_shared++; }
    else if (flags & VMM_COW)             { skip = 1; if (col->first_pass) col->rep->kept_cow++; }
    if (skip || col->n >= SWAP_BATCH) return;
    col->c[col->n].va = va;
    col->c[col->n].phys = phys;
    col->c[col->n].flags = flags;
    col->n++;
}

static void do_evict(struct swap_req* r) {
    struct task* t = r->t;
    struct swap_report* rep = &r->rep;
    if (store_open(&rep->why) != 0) { rep->result = SWAP_DECLINED; return; }
    struct collect* col = (struct collect*)kmalloc(sizeof *col);
    if (!col) { rep->why = "no memory for the work list"; rep->result = SWAP_DECLINED; return; }
    col->rep = rep;
    col->first_pass = 1;
    for (;;) {
        col->n = 0;
        vmm_space_walk(t->mm, collect_cb, col);
        col->first_pass = 0;
        if (col->n == 0) break;
        int progressed = 0;
        for (int i = 0; i < col->n; i++) {
            int slot = slot_alloc();
            if (slot < 0) { rep->why = "the swap store is full"; goto out; }
            uint64_t t0 = timer_now_ns();
            void* src = kmap_frame((pmm_phys_t)col->c[i].phys);
            for (int k = 0; k < 4096; k++) g_buf[k] = ((uint8_t*)src)[k];
            kunmap_frame(src);
            g_store->pos = (uint64_t)slot * 4096u;
            uint64_t t1 = timer_now_ns();
            if (vfs_write(g_store, g_buf, 4096) != 4096) {
                swap_slot_release((uint32_t)slot);
                rep->why = "a write to the swap store failed";
                goto out;
            }
            uint64_t t2 = timer_now_ns();
            rep->io_us += (uint32_t)((t2 - t1) / 1000u);
            rep->copy_us += (uint32_t)((t1 - t0) / 1000u);
            /* ORDER: the bytes are in the store BEFORE the entry says so, and
             * the frame is freed only AFTER nothing maps it. */
            if (vmm_space_mark_swapped(t->mm, col->c[i].va, (uint32_t)slot,
                                       col->c[i].flags) != 0) {
                swap_slot_release((uint32_t)slot);
                continue;
            }
            pmm_free_frame((pmm_phys_t)col->c[i].phys);
            rep->map_us += (uint32_t)((timer_now_ns() - t2) / 1000u);
            __atomic_add_fetch(&t->swapped_pages, 1, __ATOMIC_RELAXED);
            rep->evicted++;
            progressed = 1;
        }
        if (!progressed) break;
    }
out:
    kfree(col);
    rep->result = rep->evicted ? SWAP_OK : SWAP_DECLINED;
    if (!rep->evicted && !rep->why) rep->why = "nothing it holds may leave memory";
}

struct scollect { uintptr_t va[SWAP_BATCH]; uint32_t slot[SWAP_BATCH]; uint32_t fl[SWAP_BATCH]; int n; };
static void scollect_cb(void* ctx, uintptr_t va, uint32_t slot, uint32_t flags) {
    struct scollect* c = (struct scollect*)ctx;
    if (c->n >= SWAP_BATCH) return;
    c->va[c->n] = va; c->slot[c->n] = slot; c->fl[c->n] = flags; c->n++;
}

static void do_restore(struct swap_req* r) {
    struct task* t = r->t;
    struct swap_report* rep = &r->rep;
    if (!g_store) { rep->why = "the swap store is gone"; rep->result = SWAP_FAILED; return; }
    struct scollect* c = (struct scollect*)kmalloc(sizeof *c);
    if (!c) { rep->why = "no memory for the work list"; rep->result = SWAP_FAILED; return; }
    rep->result = SWAP_OK;
    for (;;) {
        c->n = 0;
        vmm_space_walk_swapped(t->mm, scollect_cb, c);
        if (c->n == 0) break;
        for (int i = 0; i < c->n; i++) {
            /* The reserve applies: bringing a program back is a user
             * allocation like any other, and it must not eat the memory the
             * system is keeping. */
            pmm_phys_t f = pmm_alloc_frame_user();
            if (f == PMM_ALLOC_FAIL) {
                rep->why = "not enough memory to bring it back";
                rep->result = SWAP_FAILED;
                goto out;
            }
            g_store->pos = (uint64_t)c->slot[i] * 4096u;
            if (vfs_read(g_store, g_buf, 4096) != 4096) {
                pmm_free_frame(f);
                rep->why = "a read from the swap store failed";
                rep->result = SWAP_FAILED;
                goto out;
            }
            void* dst = kmap_frame(f);
            for (int k = 0; k < 4096; k++) ((uint8_t*)dst)[k] = g_buf[k];
            kunmap_frame(dst);
            if (vmm_space_map(t->mm, c->va[i], f, c->fl[i] | VMM_USER) != 0) {
                pmm_free_frame(f);
                rep->why = "the page could not be mapped back";
                rep->result = SWAP_FAILED;
                goto out;
            }
            swap_slot_release(c->slot[i]);
            __atomic_sub_fetch(&t->swapped_pages, 1, __ATOMIC_RELAXED);
            rep->restored++;
        }
    }
out:
    kfree(c);
}

/* §M74 rung 3 — ONE page back, for a fault.  Re-checked here, under the
 * serialising mutex: two threads of one program can fault the same page, and
 * the second must find it already present rather than read a released slot. */
static uint32_t g_swapins, g_swapin_fail;
static uint64_t g_in_read_ns, g_in_total_ns;       /* where a swap-in's time goes */
static void do_restore_one_body(struct swap_req* r, uint64_t t0);
static void do_restore_one(struct swap_req* r) {
    uint64_t t0 = timer_now_ns();
    do_restore_one_body(r, t0);
    g_in_total_ns += timer_now_ns() - t0;
}
static void do_restore_one_body(struct swap_req* r, uint64_t t0) {
    (void)t0;
    struct swap_report* rep = &r->rep;
    uint32_t slot, fl;
    rep->result = SWAP_OK;
    if (vmm_space_swapped_entry(r->mm, r->va, &slot, &fl) != 0) return;   /* already back */
    if (!g_store) { rep->why = "the swap store is gone"; rep->result = SWAP_FAILED; return; }
    pmm_phys_t f = pmm_alloc_frame_user();
    if (f == PMM_ALLOC_FAIL) { pcache_reclaim(64); f = pmm_alloc_frame_user(); }
    if (f == PMM_ALLOC_FAIL) {
        rep->why = "no memory to read the page back into";
        rep->result = SWAP_FAILED;
        return;
    }
    g_store->pos = (uint64_t)slot * 4096u;
    uint64_t tr = timer_now_ns();
    ssize_t got = vfs_read(g_store, g_buf, 4096);
    g_in_read_ns += timer_now_ns() - tr;
    if (got != 4096) {
        pmm_free_frame(f);
        rep->why = "a read from the swap store failed";
        rep->result = SWAP_FAILED;
        return;
    }
    void* dst = kmap_frame(f);
    for (int k = 0; k < 4096; k++) ((uint8_t*)dst)[k] = g_buf[k];
    kunmap_frame(dst);
    if (vmm_space_map(r->mm, r->va, f, fl | VMM_USER) != 0) {
        pmm_free_frame(f);
        rep->why = "the page could not be mapped back";
        rep->result = SWAP_FAILED;
        return;
    }
    swap_slot_release(slot);
    rep->restored = 1;
    g_swapins++;
}

static void swap_work(struct work* w) {
    struct swap_req* r = (struct swap_req*)w;
    if (r->op == 1) do_evict(r);
    else if (r->op == 3) do_restore_one(r);
    else do_restore(r);
    __atomic_store_n(&r->done, 1, __ATOMIC_RELEASE);
}

/* Run one request on a kworker and wait for it.  One at a time: the store's
 * file position and the page buffer are shared. */
static struct kmutex g_swap_mx = KMUTEX_INIT("swap");
static void run_req(struct swap_req* r) {
    kmutex_lock(&g_swap_mx);
    work_init(&r->w, swap_work);
    r->done = 0;
    if (work_submit(&r->w) != 0) {
        r->rep.why = "no worker to run it";
        r->rep.result = SWAP_DECLINED;
    } else {
        while (!__atomic_load_n(&r->done, __ATOMIC_ACQUIRE)) task_msleep(2);
    }
    kmutex_unlock(&g_swap_mx);
}

/* Both entry points expect the caller to hold the task's swap claim
 * (task_swap_claim), which is what keeps a cont or a kill from resuming the
 * task while its tables are being rewritten. */
int swap_evict_task(struct task* t, struct swap_report* out) {
    struct swap_req* r = (struct swap_req*)kcalloc(1, sizeof *r);
    if (!r) return -1;
    if (!t || t->state != TASK_STOPPED || !t->mm || !t->swap_busy) {
        r->rep.why = "it is not paused";
        r->rep.result = SWAP_DECLINED;
    } else {
        r->op = 1; r->t = t;
        run_req(r);
    }
    if (out) *out = r->rep;
    int rc = r->rep.result == SWAP_OK ? 0 : -1;
    kfree(r);
    return rc;
}

int swap_restore_task(struct task* t, struct swap_report* out) {
    struct swap_req* r = (struct swap_req*)kcalloc(1, sizeof *r);
    if (!r) return -1;
    r->op = 2; r->t = t;
    run_req(r);
    if (out) *out = r->rep;
    int rc = r->rep.result == SWAP_OK ? 0 : -1;
    kfree(r);
    return rc;
}

/* Pause, wait for the pause to take effect, and evict — the one sequence the
 * shell's `stop -e` and the Task Manager's button both run, so the two cannot
 * drift.  0 = something was evicted; -1 = paused but kept its memory (the
 * report says why); -2 = it never stopped (why is set). */
int swap_stop_evict(int pid, struct swap_report* rep) {
    for (unsigned i = 0; i < sizeof *rep; i++) ((char*)rep)[i] = 0;
    if (task_stop(pid) != 0) { rep->why = "cannot be paused"; return -2; }
    struct task* t = task_find(pid);
    for (int i = 0; i < 100 && t && t->state != TASK_STOPPED && t->state != TASK_DEAD; i++) {
        task_msleep(20);
        t = task_find(pid);
    }
    if (!t || t->state != TASK_STOPPED) { rep->why = "did not stop within 2 s"; return -2; }
    if (task_swap_claim(t) != 0) { rep->why = "it is already being swapped"; return -1; }
    swap_evict_task(t, rep);
    task_swap_release(t, 1);
    return rep->evicted ? 0 : -1;
}

/* The task that OWNS an address space (its counter is the one to adjust): the
 * faulting task itself unless it is a thread borrowing the space. */
struct owner_find { struct vmm_space* mm; struct task* owner; };
static void owner_cb(const struct task* t, int cur, void* c) {
    (void)cur;
    struct owner_find* o = (struct owner_find*)c;
    if (t->mm == o->mm && !t->mm_shared && t->state != TASK_DEAD) o->owner = (struct task*)t;
}

/* §M74 rung 3 — a fault on an evicted page reads it back and retries.
 * `can_sleep` is the ARCH's judgement: the fault came from user mode, or from
 * a kernel access made with interrupts on and no spinlock held — reading the
 * store sleeps, and sleeping anywhere else is a deadlock or a corrupted lock.
 * Returns 1 when the page is back (retry the instruction), 0 when this was not
 * an evicted page or it cannot come back (the fault is then a real one). */
int swap_in_fault(uintptr_t va, int can_sleep) {
    struct task* t = task_current();
    if (!t || !t->mm) return 0;
    va &= ~(uintptr_t)4095;
    uint32_t slot, fl;
    if (vmm_space_swapped_entry(t->mm, va, &slot, &fl) != 0) return 0;
    if (!can_sleep) return 0;
    struct swap_req req;
    for (unsigned i = 0; i < sizeof req; i++) ((char*)&req)[i] = 0;
    req.op = 3; req.mm = t->mm; req.va = va;
    uint32_t ifl = hal_intr_save();
    hal_intr_enable();                        /* the read sleeps */
    /* Once the store is open, a swap-in needs no worker: the kworker exists so
     * the store is CREATED with SYSTEM credentials, and reading an open file
     * checks nothing.  Going direct removes a queue round trip and a polling
     * sleep from every page that comes back. */
    if (g_store) {
        kmutex_lock(&g_swap_mx);
        do_restore_one(&req);
        kmutex_unlock(&g_swap_mx);
    } else {
        run_req(&req);
    }
    hal_intr_restore(ifl);
    if (req.rep.result != SWAP_OK) {
        if (!g_swapin_fail++)
            kprintf("swap: pid %d '%s' - page %p cannot come back: %s\n",
                    t->pid, t->name, (void*)va, req.rep.why ? req.rep.why : "?");
        return 0;
    }
    if (req.rep.restored) {
        struct owner_find o = { t->mm, t->mm_shared ? NULL : t };
        if (!o.owner) task_for_each(owner_cb, &o);
        if (o.owner && o.owner->swapped_pages)
            __atomic_sub_fetch(&o.owner->swapped_pages, 1, __ATOMIC_RELAXED);
    }
    return 1;
}
uint32_t swap_in_count(void) { return g_swapins; }
void swap_in_timing(uint32_t* read_us, uint32_t* total_us) {
    *read_us = (uint32_t)(g_in_read_ns / 1000u);
    *total_us = (uint32_t)(g_in_total_ns / 1000u);
}

/* ---- the invariant -------------------------------------------------------- */

static void count_cb(void* ctx, uintptr_t va, uint32_t slot, uint32_t flags) {
    (void)va; (void)slot; (void)flags;
    (*(uint32_t*)ctx)++;
}

struct audit_ctx { int bad; int verbose; int any; };
static void audit_task_cb(const struct task* t, int cur, void* c) {
    (void)cur;
    struct audit_ctx* a = (struct audit_ctx*)c;
    if (!t->mm || t->state == TASK_DEAD) return;
    uint32_t n = 0;
    vmm_space_walk_swapped(t->mm, count_cb, &n);
    if (!n && !t->swapped_pages) return;
    a->any++;
    if (t->state != TASK_STOPPED) {
        a->bad++;
        kprintf("audit swap-runnable: pid %d '%s' can run with %u evicted page(s)\n",
                t->pid, t->name, n);
    } else if (n != t->swapped_pages) {
        a->bad++;
        kprintf("audit swap-runnable: pid %d '%s' - %u evicted entries, counter says %u\n",
                t->pid, t->name, n, (unsigned)t->swapped_pages);
    } else if (a->verbose) {
        kprintf("audit swap-runnable: pid %d '%s' paused with %u page(s) evicted\n",
                t->pid, t->name, n);
    }
}
int swap_audit(int verbose) {
    struct audit_ctx a = { 0, verbose, 0 };
    task_for_each(audit_task_cb, &a);
    return a.any ? a.bad : AUDIT_SKIP;
}
AUDIT(swap_runnable) = {
    .name = "swap-runnable",
    .what = "no task that can run has an evicted page, and every paused one's count matches its tables",
    .run  = swap_audit,
};

CONFIG_KEY(ck_suspend_store) = {
    .key = "mem.suspend_store", .group = "System", .type = CFG_STRING, .def = "",
    .help = "where a paused program's memory is written when it is evicted "
            "(empty = a 'swapfile' on the persistent volume)",
};
