/* =============================================================================
 * swap.h — writing a paused program's memory out, and bringing it back
 * (§M72 stage 3).  See kernel/mem/swap.c for the contract; the short form:
 * only a STOPPED user task is evicted, shared / copy-on-write / device / DMA
 * pages never are, and a task never runs with a page still out.
 * ============================================================================= */
#ifndef SWAP_H
#define SWAP_H

#include <stdint.h>

struct task;

enum { SWAP_OK = 0, SWAP_DECLINED = 1, SWAP_FAILED = 2 };

/* What one request did.  `why` is set whenever the result is not the whole
 * of what was asked — a caller that prints it tells the truth. */
struct swap_report {
    int         result;
    const char* why;
    uint32_t    evicted;        /* pages written out and freed          */
    uint32_t    restored;       /* pages read back                      */
    uint32_t    kept_shared;    /* VMM_SHARED: somebody else maps it    */
    uint32_t    kept_cow;       /* the other side of a fork shares it   */
    uint32_t    kept_device;    /* not memory the allocator manages     */
    uint32_t    kept_dma;       /* a device may write into it           */
    /* Where the time went, in µs — measured, because a slow eviction has
     * three unrelated suspects (the copy, the store's I/O, the TLB work). */
    uint32_t    copy_us, io_us, map_us;
};

/* The caller must hold the task's swap claim (task_swap_claim). */
int  swap_evict_task(struct task* t, struct swap_report* out);
int  swap_restore_task(struct task* t, struct swap_report* out);

int  swap_stop_evict(int pid, struct swap_report* rep);   /* pause + evict */
void swap_stats(uint32_t* used, uint32_t* total);
void swap_slot_release(uint32_t slot);
int  swap_audit(int verbose);

#endif
