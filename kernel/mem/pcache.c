/* =============================================================================
 * pcache.c — one copy of a file page, however many programs map it
 * (§M74 rung 2, 2026-09-27).
 *
 * WHAT WAS TRUE: a file mapping was an EAGER PRIVATE COPY — sys_mmap_full
 * read the file into fresh anonymous frames — so two programs mapping the same
 * libc.so held two full copies of it, and every dynamic program on the machine
 * paid for its own musl, libstdc++, Mesa.  There was no page cache at all:
 * block_cache.c caches disk SECTORS under the filesystem, a different object.
 *
 * NOW: a (file, page index) → frame cache, and a PRIVATE file mapping maps the
 * cached frame COPY-ON-WRITE.  The frame's COW share count is the whole
 * lifetime story, which is why this needed no new page-table semantics:
 *
 *   - the cache holds it as one sharer, each mapping as another;
 *   - a write by a program copies first (vmm_cow_fault) — the cached frame is
 *     never written through;
 *   - teardown, munmap (§M74's releasing unmap) and a MAP_FIXED overlay drop a
 *     share; fork adds one; §M72's eviction keeps COW pages; mprotect keeps
 *     them COW (fixed the same day — it used to write through);
 *   - the cache may DROP a page only when it is the sole holder, and then it
 *     frees the frame; a page still mapped is DETACHED instead (the cache
 *     lets go, the mappings keep the frame, the last of them frees it).
 *
 * THE ONE SEMANTIC DEVIATION, stated rather than discovered: a write to a
 * READ-ONLY file page (.rodata, text) gets a private WRITABLE copy instead of
 * SIGSEGV, because the COW fault cannot tell a page that was writable before
 * it was shared from one that never was — classic i386 has no spare PTE bit to
 * remember it.  The copy keeps the page's executability, exactly as a fork's
 * COW copy does (this kernel enforces "executable only if asked", not W^X, and
 * stripping execute on every COW write would break a forked JIT).  PAE, x86_64
 * and aarch64 do have spare bits; using one to refuse such writes is open.
 *
 * WHAT IT IS NOT (yet): demand paging.  Pages are still mapped when the file
 * is mapped, not when they are touched — sharing is the win measured here,
 * and mapping-on-fault needs a record of mappings (VMAs) this kernel lacks.
 * And a MAPPED cache page cannot be reclaimed: taking it back from the
 * mappings needs a reverse map.  Unmapped cache pages are reclaimable, which
 * is what `mem.swap_policy = off` still gets (§M74's rung-2 promise).
 *
 * CONSISTENCY.  A write to a file drops its cached pages (vfs_write), as do
 * truncation on open, unlink and unmount (rename cannot replace a file here —
 * it refuses an existing target — so it needs no hook).  A program that mapped the file
 * before the write keeps the old contents — POSIX leaves a MAP_PRIVATE view of
 * a concurrent write unspecified, and a snapshot is the conservative answer.
 * The key is an id given to the INODE on first use, never its address: every
 * filesystem here allocates inodes with kcalloc, so a new inode at a reused
 * address has id 0 and can never hit a dead file's pages.
 * ============================================================================= */

#include "pcache.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "kmap.h"
#include "kmutex.h"
#include "kmalloc.h"
#include "printf.h"
#include "config.h"
#include "settings.h"
#include "shellcmd.h"
#include "timer.h"
#include <stdint.h>
#include <stddef.h>

#define PC_HASH 1024

struct pc_page {
    uint32_t        id;         /* the inode's cache id                 */
    uint64_t        idx;        /* page index in the file               */
    pmm_phys_t      frame;
    uint64_t        last_ms;    /* last time a mapping asked for it     */
    struct pc_page* next;
};

static struct pc_page* g_hash[PC_HASH];
static struct kmutex   g_lock = KMUTEX_INIT("pcache");
static uint32_t        g_next_id = 1;
static struct pcache_stats g_st;

static inline uint32_t hslot(uint32_t id, uint64_t idx) {
    return (uint32_t)((id * 2654435761u) ^ (uint32_t)idx ^ (uint32_t)(idx >> 20)) & (PC_HASH - 1);
}

int pcache_enabled(void) {
    const char* v = config_get("mem.pagecache", "1");
    return v && v[0] != '0';
}

/* Read one page of `f` into a new frame.  Positioned read with the cursor
 * restored (the program owns it), through a kernel bounce page, because a
 * kmap must not be held across a sleep and vfs_read can sleep on the disk. */
static pmm_phys_t fill_page(struct file* f, uint64_t idx) {
    pmm_phys_t fr = pmm_alloc_frame_user();
    if (!fr) return 0;
    uint8_t* bounce = (uint8_t*)kmalloc(4096);
    if (!bounce) { pmm_free_frame(fr); return 0; }
    for (int b = 0; b < 4096; b++) bounce[b] = 0;
    uint64_t save = f->pos;
    f->pos = idx * 4096u;
    vfs_read(f, bounce, 4096);                 /* short tail stays zero */
    f->pos = save;
    uint8_t* p = (uint8_t*)kmap_frame(fr);
    for (int b = 0; b < 4096; b++) p[b] = bounce[b];
    kunmap_frame(p);
    kfree(bounce);
    g_st.reads++;
    return fr;
}

/* Drop one entry (lock held).  Sole holder: free.  Still mapped: detach — the
 * mappings keep the frame and the last of them frees it. */
static void drop_entry(struct pc_page* e) {
    if (vmm_frame_unshare(e->frame)) { pmm_free_frame(e->frame); g_st.freed++; }
    else                               g_st.detached++;
    g_st.pages--;
    kfree(e);
}

int pcache_map_page(struct file* f, uint64_t idx, pmm_phys_t* out) {
    if (!f || !f->inode || !out) return -1;
    struct inode* ino = f->inode;
    kmutex_lock(&g_lock);
    if (!ino->pc_id) ino->pc_id = g_next_id++;
    uint32_t id = ino->pc_id, h = hslot(id, idx);
    struct pc_page* e = g_hash[h];
    while (e && !(e->id == id && e->idx == idx)) e = e->next;
    if (e) {
        g_st.hits++;
    } else {
        pmm_phys_t fr = fill_page(f, idx);
        if (!fr) {                             /* short of memory: give some back */
            pcache_reclaim_locked(64);
            fr = fill_page(f, idx);
        }
        if (!fr) { kmutex_unlock(&g_lock); return -1; }
        e = (struct pc_page*)kcalloc(1, sizeof *e);
        if (!e) { pmm_free_frame(fr); kmutex_unlock(&g_lock); return -1; }
        e->id = id; e->idx = idx; e->frame = fr;
        e->next = g_hash[h]; g_hash[h] = e;
        g_st.pages++;
        g_st.misses++;
    }
    e->last_ms = timer_ticks_ms();
    vmm_frame_share(e->frame);                 /* the mapping's share */
    *out = e->frame;
    kmutex_unlock(&g_lock);
    return 0;
}

void pcache_invalidate(struct inode* ino) {
    if (!ino || !ino->pc_id) return;
    kmutex_lock(&g_lock);
    uint32_t id = ino->pc_id;
    for (int h = 0; h < PC_HASH; h++) {
        struct pc_page** pp = &g_hash[h];
        while (*pp) {
            struct pc_page* e = *pp;
            if (e->id == id) { *pp = e->next; drop_entry(e); g_st.invalidated++; }
            else pp = &e->next;
        }
    }
    /* A NEW id from here on: a mapping made after this must read the file
     * again, never find a page that was dropped and re-added under the old
     * id by a racing reader.  (The old pages are gone; this is belt and
     * braces for the ones a concurrent map was about to insert.) */
    ino->pc_id = 0;
    kmutex_unlock(&g_lock);
}

void pcache_drop_all(void) {
    kmutex_lock(&g_lock);
    for (int h = 0; h < PC_HASH; h++)
        while (g_hash[h]) { struct pc_page* e = g_hash[h]; g_hash[h] = e->next; drop_entry(e); }
    kmutex_unlock(&g_lock);
}

/* Give back up to `max` UNMAPPED pages, oldest first.  Only pages nobody maps
 * are candidates: dropping one frees memory at once and loses nothing, because
 * the file still holds the data.  (A mapped page would need a reverse map to
 * take back; see the header.)  Lock held.  Returns pages freed. */
uint32_t pcache_reclaim_locked(uint32_t max) {
    uint32_t freed = 0;
    while (freed < max) {
        struct pc_page** best = NULL;
        uint64_t best_ms = ~0ull;
        for (int h = 0; h < PC_HASH; h++)
            for (struct pc_page** pp = &g_hash[h]; *pp; pp = &(*pp)->next)
                if (vmm_frame_share_count((*pp)->frame) <= 1 && (*pp)->last_ms < best_ms) {
                    best = pp; best_ms = (*pp)->last_ms;
                }
        if (!best) break;
        struct pc_page* e = *best;
        *best = e->next;
        drop_entry(e);
        g_st.reclaimed++;
        freed++;
    }
    return freed;
}
uint32_t pcache_reclaim(uint32_t max) {
    kmutex_lock(&g_lock);
    uint32_t n = pcache_reclaim_locked(max);
    kmutex_unlock(&g_lock);
    return n;
}

void pcache_stats(struct pcache_stats* out) {
    kmutex_lock(&g_lock);
    *out = g_st;
    uint32_t mapped = 0;
    for (int h = 0; h < PC_HASH; h++)
        for (struct pc_page* e = g_hash[h]; e; e = e->next)
            if (vmm_frame_share_count(e->frame) > 1) mapped++;
    out->mapped = mapped;
    kmutex_unlock(&g_lock);
}

CONFIG_KEY(ck_pagecache) = {
    .key = "mem.pagecache", .group = "Memory", .type = CFG_BOOL, .def = "1",
    .help = "share file pages between the programs that map them (one copy of libc.so)",
};

static void cmd_pcache(const char* args) {
    while (args && *args == ' ') args++;
    if (args && args[0] == 'd') {
        uint32_t n = pcache_reclaim(0xFFFFFFFFu);
        kprintf("pcache: dropped %u unmapped page(s)\n", n);
        return;
    }
    struct pcache_stats st;
    pcache_stats(&st);
    kprintf("pcache: %s - %u page(s) cached (%u KB), %u of them mapped; %u hits, %u misses, "
            "%u pages read from files\n",
            pcache_enabled() ? "on" : "OFF (mem.pagecache = 0)",
            st.pages, st.pages * 4u, st.mapped, st.hits, st.misses, st.reads);
    kprintf("pcache: dropped %u (freed %u, detached from their mappings %u), %u by reclaim, "
            "%u by file changes\n",
            st.freed + st.detached, st.freed, st.detached, st.reclaimed, st.invalidated);
}
SHELL_CMD(pcache) = { "pcache", "[drop]", "the page cache: file pages shared between programs",
                      SHELL_G_MEM, cmd_pcache, SHELL_P_ANY };

/* `sharetest` (hidden) — rung 2's measurement, asked of the machine rather
 * than inferred.  Two dynamic programs (dynhold: start through ld.so, then
 * hold) are started one after the other, with the cache OFF and then ON, and
 * the free-frame count is read after each.  What the second program costs is
 * the number that must fall — by about one libc.so — and what it READS from
 * files must be zero once the first has populated the cache.  The unknown
 * constants (kernel stack, page tables, the main image the kernel copies)
 * appear in both runs and cancel. */
extern const unsigned char _binary_user_dynhold_dynelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_dynhold_dynelf_end[]   __attribute__((weak));
#include "proc.h"
#include "task.h"
static int spawn_hold(void) {
    const char* argv[2] = { "dynhold", "30" };
    return proc_spawn_argv("dynhold", _binary_user_dynhold_dynelf_start,
                           (size_t)(_binary_user_dynhold_dynelf_end - _binary_user_dynhold_dynelf_start),
                           2, argv, /*linux_abi*/1);
}
static void wait_gone(int pid) {
    for (int i = 0; i < 150; i++) {
        struct task* t = task_find(pid);
        if (!t || t->state == TASK_DEAD) return;
        task_msleep(20);
    }
}
struct share_run { int32_t a_kb, b_kb; uint32_t b_reads, b_hits; int ok; };
static void share_run(int cache_on, struct share_run* r) {
    config_set("mem.pagecache", cache_on ? "1" : "0");
    pcache_drop_all();
    task_msleep(300);
    struct pcache_stats s0, s1, s2;
    uint32_t f0 = pmm_free_frames();
    pcache_stats(&s0);
    int pa = spawn_hold();
    task_msleep(2000);
    uint32_t fa = pmm_free_frames();
    pcache_stats(&s1);
    int pb = spawn_hold();
    task_msleep(2000);
    uint32_t fb = pmm_free_frames();
    pcache_stats(&s2);
    r->ok = pa > 0 && pb > 0;
    r->a_kb = (int32_t)(f0 - fa) * 4;
    r->b_kb = (int32_t)(fa - fb) * 4;
    r->b_reads = s2.reads - s1.reads;
    r->b_hits  = s2.hits - s1.hits;
    (void)s0;
    if (pa > 0) task_kill(pa);
    if (pb > 0) task_kill(pb);
    if (pa > 0) wait_gone(pa);
    if (pb > 0) wait_gone(pb);
}
static void cmd_sharetest(const char* args) {
    (void)args;
    if (!_binary_user_dynhold_dynelf_start) {
        kprintf("sharetest: SKIP - no dynamic test program on this arch\n");
        return;
    }
    struct share_run off, on;
    share_run(0, &off);
    share_run(1, &on);
    config_set("mem.pagecache", "1");
    int saved = off.b_kb - on.b_kb;
    int ok = off.ok && on.ok && saved >= 200 && on.b_reads == 0 && on.b_hits > 0;
    kprintf("sharetest: cache OFF - first program %d KB, second %d KB\n", off.a_kb, off.b_kb);
    kprintf("sharetest: cache ON  - first program %d KB, second %d KB, second read %u page(s) "
            "from files and found %u in the cache\n", on.a_kb, on.b_kb, on.b_reads, on.b_hits);
    kprintf("sharetest: the second program costs %d KB less -> %s\n", saved, ok ? "PASS" : "FAIL");
}
SHELL_CMD(sharetest) = { "sharetest", "", 0, SHELL_G_TEST, cmd_sharetest, SHELL_P_ADMIN };

/* `pcachetest` (hidden, every arch) — the cache's own contract on the real
 * primitives: the same page asked for twice is ONE frame and ONE read; a write
 * to the file drops it; an unmapped page is reclaimable and a mapped one is
 * not. */
static void cmd_pcachetest(const char* args) {
    (void)args;
    int bad = 0;
    struct file* f = vfs_open("/tmp-pcachetest", VFS_RDWR | VFS_CREATE | VFS_TRUNC);
    if (!f) { kprintf("pcachetest: cannot create a file\n"); return; }
    uint8_t buf[64];
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)(0x40 + i);
    vfs_write(f, buf, 64);
    struct pcache_stats a, b, c;
    pcache_stats(&a);
    pmm_phys_t f1 = 0, f2 = 0, f3 = 0;
    pcache_map_page(f, 0, &f1);
    pcache_map_page(f, 0, &f2);
    pcache_stats(&b);
    if (f1 != f2) { kprintf("pcachetest: two lookups gave two frames\n"); bad++; }
    if (b.reads - a.reads != 1) { kprintf("pcachetest: %u reads for one page\n", b.reads - a.reads); bad++; }
    uint8_t* p = (uint8_t*)kmap_frame(f1);
    int same = p[0] == 0x40 && p[63] == 0x40 + 63 && p[64] == 0;
    kunmap_frame(p);
    if (!same) { kprintf("pcachetest: the cached page does not hold the file\n"); bad++; }
    /* two "mappings" hold it: not reclaimable */
    if (pcache_reclaim(1) != 0) { kprintf("pcachetest: reclaimed a MAPPED page\n"); bad++; }
    /* the file changes: the page leaves the cache (detached, the mappings keep it) */
    f->pos = 0; buf[0] = 0x7A; vfs_write(f, buf, 1);
    pcache_map_page(f, 0, &f3);
    pcache_stats(&c);
    if (f3 == f1) { kprintf("pcachetest: a write did not drop the stale page\n"); bad++; }
    p = (uint8_t*)kmap_frame(f3);
    if (p[0] != 0x7A) { kprintf("pcachetest: the re-read page is stale\n"); bad++; }
    kunmap_frame(p);
    /* drop our "mappings": f1 was detached, so its last holder frees it */
    if (vmm_frame_unshare(f1)) pmm_free_frame(f1);
    if (vmm_frame_unshare(f2)) pmm_free_frame(f2);
    if (vmm_frame_unshare(f3)) pmm_free_frame(f3);
    if (pcache_reclaim(1) != 1) { kprintf("pcachetest: an unmapped page was not reclaimable\n"); bad++; }
    vfs_close(f);
    vfs_unlink("/tmp-pcachetest");
    kprintf("pcachetest: one frame and one read for two lookups, a write drops it, only "
            "unmapped pages are reclaimed -> %s\n", bad ? "FAIL" : "PASS");
}
SHELL_CMD(pcachetest) = { "pcachetest", "", 0, SHELL_G_TEST, cmd_pcachetest, SHELL_P_ADMIN };
