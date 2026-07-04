/* =============================================================================
 * clipboard.c — kernel-global text clipboard (M22.5).  See clipboard.h.
 *
 * Storage is a kmalloc'd buffer that grows to the largest text ever
 * copied and is then reused (copy/paste churn shouldn't hammer the
 * allocator).  Content is capped at CLIP_MAX so a runaway "select all
 * + copy" in a giant buffer can't eat the heap.
 * ============================================================================= */

#include "clipboard.h"
#include "lock.h"
#include "kmalloc.h"
#include <stddef.h>

#define CLIP_MAX (64 * 1024)

static spinlock_t clip_lock;            /* zero-initialized = unlocked */
static char* clip_buf = NULL;
static int   clip_cap = 0;
static int   clip_len = 0;

int clipboard_set(const char* text, int len) {
    if (!text) len = 0;
    if (len < 0) { len = 0; while (text[len]) len++; }
    if (len > CLIP_MAX) len = CLIP_MAX;

    /* Allocate outside the lock — kmalloc may take its own locks. */
    char* nbuf = NULL;
    if (len > 0 && len > clip_cap) {
        nbuf = (char*)kmalloc((size_t)len);
        if (!nbuf) return -1;
    }

    char* old = NULL;
    uint32_t fl = spin_lock_irqsave(&clip_lock);
    if (nbuf) {
        old = clip_buf;
        clip_buf = nbuf;
        clip_cap = len;
    }
    if (len > clip_cap) len = clip_cap;         /* paranoid re-clamp */
    for (int i = 0; i < len; i++) clip_buf[i] = text[i];
    clip_len = len;
    spin_unlock_irqrestore(&clip_lock, fl);
    if (old) kfree(old);                        /* free outside the lock */
    return 0;
}

int clipboard_get(char* dst, int cap) {
    if (!dst || cap <= 0) return 0;
    uint32_t fl = spin_lock_irqsave(&clip_lock);
    int n = clip_len < cap - 1 ? clip_len : cap - 1;
    for (int i = 0; i < n; i++) dst[i] = clip_buf[i];
    spin_unlock_irqrestore(&clip_lock, fl);
    dst[n] = 0;
    return n;
}

int clipboard_len(void) { return clip_len; }
