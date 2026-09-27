/* =============================================================================
 * cmd_mem.c — memory shell commands (§M70).
 *
 * Split out of shell.c.  Each layer gets its own verb rather than one
 * `meminfo` that prints everything: the buddy allocator, the slab caches and
 * the kernel heap fail differently, and a single wall of output is a wall
 * people skim.
 * =========================================================================== */

#include "shellcmd.h"
#include "cmd_util.h"
#include "console.h"
#include "printf.h"
#include "pmm.h"
#include "vmm.h"
#include "kmalloc.h"
#include "slab.h"
#include "multiboot.h"
#include "lock.h"
#include "hal_api.h"
#include "proc.h"        /* §M75 — memhog spawns a ring-3 process */
#include "timer.h"
#include "task.h"
#include "percpu.h"
#include <stdint.h>
#include <stddef.h>

static void cmd_slabinfo(void) {
    int n = slab_cache_count();
    kprintf("NAME           OBJSZ  SLOT  SLABS  IN_USE  FREE  MAG  CACHED-EMPTY\n");
    for (int i = 0; i < n; i++) {
        struct slab_stats s;
        slab_cache_get_stats(i, &s);
        kprintf("%s  %u  %u  %u  %u  %u  %u  %u\n",
                s.name, (unsigned)s.obj_size, (unsigned)s.slot_size,
                s.slabs, s.in_use_objs, s.free_objs, s.mag_total,
                s.cached_empty);
    }
}

static void cmd_buddyinfo(void) {
    const char* zone_names[NR_ZONES] = { "DMA", "DMA32", "NORMAL", "HIGHMEM" };
    uint32_t order_counts[BUDDY_MAX_ORDER + 1];
    int nodes = pmm_node_count();
    kprintf("ZONE     MANAGED  FREE-BLOCKS-PER-ORDER (0..%u)\n",
            BUDDY_MAX_ORDER);
    /* §M19.5.3 — one line per (node, zone) on a NUMA machine, so where free
     * memory sits is visible rather than summed away. */
    for (int nd = 0; nd < nodes; nd++)
        for (int z = 0; z < NR_ZONES; z++) {
            uint32_t managed = 0;
            if (nodes > 1) pmm_node_zone_stats(nd, z, order_counts, &managed);
            else           pmm_zone_stats(z, order_counts, &managed);
            if (nodes > 1) kprintf("node %d ", nd);
            kprintf("%s ", zone_names[z]);
            kprintf("m=%u  ", managed);
            for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
                kprintf("%u ", order_counts[o]);
            }
            kprintf("\n");
        }
}

/* M25 stage 1 — per-process address space self-test.  Creates a fresh
 * vmm_space, maps one user page carrying a sentinel, switches this CPU to
 * that space, reads the page back, then switches to the kernel space and
 * proves the mapping is PRIVATE (not visible in the kernel directory).
 * IRQs are held off across the CR3 excursion so no reschedule ever runs
 * with our non-standard address space loaded (the scheduler doesn't switch
 * CR3 yet — that's the next stage-1 step). */
static void cmd_mmtest(void) {
    struct vmm_space* s = vmm_space_create();
    if (!s) { console_write("mmtest: vmm_space_create failed\n"); return; }

    pmm_phys_t frame = pmm_alloc_frame();          /* backing for the user page */
    if (!frame) { console_write("mmtest: no frame\n"); vmm_space_destroy(s); return; }

    /* Seed the sentinel through the identity map (frame < 256 MiB). */
    *(volatile uint32_t*)phys_to_virt(frame) = 0xC0FFEE42u;

    const uintptr_t UVA = vmm_user_base();        /* arch's user-region base */
    if (vmm_space_map(s, UVA, frame, VMM_WRITABLE | VMM_USER) != 0) {
        console_write("mmtest: vmm_space_map failed\n");
        pmm_free_frame(frame); vmm_space_destroy(s); return;
    }

    spinlock_t lk = SPINLOCK_INIT;
    uint32_t flags = spin_lock_irqsave(&lk);
    vmm_space_switch(s);
    uint32_t got = *(volatile uint32_t*)UVA;      /* read via the space's map */
    vmm_space_switch(NULL);                        /* back to kernel space */
    spin_unlock_irqrestore(&lk, flags);

    uintptr_t kview = vmm_translate(UVA);          /* kernel-space view of UVA */

    kprintf("mmtest: read 0x%x @ %p (want 0xc0ffee42) -> %s; "
            "kernel translate(UVA)=%p (want 0x0, private) -> %s\n",
            got, (void*)UVA, got == 0xC0FFEE42u ? "PASS" : "FAIL",
            (void*)kview, kview == 0 ? "PASS" : "FAIL");

    vmm_space_destroy(s);                          /* frees PT + user frame + PD */
}

/* --- registrations --------------------------------------------------------- */

/* The three-layer summary.  It was an inline block inside the dispatch chain,
 * which is why nothing could call it and `help` never listed it. */
static void mem_meminfo(const char* a) {
    (void)a;
    mboot_print_meminfo();
    pmm_print_stats();
    vmm_print_status();
    struct kmstat ks;
    kmalloc_stats(&ks);
    kprintf("kheap: %u/%u bytes used (%u chunks, %u free)\n",
            (unsigned)ks.used_bytes, (unsigned)ks.total_bytes,
            ks.chunk_count, ks.free_chunk_count);
}

/* Walk every buddy free list and report the first inconsistency — a diagnostic
 * for the latent large-order corruption noted in PLAN.md §M39.  "consistent"
 * means the links AND page_state agree, which is why it is not simply a count. */
static void mem_memcheck(const char* a) { (void)a; pmm_validate("memcheck"); }

/* §M19.5.3 — `numatest`: does an allocation land on the asking CPU's node?
 * Pins itself to each CPU in turn (and CHECKS it got there — affinity is a
 * request until the next schedule, §M31's hardlock lesson), allocates frames
 * and counts where they came from; then asks each node explicitly.  It can
 * fail: with the node preference removed from page_alloc every CPU gets node
 * 0's memory, and the per-CPU line says so. */
#define NUMATEST_FRAMES 256
static void mem_numatest(const char* a) {
    (void)a;
    int nodes = pmm_node_count();
    if (nodes <= 1) { kprintf("numatest: SKIP - one memory node (boot with -numa)\n"); return; }
    static pmm_phys_t fr[NUMATEST_FRAMES];
    struct task* me = task_current();
    int bad = 0;
    for (int c = 0; c < smp_ncpus(); c++) {
        task_set_affinity(me, 1u << c);
        for (int i = 0; i < 100 && this_cpu_id() != c; i++) task_yield();
        if (this_cpu_id() != c) { kprintf("numatest: could not move to CPU %d\n", c); bad++; continue; }
        int want = pmm_local_node(), local = 0, got = 0;
        for (int i = 0; i < NUMATEST_FRAMES; i++) {
            fr[i] = pmm_alloc_frame();
            if (fr[i] == PMM_ALLOC_FAIL) break;
            got++;
            if (pmm_node_of(fr[i]) == want) local++;
        }
        for (int i = 0; i < got; i++) pmm_free_frame(fr[i]);
        kprintf("numatest: CPU %d (node %d): %d/%d frames local\n", c, want, local, got);
        if (local != got || !got) bad++;
    }
    task_set_affinity(me, 0xFFFFFFFFu);
    for (int nd = 0; nd < nodes; nd++) {
        pmm_phys_t f = page_alloc_node(0, ZONE_DEFAULT, nd);
        int on = f == PMM_ALLOC_FAIL ? -1 : pmm_node_of(f);
        kprintf("numatest: asked node %d, got a frame on node %d\n", nd, on);
        if (on != nd) bad++;
        if (f != PMM_ALLOC_FAIL) pmm_free_frame(f);
    }
    pmm_validate("numatest");
    kprintf("numatest: %s\n", bad ? "FAIL" : "PASS");
}

static void mem_slabinfo (const char* a) { (void)a; cmd_slabinfo();  }
static void mem_buddyinfo(const char* a) { (void)a; cmd_buddyinfo(); }
static void mem_mmtest   (const char* a) { (void)a; cmd_mmtest();    }

SHELL_CMD(meminfo)   = { "meminfo",   "", "firmware map, buddy allocator and heap",
                         SHELL_G_MEM,  mem_meminfo, SHELL_P_ANY };
SHELL_CMD(memcheck)  = { "memcheck",  "", "validate the buddy free lists now",
                         SHELL_G_MEM,  mem_memcheck, SHELL_P_ADMIN };
SHELL_CMD(slabinfo)  = { "slabinfo",  "", "slab caches",
                         SHELL_G_MEM,  mem_slabinfo, SHELL_P_ANY };
SHELL_CMD(numatest)  = { "numatest",  "", "allocations land on the CPU's own NUMA node",
                         SHELL_G_MEM,  mem_numatest, SHELL_P_ADMIN };
SHELL_CMD(buddyinfo) = { "buddyinfo", "", "buddy free lists by order",
                         SHELL_G_MEM,  mem_buddyinfo, SHELL_P_ANY };
SHELL_CMD(mmtest)    = { "mmtest",    "", "allocator self-test",
                         SHELL_G_TEST, mem_mmtest, SHELL_P_ADMIN };

/* ---------------------------------------------------------------------------
 * §M75 — `memhog`: the falsifier for the per-process memory column.
 *
 * On a bare boot every task is a kernel thread, so the whole column reads 0.
 * That is the TRUE answer and it is also indistinguishable from a walk that
 * does not work, which is §M71's rule 3 one subsystem over.  `memhog` puts a
 * real ring-3 address space on the machine that grows by a known amount on a
 * known schedule, so two `ps` runs prove the column tracks reality by
 * SUBTRACTION — see user/memhog.c for why a difference and not an absolute.
 *
 * It lives here, next to the memory commands, because that is what it is a
 * test OF; it is spawned detached so the shell stays usable while it grows. */
extern const unsigned char _binary_user_memhog_elf_start[]         __attribute__((weak));
extern const unsigned char _binary_user_memhog_elf_end[]           __attribute__((weak));
extern const unsigned char _binary_user_memhog_x86_64_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_memhog_x86_64_elf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_memhog_aarch64_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_memhog_aarch64_elf_end[]   __attribute__((weak));

static void mem_memhog(const char* a) {
    (void)a;
    const unsigned char *s = 0, *e = 0;
    if (_binary_user_memhog_elf_start)         { s = _binary_user_memhog_elf_start;         e = _binary_user_memhog_elf_end; }
    else if (_binary_user_memhog_x86_64_elf_start)  { s = _binary_user_memhog_x86_64_elf_start;  e = _binary_user_memhog_x86_64_elf_end; }
    else if (_binary_user_memhog_aarch64_elf_start) { s = _binary_user_memhog_aarch64_elf_start; e = _binary_user_memhog_aarch64_elf_end; }
    if (!s || !e) { console_write("memhog: no ELF embedded for this arch\n"); return; }

    int pid = proc_spawn("memhog", s, (size_t)(e - s));
    if (pid < 0) { console_write("memhog: spawn failed\n"); return; }
    kprintf("memhog: pid %d — `ps` twice and subtract; `kill %d` when done\n", pid, pid);
}

SHELL_CMD(memhog)    = { "memhog",    "", "ring-3 process that grows 1 MiB/2 s (memory-column falsifier)",
                         SHELL_G_TEST, mem_memhog, SHELL_P_ADMIN };

/* ---------------------------------------------------------------------------
 * §M86 — `highmemtest`: every frame of ZONE_HIGHMEM, filled and verified.
 *
 * The acceptance test in PLAN §M86 is "a fill-and-verify pass over every
 * frame", and §M48's lesson is why: a ceiling raised without touching what is
 * above it hides the bug that matters.  So: take user frames until the
 * allocator starts handing out LOW memory (highmem exhausted), write a
 * frame-specific pattern into each through kmap, read every one back, free
 * them all, and report the counts.  A pattern that depends on the frame
 * number is what catches two frames aliasing one window slot.  The frame list
 * itself is kept IN the frames (a chain through word 1), so the test needs no
 * allocation proportional to what it tests.
 * ------------------------------------------------------------------------- */
#include "kmap.h"
/* `highmemtest all` (§M86 stage 3, 2026-09-26) tests EVERY frame the user
 * allocator will hand out, not just the HIGHMEM zone — which is the question
 * on a machine whose RAM is all directly mapped (x86_64, and aarch64 since its
 * TTBR1 direct map): there the frames above 4 GiB are ordinary frames, and
 * whether the direct map really reaches them is exactly what needs proving.
 * It leaves a reserve (ALL_RESERVE frames) so the rest of the machine can
 * still allocate while the test holds everything else. */
#define ALL_RESERVE 4096u                          /* 16 MiB */
static void cmd_highmemtest(const char* args) {
    int all = args && args[0] == 'a';
    uint32_t mgr = 0;
    pmm_zone_stats(ZONE_HIGHMEM, NULL, &mgr);
    if (all) mgr = pmm_free_frames();
    if (!mgr) { kprintf("highmemtest: no HIGHMEM on this machine (all RAM directly mapped) - try `highmemtest all`\n"); return; }
    if (all && mgr <= ALL_RESERVE) { kprintf("highmemtest: too little free memory\n"); return; }
    if (all) mgr -= ALL_RESERVE;
    pmm_phys_t top = 0;
    uint64_t t0 = timer_ticks_ms();
    pmm_phys_t head = 0, low = 0;
    uint32_t n = 0;
    for (;;) {
        pmm_phys_t f = pmm_alloc_frame_user();
        if (!f) break;
        /* Asked, not inferred: comparing the kmap address with the frame's
         * number looks like "is it direct?" and is wrong exactly once — the
         * first highmem frame, 0x3FC00000, IS the address of CPU 0's first
         * window slot.  That coincidence stopped the first version at 2016
         * frames while 525 280 were free. */
        if (!all && !pmm_frame_is_highmem(f)) { low = f; break; }  /* highmem is exhausted */
        if (all && n >= mgr) { low = f; break; }            /* keep the reserve  */
        if (f > top) top = f;
        uint32_t* p = (uint32_t*)kmap_frame(f);
        if (!p) { pmm_free_frame(f); break; }
        uint32_t pat = (uint32_t)(f >> 12) * 2654435761u;
        for (int i = 2; i < 1024; i++) p[i] = pat ^ (uint32_t)i;
        p[0] = 0xC0DEF00Du;
        /* The chain holds the previous FRAME NUMBER, not its address: with
         * PAE a frame lives above 4 GiB and a 32-bit address word truncates
         * it — the first version walked into low memory after the first
         * such frame, "verified" 520 196 of 1 836 000 and freed frames it
         * had never allocated.  A pfn fits 32 bits up to 16 TiB. */
        p[1] = (uint32_t)(head >> 12);            /* chain: previous frame */
        kunmap_frame(p);
        head = f;
        n++;
        if ((n & 4095) == 0) task_yield();   /* seconds of work: offer the CPU */
    }
    if (low) pmm_free_frame(low);
    uint32_t bad = 0, checked = 0;
    while (head) {
        uint32_t* p = (uint32_t*)kmap_frame(head);
        uint32_t pat = (uint32_t)(head >> 12) * 2654435761u;
        int ok = p[0] == 0xC0DEF00Du;
        for (int i = 2; ok && i < 1024; i++) if (p[i] != (pat ^ (uint32_t)i)) ok = 0;
        pmm_phys_t next = (pmm_phys_t)p[1] << 12;
        kunmap_frame(p);
        if (!ok) bad++;
        checked++;
        pmm_free_frame(head);
        head = next;
        if ((checked & 4095) == 0) task_yield();
    }
    kprintf("highmemtest: %u of %u %s frames (%u MiB, highest at %u MiB) filled, %u verified, %u bad, "
            "in %u ms: %s\n", n, mgr, all ? "free" : "HIGHMEM", (n * 4) / 1024,
            (unsigned)(top >> 20), checked, bad,
            (unsigned)(timer_ticks_ms() - t0),
            (bad == 0 && checked == n && n > 0) ? "PASS" : "FAIL");
}
SHELL_CMD(highmemtest) = { "highmemtest", "[all]", "fill and verify every HIGHMEM frame (kmap)",
                           SHELL_G_TEST, cmd_highmemtest, SHELL_P_ADMIN };
