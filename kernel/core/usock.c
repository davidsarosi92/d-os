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
 * path) build on it later.  Blocking semantics (a read that waits for data)
 * arrive with the concurrent-process scheduling work — today read/recv are
 * non-blocking (return what's buffered, 0 if empty).
 * ============================================================================= */

#include "fd.h"
#include "kmalloc.h"
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
};

/* Create a connected pair.  Returns 0 on success, -1 on OOM. */
int usock_pair(struct usock** a, struct usock** b) {
    struct usock* sa = (struct usock*)kcalloc(1, sizeof *sa);
    struct usock* sb = (struct usock*)kcalloc(1, sizeof *sb);
    if (!sa || !sb) { if (sa) kfree(sa); if (sb) kfree(sb); return -1; }
    sa->peer = sb;
    sb->peer = sa;
    *a = sa;
    *b = sb;
    return 0;
}

/* Send: append `n` bytes to the peer's ring (up to available space) and,
 * if `passfile` is non-NULL, queue a fresh reference to it on the peer.
 * Returns bytes written, or -1 if the peer has closed. */
long usock_send(struct usock* s, const void* buf, size_t n, struct ofile* passfile) {
    if (!s || !s->peer) return -1;
    struct usock* p = s->peer;

    const uint8_t* src = (const uint8_t*)buf;
    size_t wrote = 0;
    while (wrote < n && p->count < USOCK_BUF) {
        p->rx[(p->head + p->count) % USOCK_BUF] = src[wrote++];
        p->count++;
    }

    if (passfile && p->fdq_count < USOCK_FDQ)
        p->fdq[p->fdq_count++] = ofile_ref(passfile);   /* travelling reference */

    return (long)wrote;
}

/* Receive: drain up to `n` bytes from this endpoint's ring into `buf`.  If
 * `passfile_out` is non-NULL it receives the next queued passed ofile (whose
 * reference now belongs to the caller) or NULL if none. */
long usock_recv(struct usock* s, void* buf, size_t n, struct ofile** passfile_out) {
    if (!s) { if (passfile_out) *passfile_out = NULL; return -1; }

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
    return (long)got;
}

/* Readiness queries for poll(2). */
int usock_can_read(struct usock* s)  { return s && s->count > 0; }
int usock_can_write(struct usock* s) { return s && s->peer && s->peer->count < USOCK_BUF; }

/* Close one endpoint: disconnect the peer, drop any still-queued passed fds
 * (their travelling references), and free.  Called by ofile_unref(FD_SOCK)
 * — this strong definition overrides the weak stub in fd.c. */
void usock_close(struct usock* s) {
    if (!s) return;
    if (s->peer) s->peer->peer = NULL;
    for (int i = 0; i < s->fdq_count; i++) ofile_unref(s->fdq[i]);
    kfree(s);
}
