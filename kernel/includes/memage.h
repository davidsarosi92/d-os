/* =============================================================================
 * memage.h — the accessed-bit sweep (§M74 rung 1).  See kernel/mem/memage.c.
 * ============================================================================= */
#ifndef MEMAGE_H
#define MEMAGE_H

#include <stdint.h>

struct memage_stats {
    uint32_t sweeps;
    uint64_t hot_bytes;      /* touched since the last sweep              */
    uint64_t warm_bytes;
    uint64_t cold_bytes;     /* untouched for cold_rounds sweeps or more  */
    uint32_t cold_rounds;
    uint32_t spaces, pages;  /* what the last sweep visited               */
    uint32_t untracked;      /* frames above the tracked range            */
    uint32_t last_cost_us;
};

void memage_sweep(void);
void memage_stats(struct memage_stats* out);
int  memage_task_split(int pid, uint64_t* hot, uint64_t* warm, uint64_t* cold);

#endif
