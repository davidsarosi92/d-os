/* =============================================================================
 * pcache.h — the page cache (§M74 rung 2).  See kernel/mem/pcache.c.
 * ============================================================================= */
#ifndef PCACHE_H
#define PCACHE_H

#include <stdint.h>
#include <stddef.h>
#include "pmm.h"

struct file;
struct inode;

struct pcache_stats {
    uint32_t pages, mapped;
    uint32_t hits, misses, reads;
    uint32_t freed, detached, reclaimed, invalidated;
    uint32_t updated;           /* §M90 — pages refreshed in place by a write */
};

int      pcache_enabled(void);
/* The frame holding page `idx` of `f`, read on first use, with ONE share taken
 * for the caller's mapping (map it VMM_COW; on failure vmm_frame_unshare it). */
int      pcache_map_page(struct file* f, uint64_t idx, pmm_phys_t* out);
void     pcache_invalidate(struct inode* ino);   /* the file went away / was truncated */
/* §M90 — `n` bytes at `off` were written to the file: copy them into its
 * cached pages so every mapping sees them (buf is kernel memory). */
void     pcache_update(struct inode* ino, uint64_t off, const void* buf, size_t n);
void     pcache_drop_all(void);                  /* a filesystem went away        */
uint32_t pcache_reclaim(uint32_t max);           /* unmapped pages, oldest first  */
uint32_t pcache_reclaim_locked(uint32_t max);
void     pcache_stats(struct pcache_stats* out);

#endif
