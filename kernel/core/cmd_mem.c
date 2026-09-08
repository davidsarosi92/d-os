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
    const char* zone_names[NR_ZONES] = { "DMA", "DMA32", "NORMAL" };
    uint32_t order_counts[BUDDY_MAX_ORDER + 1];
    kprintf("ZONE     MANAGED  FREE-BLOCKS-PER-ORDER (0..%u)\n",
            BUDDY_MAX_ORDER);
    for (int z = 0; z < NR_ZONES; z++) {
        uint32_t managed = 0;
        pmm_zone_stats(z, order_counts, &managed);
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

static void mem_slabinfo (const char* a) { (void)a; cmd_slabinfo();  }
static void mem_buddyinfo(const char* a) { (void)a; cmd_buddyinfo(); }
static void mem_mmtest   (const char* a) { (void)a; cmd_mmtest();    }

SHELL_CMD(meminfo)   = { "meminfo",   "", "firmware map, buddy allocator and heap",
                         SHELL_G_MEM,  mem_meminfo };
SHELL_CMD(memcheck)  = { "memcheck",  "", "validate the buddy free lists now",
                         SHELL_G_MEM,  mem_memcheck };
SHELL_CMD(slabinfo)  = { "slabinfo",  "", "slab caches",
                         SHELL_G_MEM,  mem_slabinfo };
SHELL_CMD(buddyinfo) = { "buddyinfo", "", "buddy free lists by order",
                         SHELL_G_MEM,  mem_buddyinfo };
SHELL_CMD(mmtest)    = { "mmtest",    "", "allocator self-test",
                         SHELL_G_TEST, mem_mmtest };
