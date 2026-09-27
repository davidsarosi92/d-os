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
#include "memage.h"
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

/* §M74 rung 4 — THE SIZE IS ALSO THE CEILING: `mem.swap_size_mb` (one
 * number, one meaning) bounds how many slots may be in use.  Reaching it is a
 * REFUSAL — the eviction stops and the reserve refuses the allocation that
 * needed the room — never a spiral.  Read once per allocation: cheap, and a
 * changed size applies at once rather than at a "next enable" nobody sees. */
static uint32_t g_cap_slots = SWAP_SLOTS;
static void refresh_cap(void) {
    const char* v = config_get("mem.swap_size_mb", "64");
    uint32_t mb = 0;
    for (; v && *v >= '0' && *v <= '9'; v++) mb = mb * 10 + (uint32_t)(*v - '0');
    if (mb == 0) mb = 64;
    uint32_t cap = mb * 256u;
    g_cap_slots = cap > SWAP_SLOTS ? SWAP_SLOTS : cap;
}
static int slot_alloc(void) {
    if (slots_used + 1 >= g_cap_slots) return -1;          /* the ceiling */
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

/* ---- §M74 rung 3: eviction under pressure ------------------------------------
 *
 * THE VICTIM IS NOT PAUSED.  A program that is idle sleeps inside a system
 * call and never reaches a ring-3 safe point, so pausing would reach exactly
 * the memory that is warm and never the memory that is cold.  Instead each
 * page is taken with vmm_space_swap_out: the entry becomes the evicted marker
 * atomically and the TLB is shot down FIRST, so from then on any access by any
 * CPU faults — and the swap-in waits on g_swap_mx, which this holds until the
 * page is written.  Only then is the frame copied (no write can race the copy)
 * and freed.  The frame freed is the one that was in the entry at that moment,
 * which a concurrent munmap/mmap by the owner cannot make wrong.
 *
 * WHAT IS TAKEN: private pages the accessed-bit sweep has seen idle for at
 * least `min_age` sweeps (rung 1's data; without it this would be random
 * eviction).  Never shared, COW, device or DMA pages — checked on the entry
 * actually swapped out, and put back exactly if it turns out to be one.
 *
 * RULE 3 — THE WRITE-OUT ALLOCATES NOTHING: the candidate list is static and
 * the store must already be open (opening it can allocate); pressure never
 * opens it.  The kernel frames freed here are what the reserve is waiting on,
 * so an allocation on this path would fail exactly when it is called. */
#define PRESSURE_BATCH 64
static struct cand g_pcand[PRESSURE_BATCH];
static uint32_t g_pressure_out, g_pressure_passes, g_pressure_full;
struct pcollect { uint32_t n, min_age; };
static void pcollect_cb(void* c, uintptr_t va, uint64_t phys, uint32_t flags) {
    struct pcollect* x = (struct pcollect*)c;
    if (x->n >= PRESSURE_BATCH) return;
    if (flags & (VMM_SHARED | VMM_COW)) return;
    if (!pmm_frame_is_managed((pmm_phys_t)phys) || drv_res_phys_in_dma(phys)) return;
    int age = memage_frame_age(phys);
    if (age < 0 || (uint32_t)age < x->min_age) return;
    g_pcand[x->n].va = va; g_pcand[x->n].phys = phys; g_pcand[x->n].flags = flags;
    x->n++;
}
/* Caller holds g_swap_mx and has pinned `t`.  Returns pages written out.
 *
 * TWO PHASES, ONE TLB FLUSH (2026-09-27).  The first version shot the TLB down
 * once PER PAGE — 64 IPI round trips a pass — and under `swapracetest`'s
 * 10 ms passes on i386 that starved CPU 0 of emulated time until the ib700
 * went four seconds unfed (twice, "host stall?" both times, both in heavy
 * eviction).  Now every page of the pass is marked first, then ONE full flush,
 * then each is copied and written.  The copy still happens only after the
 * flush, so no write through a stale entry can race it. */
static struct { uint64_t raw, phys; uint32_t slot; } g_pout[PRESSURE_BATCH];
static uint32_t evict_cold_locked(struct task* t, struct vmm_space* mm, uint32_t min_age) {
    struct pcollect x = { 0, min_age };
    vmm_space_walk(mm, pcollect_cb, &x);
    uint32_t m = 0;
    for (uint32_t i = 0; i < x.n; i++) {                   /* phase 1: mark */
        int slot = slot_alloc();
        if (slot < 0) { g_pressure_full++; break; }         /* the ceiling */
        uint64_t raw, phys; uint32_t fl;
        if (vmm_space_swap_out(mm, g_pcand[i].va, (uint32_t)slot, &raw, &phys, &fl, 0) != 0) {
            swap_slot_release((uint32_t)slot);
            continue;                                        /* gone meanwhile */
        }
        if ((fl & (VMM_SHARED | VMM_COW)) || !pmm_frame_is_managed((pmm_phys_t)phys) ||
            drv_res_phys_in_dma(phys)) {                     /* changed meanwhile */
            vmm_space_swap_undo(mm, g_pcand[i].va, raw);
            swap_slot_release((uint32_t)slot);
            continue;
        }
        g_pcand[m].va = g_pcand[i].va;
        g_pout[m].raw = raw; g_pout[m].phys = phys; g_pout[m].slot = (uint32_t)slot;
        m++;
    }
    if (!m) return 0;
    vmm_age_flush();                                         /* one flush, everywhere */
    uint32_t out = 0;
    for (uint32_t i = 0; i < m; i++) {                       /* phase 2: copy, write, free */
        void* src = kmap_frame((pmm_phys_t)g_pout[i].phys);
        for (int k = 0; k < 4096; k++) g_buf[k] = ((uint8_t*)src)[k];
        kunmap_frame(src);
        g_store->pos = (uint64_t)g_pout[i].slot * 4096u;
        if (vfs_write(g_store, g_buf, 4096) != 4096) {
            /* the frames of this and every later page are still ours: put
             * them all back */
            for (uint32_t j = i; j < m; j++) {
                vmm_space_swap_undo(mm, g_pcand[j].va, g_pout[j].raw);
                swap_slot_release(g_pout[j].slot);
            }
            break;
        }
        pmm_free_frame((pmm_phys_t)g_pout[i].phys);
        __atomic_add_fetch(&t->swapped_pages, 1, __ATOMIC_RELAXED);
        out++;
    }
    return out;
}

/* The victim: a program owning its space, pinnable, with the most memory the
 * sweep calls cold.  task_for_each gives candidates; the split is taken
 * outside its lock. */
struct victim_find { int pids[32]; int n; };
static void victim_cb(const struct task* t, int cur, void* c) {
    (void)cur;
    struct victim_find* v = (struct victim_find*)c;
    if (!t->user_task || !t->mm || t->mm_shared || t->state == TASK_DEAD || t->swap_busy) return;
    if (v->n < 32) v->pids[v->n++] = t->pid;
}
static int pick_victim_pid(uint64_t* cold_out) {
    struct victim_find v = { {0}, 0 };
    task_for_each(victim_cb, &v);
    int best = -1; uint64_t best_cold = 0;
    for (int i = 0; i < v.n; i++) {
        uint64_t h = 0, w = 0, c = 0;
        if (memage_task_split(v.pids[i], &h, &w, &c) != 0) continue;
        uint64_t score = c + (w >> 2);           /* cold first, a little warm counts */
        if (score > best_cold) { best_cold = score; best = v.pids[i]; }
    }
    if (cold_out) *cold_out = best_cold;
    return best;
}

int swap_policy(void) {
    const char* v = config_get("mem.swap_policy", "off");
    if (!v || v[0] == 'o') return 0;             /* off */
    return v[0] == 'e' ? 1 : 2;                  /* emergency : normal */
}

/* Called by the memage service after the page cache has had its turn.
 * `pressure`: free memory is within reach of the reserve.  Returns pages
 * written out.  emergency = only under pressure, anything idle for 2 sweeps;
 * normal = also without pressure, only what is cold. */
uint32_t swap_pressure(int pressure) {
    int pol = swap_policy();
    if (!pol) return 0;
    if (pol == 1 && !pressure) return 0;
    refresh_cap();
    if (!g_store) {
        /* Opening can allocate — do it now, while there is still room, not on
         * the path that runs when there is none (rule 3). */
        const char* why = NULL;
        if (store_open(&why) != 0) {
            static int told;
            if (!told++) kprintf("swap: %s is on but %s - nothing will be written out\n",
                                 pol == 1 ? "emergency swap" : "swap", why);
            return 0;
        }
    }
    uint32_t min_age = pressure ? 2u : memage_cold_rounds();
    /* `swap.test_min_age` — a test knob: 0 evicts pages that are IN USE, which
     * is what the in-flight race falsifier (`swapracetest`) needs. */
    {
        const char* tv = config_get("swap.test_min_age", 0);
        if (tv && *tv >= '0' && *tv <= '9') min_age = (uint32_t)(*tv - '0');
    }
    int pid = pick_victim_pid(NULL);
    if (pid < 0) return 0;
    struct task* t = task_find(pid);
    if (!t || task_swap_pin(t) != 0) return 0;
    kmutex_lock(&g_swap_mx);
    uint32_t n = t->mm ? evict_cold_locked(t, t->mm, min_age) : 0;
    kmutex_unlock(&g_swap_mx);
    task_swap_release(t, 0);
    g_pressure_out += n;
    g_pressure_passes++;
    return n;
}
void swap_pressure_stats(uint32_t* out, uint32_t* passes, uint32_t* full) {
    *out = g_pressure_out; *passes = g_pressure_passes; *full = g_pressure_full;
}

CONFIG_KEY(ck_swap_policy) = {
    .key = "mem.swap_policy", .group = "System", .type = CFG_ENUM,
    .values = "off emergency normal", .def = "off",
    .help = "write idle program memory to disk: off, only when memory runs out, or routinely",
};
CONFIG_KEY(ck_swap_size) = {
    .key = "mem.swap_size_mb", .group = "System", .type = CFG_INT, .min = 1, .max = 256,
    .def = "64",
    .help = "the swap area's size - and the most that may ever be swapped out",
};

/* `thrashtest` (hidden, §M74 rung 4 and the three rules).
 *
 *   rule 2 — switching swap on (the store opened) must not move the free
 *            figure the watermark reads: swap is never free memory.
 *   rule 3 — an eviction pass with THIS task's allocations forced to fail must
 *            still write pages out: the write-out needs no memory.
 *   rung 4 — an 8 MB swap area and `memhog fill`: the area fills to its
 *            ceiling and STOPS, the reserve then refuses memhog, and the
 *            machine stays usable — measured as scheduling lateness of a 20 ms
 *            sleep sampled throughout, because "it did not crash" and "it was
 *            usable" are different claims.
 *   rule 1 — by construction here (the reserve is a count of free frames,
 *            which eviction only ever raises, and nothing but user spaces is
 *            walked): shown by a KERNEL allocation succeeding during the
 *            refusal. */
extern const unsigned char _binary_user_memhog_elf_start[]         __attribute__((weak));
extern const unsigned char _binary_user_memhog_elf_end[]           __attribute__((weak));
extern const unsigned char _binary_user_memhog_x86_64_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_memhog_x86_64_elf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_memhog_aarch64_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_memhog_aarch64_elf_end[]   __attribute__((weak));
#include "proc.h"
#include "shellcmd.h"
static void cmd_thrashtest(const char* args) {
    (void)args;
    const unsigned char *s = 0, *e = 0;
    if (_binary_user_memhog_elf_start)             { s = _binary_user_memhog_elf_start;         e = _binary_user_memhog_elf_end; }
    else if (_binary_user_memhog_x86_64_elf_start) { s = _binary_user_memhog_x86_64_elf_start;  e = _binary_user_memhog_x86_64_elf_end; }
    else if (_binary_user_memhog_aarch64_elf_start){ s = _binary_user_memhog_aarch64_elf_start; e = _binary_user_memhog_aarch64_elf_end; }
    if (!s) { kprintf("thrashtest: no memhog embedded\n"); return; }
    int bad = 0;

    /* rule 2 */
    config_set("mem.swap_size_mb", "8");
    uint32_t f0 = pmm_free_frames();
    config_set("mem.swap_policy", "emergency");
    { const char* why = NULL; if (!g_store && store_open(&why) != 0) {
        kprintf("thrashtest: SKIP - %s\n", why ? why : "no store"); config_set("mem.swap_policy", "off"); return; } }
    uint32_t f1 = pmm_free_frames();
    int moved = (int)f1 - (int)f0;
    if (moved > 64 || moved < -64) { kprintf("thrashtest: rule 2 - free moved by %d frames\n", moved); bad++; }

    /* rule 3: a running program with idle pages, and a pass that cannot allocate */
    const char* av[2] = { "memhog", "age" };
    int pa = proc_spawn_argv("rule3-prog", s, (size_t)(e - s), 2, av, 0);
    task_msleep(3000);
    memage_sweep(); memage_sweep(); memage_sweep();
    struct task* me = task_current();
    config_set("swap.test_min_age", "1");
    config_set("mem.swap_policy", "normal");
    me->alloc_fail = 1;
    uint32_t r3 = swap_pressure(0);
    me->alloc_fail = 0;
    config_set("swap.test_min_age", "");
    config_set("mem.swap_policy", "emergency");
    if (r3 == 0) { kprintf("thrashtest: rule 3 - nothing written with allocations failing\n"); bad++; }
    task_kill_tree(pa);
    task_msleep(500);

    /* rung 4 */
    uint32_t rkb, refused0, refused1; int low;
    pmm_reserve_stats(&rkb, &refused0, &low);
    uint32_t out0, pass0, full0, out1, pass1, full1;
    swap_pressure_stats(&out0, &pass0, &full0);
    const char* fv[2] = { "memhog", "press" };
    int pf = proc_spawn_argv("thrash-prog", s, (size_t)(e - s), 2, fv, 0);
    uint64_t worst = 0, t_end = timer_ticks_ms() + 60000;
    int kernel_alloc_ok = 1, at_ceiling = 0;
    uint32_t refused_at_ceiling = 0;
    while (timer_ticks_ms() < t_end) {
        uint64_t a = timer_now_ns();
        task_msleep(20);
        uint64_t late = (timer_now_ns() - a) / 1000000u;
        late = late > 20 ? late - 20 : 0;
        if (late > worst) worst = late;
        pmm_reserve_stats(&rkb, &refused1, &low);
        if (!at_ceiling && slots_used + 1 >= g_cap_slots) {
            at_ceiling = 1;
            refused_at_ceiling = refused1;
        }
        /* At the ceiling and refused again since: the stop is holding.  The
         * kernel must still get memory at that moment (rule 1). */
        if (at_ceiling && refused1 > refused_at_ceiling + 5) {
            void* k = kmalloc(16384);
            if (!k) kernel_alloc_ok = 0; else kfree(k);
            break;
        }
    }
    swap_pressure_stats(&out1, &pass1, &full1);
    uint32_t used = slots_used;
    task_kill_tree(pf);
    config_set("mem.swap_policy", "off");
    config_set("mem.swap_size_mb", "64");
    int refusal = at_ceiling && refused1 > refused_at_ceiling + 5;
    int ceiling = at_ceiling && full1 > full0;
    if (!refusal)         { kprintf("thrashtest: rung 4 - the fill was never refused\n"); bad++; }
    if (!ceiling)         { kprintf("thrashtest: rung 4 - the swap area did not reach its ceiling (%u slots)\n", used); bad++; }
    if (!kernel_alloc_ok) { kprintf("thrashtest: rule 1 - a kernel allocation failed during the refusal\n"); bad++; }
    if (worst > 1000)     { kprintf("thrashtest: rung 4 - a 20 ms sleep ran %u ms late\n", (unsigned)worst); bad++; }
    kprintf("thrashtest: rule 2 free moved %d frames; rule 3 wrote %u page(s) with allocations failing; "
            "rung 4 wrote %u page(s) out, %u slot(s) at the 8 MB ceiling, refused past it %s, worst lateness %u ms "
            "-> %s\n", moved, r3, out1 - out0, used, refusal ? "yes" : "NO", (unsigned)worst,
            bad ? "FAIL" : "PASS");
}
/* `swap` — what the swap machinery is doing, in one place. */
static void cmd_swap(const char* args) {
    (void)args;
    refresh_cap();
    uint32_t out, passes, full;
    swap_pressure_stats(&out, &passes, &full);
    static const char* pol[] = { "off", "emergency (only when memory runs out)", "normal (idle pages routinely)" };
    kprintf("swap: policy %s; area %u KB (the ceiling), %u KB in use; store %s\n",
            pol[swap_policy()], g_cap_slots * 4u, slots_used * 4u,
            g_store ? g_store_path : "not open");
    kprintf("swap: written out under pressure %u page(s) in %u pass(es), ceiling reached %u time(s); "
            "brought back by fault %u, could not bring back %u\n",
            out, passes, full, g_swapins, g_swapin_fail);
}
SHELL_CMD(swap) = { "swap", "", "the swap area: policy, size, use, pages out and back",
                    SHELL_G_MEM, cmd_swap, SHELL_P_ANY };

SHELL_CMD(thrashtest) = { "thrashtest", "", 0, SHELL_G_TEST, cmd_thrashtest, SHELL_P_ADMIN };

/* ---- the invariant -------------------------------------------------------- */


/* THE INVARIANT (§M74, replacing §M72's "no runnable task has an evicted
 * page", which swap-in by fault made false by design): EVERY SLOT IS OWNED BY
 * EXACTLY ONE ENTRY.  A slot claimed twice is two pages silently sharing one
 * copy; an entry naming a slot that is not allocated can have its data
 * overwritten by the next eviction; an allocated slot nobody names is a disk
 * that fills with nothing.  Checked with the swap mutex held, so no eviction
 * or swap-in is ever seen half done.  Plus, for a paused program, its counter
 * against its tables. */
static uint32_t g_seen[SWAP_SLOTS / 32];
struct audit_ctx { int bad; int verbose; uint32_t entries; };
struct audit_one { struct audit_ctx* a; const struct task* t; uint32_t n; };
static void audit_entry_cb(void* ctx, uintptr_t va, uint32_t slot, uint32_t flags) {
    (void)flags;
    struct audit_one* o = (struct audit_one*)ctx;
    o->n++; o->a->entries++;
    if (slot == 0 || slot >= SWAP_SLOTS) {
        o->a->bad++;
        kprintf("audit swap-slots: pid %d page %p names slot %u, outside the store\n",
                o->t->pid, (void*)va, slot);
        return;
    }
    uint32_t w = slot / 32, b = 1u << (slot % 32);
    if (g_seen[w] & b) {
        o->a->bad++;
        kprintf("audit swap-slots: slot %u is named TWICE (again by pid %d at %p)\n",
                slot, o->t->pid, (void*)va);
    }
    g_seen[w] |= b;
    if (!(slot_map[w] & b)) {
        o->a->bad++;
        kprintf("audit swap-slots: pid %d page %p names slot %u, which is FREE\n",
                o->t->pid, (void*)va, slot);
    }
}
static void audit_task_cb(const struct task* t, int cur, void* c) {
    (void)cur;
    struct audit_ctx* a = (struct audit_ctx*)c;
    if (!t->mm || t->mm_shared || t->state == TASK_DEAD) return;
    struct audit_one o = { a, t, 0 };
    vmm_space_walk_swapped(t->mm, audit_entry_cb, &o);
    if (t->state == TASK_STOPPED && o.n != t->swapped_pages) {
        a->bad++;
        kprintf("audit swap-slots: paused pid %d '%s' - %u evicted entries, counter says %u\n",
                t->pid, t->name, o.n, (unsigned)t->swapped_pages);
    }
}
int swap_audit(int verbose) {
    kmutex_lock(&g_swap_mx);
    for (uint32_t i = 0; i < SWAP_SLOTS / 32; i++) g_seen[i] = 0;
    struct audit_ctx a = { 0, verbose, 0 };
    task_for_each(audit_task_cb, &a);
    uint32_t orphans = 0;
    for (uint32_t w = 0; w < SWAP_SLOTS / 32; w++) {
        uint32_t lost = slot_map[w] & ~g_seen[w];
        if (w == 0) lost &= ~1u;                 /* slot 0 is reserved, never owned */
        for (; lost; lost &= lost - 1) orphans++;
    }
    kmutex_unlock(&g_swap_mx);
    if (orphans) {
        a.bad++;
        kprintf("audit swap-slots: %u allocated slot(s) no page names - the store leaks\n", orphans);
    }
    if (verbose)
        kprintf("audit swap-slots: %u evicted page(s), %u slot(s) in use\n", a.entries, slots_used);
    if (!a.entries && !slots_used && !orphans) return AUDIT_SKIP;
    return a.bad;
}
AUDIT(swap_slots) = {
    .name = "swap-slots",
    .what = "every swap slot is owned by exactly one evicted page, and no page names a free slot",
    .run  = swap_audit,
};

/* The falsifier: allocate a slot no page names, and the audit must say so;
 * give it back, and it must be clean again.  Returns 10 for "seen, then
 * cleared". */
int swap_audit_selftest(void) {
    int slot = slot_alloc();
    if (slot < 0) return -1;
    int seen = swap_audit(0) > 0;
    swap_slot_release((uint32_t)slot);
    int after = swap_audit(0);
    return seen * 10 + (after > 0);
}

CONFIG_KEY(ck_suspend_store) = {
    .key = "mem.suspend_store", .group = "System", .type = CFG_STRING, .def = "",
    .help = "where a paused program's memory is written when it is evicted "
            "(empty = a 'swapfile' on the persistent volume)",
};
