/* =============================================================================
 * usock.c — anonymous unix-domain socket pairs + fd passing (M25 stage 5).
 *
 * A `socketpair` is two connected endpoints (`struct usock`).  Bytes written
 * to one endpoint land in the PEER's receive ring; reads drain the endpoint's
 * own ring.  Alongside the byte stream each endpoint carries a small queue of
 * *passed file descriptors* — the SCM_RIGHTS mechanism Wayland uses to hand a
 * shm buffer or a keymap from one process to another: a sender queues a NEW
 * reference to an `ofile` on the peer, and the receiver installs it into its
 * own fd table.  Because the reference travels, the underlying object (a shm
 * frame set, a file) outlives the sender's descriptor.
 *
 * This is the connected-pair core; named sockets (bind/listen/connect on a
 * path) build on it later.  Tier A.3 adds blocking semantics: an empty
 * blocking recv parks the caller on the endpoint's read wait-queue until a
 * peer send (or the peer closing) wakes it.  That per-endpoint waitq ALSO
 * serialises the receive ring, so two tasks (producer + consumer) may now
 * hit the same pair concurrently — the pre-Tier-A code had no locking because
 * only one task ever touched a socket at a time.
 * ============================================================================= */

#include "fd.h"
#include "kmalloc.h"
#include "waitq.h"
#include <stddef.h>
#include <stdint.h>

#define USOCK_BUF   4096        /* per-endpoint receive ring */
#define USOCK_FDQ   8           /* max queued passed-fds per endpoint */

struct usock {
    struct usock* peer;                 /* other endpoint, NULL once it closes */
    uint8_t  rx[USOCK_BUF];             /* bytes the peer wrote, waiting to read */
    int      head;                      /* read cursor */
    int      count;                     /* bytes available */
    struct ofile* fdq[USOCK_FDQ];       /* fds the peer passed, waiting to recv */
    int      fdq_count;
    /* Tier A.3 — readers blocked on THIS endpoint's rx wait here.  The queue
     * lock guards this endpoint's ring + fd queue too (condvar discipline):
     * a peer's usock_send fills the ring and wakes under this lock, a recv
     * drains + blocks under it, so no wakeup is lost and the ring is
     * concurrency-safe.  Note the pairing: send(A) fills A->peer(B)'s ring
     * and wakes B->readers; a blocked recv(B) waits on B->readers. */
    struct waitq readers;
};

/* Create a connected pair.  Returns 0 on success, -1 on OOM. */
int usock_pair(struct usock** a, struct usock** b) {
    struct usock* sa = (struct usock*)kcalloc(1, sizeof *sa);
    struct usock* sb = (struct usock*)kcalloc(1, sizeof *sb);
    if (!sa || !sb) { if (sa) kfree(sa); if (sb) kfree(sb); return -1; }
    sa->peer = sb;
    sb->peer = sa;
    waitq_init(&sa->readers);
    waitq_init(&sb->readers);
    *a = sa;
    *b = sb;
    return 0;
}

/* Send: append `n` bytes to the peer's ring (up to available space) and,
 * if `passfile` is non-NULL, queue a fresh reference to it on the peer.
 * Returns bytes written, or -1 if the peer has closed.  Fills + wakes under
 * the PEER's read wait-queue lock so a blocked recv(peer) sees the data and
 * cannot miss the wakeup. */
long usock_send(struct usock* s, const void* buf, size_t n, struct ofile* passfile) {
    if (!s || !s->peer) return -1;
    struct usock* p = s->peer;

    uint32_t f = waitq_lock(&p->readers);
    const uint8_t* src = (const uint8_t*)buf;
    size_t wrote = 0;
    while (wrote < n && p->count < USOCK_BUF) {
        p->rx[(p->head + p->count) % USOCK_BUF] = src[wrote++];
        p->count++;
    }
    if (passfile && p->fdq_count < USOCK_FDQ)
        p->fdq[p->fdq_count++] = ofile_ref(passfile);   /* travelling reference */
    if (wrote > 0 || passfile)
        waitq_wake_all(&p->readers);                     /* wake blocked recv(peer) */
    waitq_unlock(&p->readers, f);

    if (wrote > 0 || passfile) fd_readiness_signal();    /* wake blocked poll() */
    return (long)wrote;
}

/* Receive: drain up to `n` bytes from this endpoint's ring into `buf`.  If
 * `block` and nothing is available (no bytes, no passed fd) while the peer is
 * still open, park on this endpoint's read wait-queue until a send/close wakes
 * us, then re-drain.  If `passfile_out` is non-NULL it receives the next
 * queued passed ofile (whose reference now belongs to the caller) or NULL. */
long usock_recv(struct usock* s, void* buf, size_t n, int block,
                struct ofile** passfile_out) {
    if (!s) { if (passfile_out) *passfile_out = NULL; return -1; }

    uint32_t f = waitq_lock(&s->readers);

    /* Wait until there is something to receive — bytes or a passed fd — or the
     * peer has closed (then we return EOF/0), or the caller is non-blocking. */
    while (block && s->count == 0 && s->fdq_count == 0 && s->peer != NULL)
        waitq_block(&s->readers);

    uint8_t* dst = (uint8_t*)buf;
    size_t got = 0;
    while (got < n && s->count > 0) {
        dst[got++] = s->rx[s->head];
        s->head = (s->head + 1) % USOCK_BUF;
        s->count--;
    }

    if (passfile_out) {
        if (s->fdq_count > 0) {
            *passfile_out = s->fdq[0];
            for (int i = 1; i < s->fdq_count; i++) s->fdq[i - 1] = s->fdq[i];
            s->fdq_count--;
        } else {
            *passfile_out = NULL;
        }
    }
    waitq_unlock(&s->readers, f);
    return (long)got;
}

/* Readiness queries for poll(2). */
int usock_can_read(struct usock* s)  { return s && s->count > 0; }
int usock_can_write(struct usock* s) { return s && s->peer && s->peer->count < USOCK_BUF; }

/* Close one endpoint: disconnect the peer, drop any still-queued passed fds
 * (their travelling references), and free.  Called by ofile_unref(FD_SOCK)
 * — this strong definition overrides the weak stub in fd.c.
 *
 * Waking the peer's readers (under its queue lock) is what lets a task blocked
 * in recv(peer) return EOF once we go away instead of hanging forever. */
void usock_close(struct usock* s) {
    if (!s) return;
    struct usock* p = s->peer;
    if (p) {
        uint32_t f = waitq_lock(&p->readers);
        p->peer = NULL;                      /* peer now sees us gone */
        waitq_wake_all(&p->readers);         /* unblock recv(peer) → EOF */
        waitq_unlock(&p->readers, f);
        fd_readiness_signal();               /* poll(peer) re-scans → EOF/err */
    }
    for (int i = 0; i < s->fdq_count; i++) ofile_unref(s->fdq[i]);
    kfree(s);
}
