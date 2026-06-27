/* =============================================================================
 * pmm.c — bitmap-based Physical Memory Manager.
 *
 * The allocator is deliberately simple: one bit per 4 KiB frame in a
 * statically-allocated bitmap that covers the full 32-bit address space
 * (1 Mi frames → 128 KiB of bitmap in .bss).  Sizing it for the whole
 * address space means we never need to reason about "managed range" vs
 * "bitmap range" at lookup time; any frame index between 0 and 2^20-1 is
 * addressable.
 *
 * Bitmap convention: bit = 1 means the frame is in use (or doesn't exist
 * as usable memory), bit = 0 means the frame is free.  We start with
 * every bit set to 1, then walk the multiboot memory map clearing bits
 * for frames inside AVAILABLE regions.  Finally we re-mark a few
 * protected ranges: frame 0 (NULL safety), everything below 1 MiB (BIOS,
 * VGA, EBDA, option ROMs), the kernel image [kernel_start, kernel_end),
 * and a generous region around the multiboot info structure itself.
 *
 * Allocation is a linear scan for the first 0 bit; freeing is a direct
 * bit clear.  This is O(N/32) worst case on allocation but in practice
 * the pointer walks very quickly because most of the bitmap is set.
 * A "next free hint" can shave average-case time later if it matters.
 * ============================================================================= */

#include "pmm.h"
#include "multiboot.h"
#include "printf.h"
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Sizing.                                                                    */
/* -------------------------------------------------------------------------- */

#define MAX_FRAMES   (1u << 20)                     /* 4 GiB / 4 KiB */
#define WORD_BITS    32
#define BITMAP_WORDS (MAX_FRAMES / WORD_BITS)       /* 32768 words = 128 KiB */

/* The bitmap lives in .bss so no disk footprint; zeroed by the loader. */
static uint32_t bitmap[BITMAP_WORDS];

/* Counters.  `managed` is bumped every time we first mark a frame free
 * during init; `free` moves up and down with free / alloc operations.
 * `used = managed - free` is computed on demand. */
static uint32_t managed_frames = 0;
static uint32_t free_frames_cnt = 0;

/* Symbols from linker.ld marking the kernel image bounds. */
extern uint8_t kernel_start[];
extern uint8_t kernel_end[];

/* -------------------------------------------------------------------------- */
/* Bit helpers.  Inlined by the compiler in non-debug builds.                 */
/* -------------------------------------------------------------------------- */

static inline int bit_is_set(uint32_t idx) {
    return (bitmap[idx / WORD_BITS] >> (idx % WORD_BITS)) & 1u;
}

static inline void bit_set(uint32_t idx) {
    bitmap[idx / WORD_BITS] |= (1u << (idx % WORD_BITS));
}

static inline void bit_clear(uint32_t idx) {
    bitmap[idx / WORD_BITS] &= ~(1u << (idx % WORD_BITS));
}

/* Frame-level "mark free" / "mark used" with counter bookkeeping.  Never
 * double-counts: a mark-free on an already-free frame is a no-op, same
 * for mark-used on an already-used frame. */
static void frame_mark_free(uint32_t idx) {
    if (idx >= MAX_FRAMES) return;
    if (bit_is_set(idx)) {
        bit_clear(idx);
        managed_frames++;
        free_frames_cnt++;
    }
}

static void frame_mark_used(uint32_t idx) {
    if (idx >= MAX_FRAMES) return;
    if (!bit_is_set(idx)) {
        bit_set(idx);
        free_frames_cnt--;                          /* total managed stays the same */
    }
}

/* Mark every frame in a byte range as used.  `start` rounds down, `end`
 * rounds up — when in doubt we err on the side of wasting a frame rather
 * than handing one out that partially overlaps a protected region. */
static void mark_range_used(uint32_t start, uint32_t end) {
    uint32_t s = start / PMM_FRAME_SIZE;
    uint32_t e = (end + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    for (uint32_t i = s; i < e; i++) frame_mark_used(i);
}

/* -------------------------------------------------------------------------- */
/* Init.                                                                      */
/* -------------------------------------------------------------------------- */

void pmm_init(void) {
    /* Everything starts "used" — we then clear bits for memory the BIOS
     * told us is AVAILABLE. */
    for (uint32_t i = 0; i < BITMAP_WORDS; i++) bitmap[i] = 0xFFFFFFFFu;

    const struct mboot_info* mbi = mboot_get_info();
    if (!mbi || (mbi->flags & MBI_FLAG_MMAP) == 0 || mbi->mmap_length == 0) {
        kprintf("pmm: no memory map — PMM disabled\n");
        return;
    }

    /* First pass: clear bits for every AVAILABLE frame. */
    uintptr_t p   = mbi->mmap_addr;
    uintptr_t end = mbi->mmap_addr + mbi->mmap_length;
    int entry_budget = 64;                          /* defensive iteration cap */
    while (p < end && entry_budget-- > 0) {
        const struct mboot_mmap_entry* e = (const struct mboot_mmap_entry*)p;
        if (e->type == MMAP_TYPE_AVAILABLE) {
            uint64_t base = e->base;
            uint64_t len  = e->length;

            /* We're a 32-bit kernel — clamp anything above 4 GiB so our
             * frame index stays within MAX_FRAMES. */
            if (base >= 0x100000000ull) {
                p += e->size + 4;
                continue;
            }
            if (base + len > 0x100000000ull) len = 0x100000000ull - base;

            /* Round base UP, end DOWN.  A partial frame that straddles
             * an AVAILABLE boundary could cover non-memory; skipping it
             * is safer than handing it out. */
            uint32_t first = (uint32_t)((base + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
            uint32_t last  = (uint32_t)((base + len) / PMM_FRAME_SIZE);
            for (uint32_t i = first; i < last; i++) frame_mark_free(i);
        }
        p += e->size + 4;
    }

    /* Second pass: re-reserve regions that the mmap might have lumped
     * into AVAILABLE but that we don't want the allocator touching. */

    /* (a) Frame 0.  Prevents an accidental NULL-looking allocation and
     *     reserves the real-mode IVT / BDA area underneath. */
    frame_mark_used(0);

    /* (b) Everything below 1 MiB.  Modern PCs treat low memory as a
     *     minefield (VGA buffer at 0xA0000, BIOS shadow at 0xE0000,
     *     legacy DMA buffers, ...).  Our kernel lives at 1 MiB anyway. */
    mark_range_used(0, 0x100000);

    /* (c) The kernel image.  Symbols come from linker.ld. */
    mark_range_used((uint32_t)(uintptr_t)kernel_start,
                    (uint32_t)(uintptr_t)kernel_end);

    /* (d) The multiboot info + its attached memory map.  The info
     *     struct itself is small, but `mmap_addr` can sit outside the
     *     struct, so reserve both separately. */
    mark_range_used((uint32_t)(uintptr_t)mbi,
                    (uint32_t)(uintptr_t)mbi + sizeof(struct mboot_info));
    mark_range_used(mbi->mmap_addr, mbi->mmap_addr + mbi->mmap_length);

    kprintf("pmm: %u frames managed, %u free (%u MiB total, %u MiB free)\n",
            managed_frames, free_frames_cnt,
            (managed_frames * 4) / 1024,
            (free_frames_cnt * 4) / 1024);
}

/* -------------------------------------------------------------------------- */
/* Alloc / free.                                                              */
/* -------------------------------------------------------------------------- */

uint32_t pmm_alloc_frame(void) {
    if (free_frames_cnt == 0) return PMM_ALLOC_FAIL;

    for (uint32_t w = 0; w < BITMAP_WORDS; w++) {
        uint32_t word = bitmap[w];
        if (word == 0xFFFFFFFFu) continue;          /* fully used — skip fast */

        /* Find the lowest zero bit.  Could use __builtin_ctz on ~word for
         * speed; a manual scan is fine for today's working set. */
        for (int b = 0; b < WORD_BITS; b++) {
            if ((word & (1u << b)) == 0) {
                uint32_t idx = w * WORD_BITS + b;
                bitmap[w] |= (1u << b);
                free_frames_cnt--;
                return idx * PMM_FRAME_SIZE;
            }
        }
    }

    /* Reached if the free counter is stale — shouldn't happen, but fall
     * through cleanly rather than loop. */
    return PMM_ALLOC_FAIL;
}

uint32_t pmm_alloc_contiguous(uint32_t n) {
    if (n == 0) return PMM_ALLOC_FAIL;
    if (n == 1) return pmm_alloc_frame();
    if (free_frames_cnt < n) return PMM_ALLOC_FAIL;

    /* Linear scan for n consecutive 0 bits.  Start at frame 256 (1 MiB)
     * to skip low memory deliberately; below that lies BIOS + VGA we
     * never want to hand out anyway. */
    uint32_t run_start = 0;
    uint32_t run_len   = 0;
    for (uint32_t i = 256; i < MAX_FRAMES; i++) {
        if (bit_is_set(i)) {
            run_len = 0;
            continue;
        }
        if (run_len == 0) run_start = i;
        run_len++;
        if (run_len == n) {
            /* Claim them all. */
            for (uint32_t j = 0; j < n; j++) {
                bit_set(run_start + j);
                free_frames_cnt--;
            }
            return run_start * PMM_FRAME_SIZE;
        }
    }
    return PMM_ALLOC_FAIL;
}

void pmm_free_frame(uint32_t addr) {
    if (addr == 0) return;                          /* NULL never allocated */
    if (addr & (PMM_FRAME_SIZE - 1)) return;        /* misaligned — caller bug */

    uint32_t idx = addr / PMM_FRAME_SIZE;
    if (idx >= MAX_FRAMES) return;

    if (bit_is_set(idx)) {
        bit_clear(idx);
        free_frames_cnt++;
    }
}

/* -------------------------------------------------------------------------- */
/* Queries.                                                                   */
/* -------------------------------------------------------------------------- */

uint32_t pmm_managed_frames(void) { return managed_frames; }
uint32_t pmm_free_frames(void)    { return free_frames_cnt; }
uint32_t pmm_used_frames(void)    { return managed_frames - free_frames_cnt; }

void pmm_print_stats(void) {
    kprintf("pmm: managed=%u free=%u used=%u (%u/%u MiB free)\n",
            managed_frames, free_frames_cnt, managed_frames - free_frames_cnt,
            (free_frames_cnt * 4) / 1024, (managed_frames * 4) / 1024);
}
