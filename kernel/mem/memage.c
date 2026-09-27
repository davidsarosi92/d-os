/* =============================================================================
 * memage.c — how much of the resident memory is actually being USED
 * (§M74 rung 1, 2026-09-27).
 *
 * Nothing in this tree read the hardware's own usage data: x86's PTE Accessed
 * bit and ARM's Access Flag existed and were referenced nowhere.  Every later
 * rung of §M74 — which page to reclaim — needs exactly that answer, and a
 * reclaim without it is random eviction, which is worse than none: it picks
 * the page a loop is about to touch as readily as one nobody has read since
 * boot.  So this rung ships ALONE, as instrumentation, and has to be shown to
 * respond to usage before anything is allowed to act on it.
 *
 * THE SWEEP.  Every `mem.age_ms` (default 1000; 0 = off) a service walks every
 * user address space once (vmm_space_age: report each present page with its
 * accessed bit, and clear the bit atomically), then does ONE full TLB flush,
 * outside the task-list lock (vmm_flags.h says why).  Each frame carries an AGE:
 * sweeps since it was last seen accessed, saturating at 255.
 *
 * WHY PER FRAME AND NOT PER MAPPING.  A frame mapped by two spaces (a COW page
 * after fork, a shared memfd) is used if EITHER touches it.  So a frame's age
 * is updated once per round, on its first visit, and any later visit in the
 * same round that finds the bit set brings it back to 0.
 *
 * A FRAME SEEN THIS ROUND BUT NOT LAST ROUND IS NEW (age 0): frames are freed
 * and reallocated, and a new page inheriting an old frame's age would look
 * cold on its first sweep.  The round marker is 8 bits, so a frame unmapped for
 * exactly 256 rounds and then mapped again keeps a stale age for one round —
 * stated rather than paid for with a wider array.
 *
 * WHAT IT REPORTS: resident user memory split HOT (touched since the last
 * sweep), WARM, and COLD (untouched for `mem.cold_ms`, default 30 s), plus the
 * sweep's own cost — a sampler whose cost is unmeasured is one nobody can
 * defend when the machine gets slower.
 *
 * ON aarch64 a cleared Access Flag makes the next access FAULT (no hardware
 * flag management is used); vmm_af_fault sets it again.  So a sweep there
 * costs one fault per page that is actually in use, per interval — measured
 * by the report's fault counter, not assumed small.
 * ============================================================================= */

#include "memage.h"
#include "vmm.h"
#include "pmm.h"
#include "task.h"
#include "kmalloc.h"
#include "printf.h"
#include "config.h"
#include "settings.h"
#include "service.h"
#include "shellcmd.h"
#include "timer.h"
#include "lock.h"
#include "proc.h"
#include "pcache.h"
#include "swap.h"
#include <stdint.h>
#include <stddef.h>

#define MEMAGE_MAX_FRAMES  (16u * 1024u * 1024u)   /* 64 GiB tracked; above: counted as untracked */

static uint8_t*  g_age;       /* per frame: sweeps since last seen accessed        */
static uint8_t*  g_round;     /* per frame: the round it was last visited (mod 256) */
static uint32_t  g_n;         /* frames tracked                                    */
static uint8_t   g_cur;       /* the current round                                 */
static int       g_alloc_failed;

static struct memage_stats g_st;
static spinlock_t g_st_lock = SPINLOCK_INIT;

static int cfg_ms(const char* key, int def) {
    const char* v = config_get(key, 0);
    if (!v || !*v) return def;
    int n = 0;
    for (; *v >= '0' && *v <= '9'; v++) n = n * 10 + (*v - '0');
    return n;
}
static uint32_t cold_rounds(void) {
    int age = cfg_ms("mem.age_ms", 1000), cold = cfg_ms("mem.cold_ms", 30000);
    if (age <= 0) age = 1000;
    uint32_t r = (uint32_t)(cold / age);
    if (r < 2) r = 2;
    if (r > 250) r = 250;
    return r;
}

static int arrays_ready(void) {
    if (g_age) return 1;
    if (g_alloc_failed) return 0;
    uint32_t n = pmm_nr_frames;
    if (n > MEMAGE_MAX_FRAMES) n = MEMAGE_MAX_FRAMES;
    uint8_t* a = (uint8_t*)kcalloc(n, 1);
    uint8_t* r = (uint8_t*)kcalloc(n, 1);
    if (!a || !r) {
        if (a) kfree(a);
        if (r) kfree(r);
        g_alloc_failed = 1;
        kprintf("memage: no memory for %u frame ages - the sweep is off\n", n);
        return 0;
    }
    g_n = n; g_round = r; g_age = a;
    return 1;
}

struct sweep_ctx { uint32_t spaces, pages, untracked; };

static void visit(void* c, uintptr_t va, uint64_t phys, uint32_t flags) {
    (void)va;
    struct sweep_ctx* x = (struct sweep_ctx*)c;
    if (!pmm_frame_is_managed((pmm_phys_t)phys)) return;   /* a device window */
    uint64_t pfn = phys >> 12;
    if (pfn < pmm_pfn_base || pfn - pmm_pfn_base >= g_n) { x->untracked++; return; }
    uint32_t i = (uint32_t)(pfn - pmm_pfn_base);
    int touched = (flags & VMM_ACCESSED) != 0;
    if (g_round[i] != g_cur) {                         /* first visit this round */
        uint8_t prev = g_round[i];
        g_round[i] = g_cur;
        x->pages++;
        if (touched || prev != (uint8_t)(g_cur - 1)) g_age[i] = 0;   /* used, or new */
        else if (g_age[i] < 255) g_age[i]++;
    } else if (touched) {
        g_age[i] = 0;                                  /* another mapping used it */
    }
}

static void sweep_task(const struct task* t, int cur, void* c) {
    (void)cur;
    struct sweep_ctx* x = (struct sweep_ctx*)c;
    if (!t->user_task || !t->mm || t->mm_shared || t->state == TASK_DEAD) return;
    x->spaces++;
    vmm_space_age(t->mm, visit, x);
}

/* One round.  Callable from the service and from the test (which must not wait
 * a whole interval to see the effect of what it did). */
void memage_sweep(void) {
    if (!arrays_ready()) return;
    uint64_t t0 = timer_now_ns();
    struct sweep_ctx x = { 0, 0, 0 };
    g_cur++;
    task_for_each(sweep_task, &x);
    vmm_age_flush();                                   /* outside the lock */

    uint32_t cr = cold_rounds();
    uint64_t hot = 0, warm = 0, cold = 0;
    for (uint32_t i = 0; i < g_n; i++) {
        if (g_round[i] != g_cur) continue;
        if (g_age[i] == 0)        hot  += 4096;
        else if (g_age[i] < cr)   warm += 4096;
        else                      cold += 4096;
    }
    uint64_t t1 = timer_now_ns();

    uint32_t fl = spin_lock_irqsave(&g_st_lock);
    g_st.sweeps++;
    g_st.hot_bytes = hot; g_st.warm_bytes = warm; g_st.cold_bytes = cold;
    g_st.spaces = x.spaces; g_st.pages = x.pages; g_st.untracked = x.untracked;
    g_st.last_cost_us = (uint32_t)((t1 - t0) / 1000u);
    g_st.cold_rounds = cr;
    spin_unlock_irqrestore(&g_st_lock, fl);
}

/* §M74 rung 3 — a frame's age in sweeps since last seen used, or -1 when the
 * sweep has no opinion (not tracked, or not seen this round). */
int memage_frame_age(uint64_t phys) {
    if (!g_age) return -1;
    uint64_t pfn = phys >> 12;
    if (pfn < pmm_pfn_base || pfn - pmm_pfn_base >= g_n) return -1;
    uint32_t i = (uint32_t)(pfn - pmm_pfn_base);
    if (g_round[i] != g_cur) return -1;
    return g_age[i];
}
uint32_t memage_cold_rounds(void) { return cold_rounds(); }

void memage_stats(struct memage_stats* out) {
    uint32_t fl = spin_lock_irqsave(&g_st_lock);
    *out = g_st;
    spin_unlock_irqrestore(&g_st_lock, fl);
}

/* One space's resident set by age, as of the last sweep — the per-process view
 * the test needs, read without clearing anything. */
struct split_ctx { uint64_t hot, warm, cold, unseen; uint32_t cr; };
static void split_visit(void* c, uintptr_t va, uint64_t phys, uint32_t flags) {
    (void)va; (void)flags;
    struct split_ctx* x = (struct split_ctx*)c;
    if (!pmm_frame_is_managed((pmm_phys_t)phys)) return;
    uint64_t pfn = phys >> 12;
    if (!g_age || pfn < pmm_pfn_base || pfn - pmm_pfn_base >= g_n) { x->unseen += 4096; return; }
    uint32_t i = (uint32_t)(pfn - pmm_pfn_base);
    if (g_round[i] != g_cur)     x->unseen += 4096;       /* mapped since the sweep */
    else if (g_age[i] == 0)      x->hot  += 4096;
    else if (g_age[i] < x->cr)   x->warm += 4096;
    else                         x->cold += 4096;
}
struct split_find { int pid; struct split_ctx* x; int found; };
static void split_task(const struct task* t, int cur, void* c) {
    (void)cur;
    struct split_find* f = (struct split_find*)c;
    if (t->pid != f->pid || !t->mm || t->mm_shared) return;
    f->found = 1;
    vmm_space_walk(t->mm, split_visit, f->x);
}
int memage_task_split(int pid, uint64_t* hot, uint64_t* warm, uint64_t* cold) {
    struct split_ctx x = { 0, 0, 0, 0, cold_rounds() };
    struct split_find f = { pid, &x, 0 };
    task_for_each(split_task, &f);            /* under the lock: the space stays */
    if (!f.found) return -1;
    *hot = x.hot + x.unseen; *warm = x.warm; *cold = x.cold;
    return 0;
}

/* ---- the service ----------------------------------------------------------- */

/* §M74 rung 2 — reclaim under pressure, the rung that works with swap off and
 * with no disk at all: when free memory has fallen to within twice the reserve,
 * unmapped page-cache pages go back (they are copies of files; nothing is
 * lost).  Announced once per crossing. */
static void pressure_reclaim(void) {
    uint32_t rkb = 0; int low = 0;
    pmm_reserve_stats(&rkb, NULL, &low);
    uint32_t water = (rkb / 4u) * 2u + 256u;           /* frames: 2x reserve + 1 MiB */
    static int told;
    int pressure = pmm_free_frames() < water;
    /* RULE 2 — the watermark reads PHYSICAL free frames only; swap never
     * counts as free memory, or the line would not fire until the swap area
     * itself was full (and the machine already thrashing). */
    if (!pressure) {
        told = 0;
        swap_pressure(0);                        /* normal policy: cold pages only */
        return;
    }
    /* The order of sacrifice (§M74): clean file pages first — nothing is
     * lost — then idle anonymous pages if swap is on, then the reserve
     * refuses. */
    uint32_t n = pcache_reclaim(256);
    uint32_t s = 0;
    /* Under real pressure one batch is 256 KB — repeat while it helps. */
    for (int k = 0; k < 32 && pmm_free_frames() < water; k++) {
        uint32_t got = swap_pressure(1);
        s += got;
        if (!got) break;
    }
    if ((n || s) && !told) {
        told = 1;
        kprintf("memage: memory low - gave back %u cached file page(s), wrote out %u idle "
                "page(s)\n", n, s);
    }
}

static void memage_main(void) {
    for (;;) {
        int ms = cfg_ms("mem.age_ms", 1000);
        if (ms <= 0) { task_msleep(1000); pressure_reclaim(); continue; }
        if (ms < 50) ms = 50;
        task_msleep((uint32_t)ms);
        memage_sweep();
        pressure_reclaim();
    }
}
SERVICE("memage", memage_main, /*autostart*/1, SVC_RESTART_ALWAYS);

CONFIG_KEY(ck_age_ms) = {
    .key = "mem.age_ms", .group = "System", .type = CFG_INT, .min = 0, .max = 60000,
    .def = "1000",
    .help = "how often the accessed-bit sweep runs, in ms (0 = off)",
};
CONFIG_KEY(ck_cold_ms) = {
    .key = "mem.cold_ms", .group = "System", .type = CFG_INT, .min = 100, .max = 3600000,
    .def = "30000",
    .help = "memory untouched for this long counts as cold",
};

/* ---- commands --------------------------------------------------------------- */

static void cmd_memage(const char* args) {
    (void)args;
    struct memage_stats st;
    memage_stats(&st);
    if (!st.sweeps) {
        kprintf("memage: no sweep yet (mem.age_ms = %d)\n", cfg_ms("mem.age_ms", 1000));
        return;
    }
    kprintf("memage: resident user memory %u KB - hot %u KB, warm %u KB, cold %u KB "
            "(cold = untouched for %u sweeps)\n",
            (unsigned)((st.hot_bytes + st.warm_bytes + st.cold_bytes) >> 10),
            (unsigned)(st.hot_bytes >> 10), (unsigned)(st.warm_bytes >> 10),
            (unsigned)(st.cold_bytes >> 10), st.cold_rounds);
    kprintf("memage: %u sweeps, last over %u spaces / %u pages in %u us; "
            "access-flag faults caused so far %u%s\n",
            st.sweeps, st.spaces, st.pages, st.last_cost_us,
            (unsigned)vmm_af_fault_count(),
            st.untracked ? " (some frames above the tracked range)" : "");
}
SHELL_CMD(memage) = { "memage", "", "how much resident memory is actually being used",
                      SHELL_G_MEM, cmd_memage, SHELL_P_ANY };

/* `agetest` (hidden) — the falsifier.  memhog's `age` mode keeps touching the
 * first 25 % of 8 MiB, then switches to 75 %.  With the sweep fast (200 ms,
 * cold after 1 s) the process's split must follow: about a quarter hot and the
 * rest cold, then about three quarters hot.  A metric that does not move with
 * usage is a constant with a label. */
extern const unsigned char _binary_user_memhog_elf_start[]         __attribute__((weak));
extern const unsigned char _binary_user_memhog_elf_end[]           __attribute__((weak));
extern const unsigned char _binary_user_memhog_x86_64_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_memhog_x86_64_elf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_memhog_aarch64_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_memhog_aarch64_elf_end[]   __attribute__((weak));
static void cmd_agetest(const char* args) {
    (void)args;
    const unsigned char *s = 0, *e = 0;
    if (_binary_user_memhog_elf_start)             { s = _binary_user_memhog_elf_start;         e = _binary_user_memhog_elf_end; }
    else if (_binary_user_memhog_x86_64_elf_start) { s = _binary_user_memhog_x86_64_elf_start;  e = _binary_user_memhog_x86_64_elf_end; }
    else if (_binary_user_memhog_aarch64_elf_start){ s = _binary_user_memhog_aarch64_elf_start; e = _binary_user_memhog_aarch64_elf_end; }
    if (!s) { kprintf("agetest: no memhog embedded\n"); return; }
    config_set("mem.age_ms", "200");
    config_set("mem.cold_ms", "1000");
    const char* argv[2] = { "memhog", "age" };
    int pid = proc_spawn_argv("age-prog", s, (size_t)(e - s), 2, argv, 0);
    if (pid < 0) { kprintf("agetest: spawn failed\n"); return; }
    uint64_t h1 = 0, w1 = 0, c1 = 0, h2 = 0, w2 = 0, c2 = 0;
    task_msleep(4000);                        /* phase 1: 25 % touched */
    memage_sweep();
    int r1 = memage_task_split(pid, &h1, &w1, &c1);
    task_msleep(5000);                        /* phase 2 starts at ~6 s: 75 % */
    memage_sweep();
    int r2 = memage_task_split(pid, &h2, &w2, &c2);
    task_kill(pid);
    config_set("mem.age_ms", "1000");
    config_set("mem.cold_ms", "30000");
    struct memage_stats st;
    memage_stats(&st);
    /* DIFFERENTIAL, as §M75's memory column: the program also holds pages it
     * never touches again (libc's heap, its image), an unknown constant — so
     * the claim is about what MOVES.  In use (hot + warm: touched within the
     * cold window) must be ~2 MiB, then ~6 MiB, and cold must fall by ~4 MiB.
     * Hot alone is not the measure: a sweep landing just after the previous
     * one finds the touched quarter WARM, which is correct. */
    uint32_t use1 = (uint32_t)((h1 + w1) >> 10), use2 = (uint32_t)((h2 + w2) >> 10);
    int32_t  fell = (int32_t)((c1 >> 10) - (c2 >> 10));
    int ok = r1 == 0 && r2 == 0 &&
             use1 >= 1900 && use1 <= 2600 && use2 >= 5900 && use2 <= 6700 &&
             fell >= 3800 && fell <= 4400;
    kprintf("agetest: 25%% phase hot %u KB warm %u KB cold %u KB; 75%% phase hot %u KB "
            "warm %u KB cold %u KB; in use %u -> %u KB, cold fell %d KB; sweep %u us "
            "over %u pages -> %s\n",
            (unsigned)(h1 >> 10), (unsigned)(w1 >> 10), (unsigned)(c1 >> 10),
            (unsigned)(h2 >> 10), (unsigned)(w2 >> 10), (unsigned)(c2 >> 10),
            use1, use2, (int)fell, st.last_cost_us, st.pages, ok ? "PASS" : "FAIL");
    kprintf("agetest: access-flag faults so far %u (0 on x86: the hardware sets the bit)\n",
            (unsigned)vmm_af_fault_count());
}
SHELL_CMD(agetest) = { "agetest", "", 0, SHELL_G_TEST, cmd_agetest, SHELL_P_ADMIN };
