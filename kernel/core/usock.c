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
#include "task.h"
#include "cred.h"
#include <stddef.h>
#include <stdint.h>

#define USOCK_BUF   32768       /* per-endpoint receive ring (§M90: was 4096 —
                                 * gRPC frames between docker and dockerd are
                                 * 16 KiB and more, and a ring smaller than one
                                 * frame turns every message into many wakeups) */
#define USOCK_ACCQ  16          /* §M90 — pending connections per listener */
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
    /* §M56.2 — the ofile this endpoint lives behind, so a readiness change can
     * name itself instead of telling every poller in the system to re-look.
     * Safe as a bare pointer because the ofile OWNS this object: it is freed
     * by ofile_unref, so the pointer cannot outlive what it points at. */
    struct ofile* owner;
    /* §M90 — NAMED sockets (AF_UNIX bound to a path).  An endpoint is one of:
     * a member of a connected pair (peer set, or NULL once it closed), an
     * UNBOUND/BOUND socket that is neither (fresh from socket(2)), or a
     * LISTENER holding connections waiting for accept(2). */
    int           listening;
    int           backlog;
    struct usock* accq[USOCK_ACCQ];     /* server ends, waiting for accept   */
    int           acc_n;
    char          path[108];            /* bound name ("" = unnamed)         */
    int           ever_connected;       /* send/recv before connect: ENOTCONN */
    /* §M90 — writers blocked because THIS endpoint's ring is full; woken by
     * recv after it drains.  Before this, a full ring made write(2) return 0,
     * which a stream writer cannot interpret (it is neither EAGAIN nor EOF). */
    struct waitq  writers;
    /* §M90 — SO_PEERCRED.  `own_*`: the process that created this endpoint
     * (a listener: the one that called listen).  `peer_*`: who was at the
     * other end WHEN THE CONNECTION WAS MADE — Linux records it then, and it
     * survives the peer closing.  containerd's ttrpc refuses a connection
     * whose credentials it cannot read. */
    int           own_pid, own_uid, own_gid;
    int           peer_pid, peer_uid, peer_gid;
    int           has_peercred;
    /* §M90 — SOCK_SEQPACKET: the ring holds MESSAGES, each a 4-byte length
     * then its bytes.  A send is one message, whole or not at all; a recv
     * returns exactly one, dropping what does not fit (Linux's MSG_TRUNC
     * behaviour).  runc's sync pipe is one: a stream would merge two of its
     * JSON messages into one read. */
    int           seqpacket;
    int           is_pipe;           /* §M90 — made by pipe(): reads as "pipe:[n]" */
    uint32_t      pipe_id;           /* §M90 — one number for both ends of a pipe */
};
#define SEQ_HDR 4

/* The calling process's credentials, as SO_PEERCRED reports them. */
static void usock_my_creds(int* pid, int* uid, int* gid) {
    struct task* t = task_current();
    *pid = t ? task_tgid(t) : 0;
    int u = t ? cred_uid(&t->cred) : 0, g = t ? cred_gid(&t->cred) : 0;
    *uid = u < 0 ? 0 : u;
    *gid = g < 0 ? 0 : g;
}

void usock_set_owner(struct usock* s, struct ofile* o) { if (s) s->owner = o; }

/* Create a connected pair.  Returns 0 on success, -1 on OOM. */
int usock_pair(struct usock** a, struct usock** b) {
    struct usock* sa = (struct usock*)kcalloc(1, sizeof *sa);
    struct usock* sb = (struct usock*)kcalloc(1, sizeof *sb);
    if (!sa || !sb) { if (sa) kfree(sa); if (sb) kfree(sb); return -1; }
    sa->peer = sb;
    sb->peer = sa;
    waitq_init(&sa->readers);
    waitq_init(&sb->readers);
    waitq_init(&sa->writers);
    waitq_init(&sb->writers);
    sa->ever_connected = sb->ever_connected = 1;
    /* §M90 — both ends were made by the caller, so each one's peer is it. */
    usock_my_creds(&sa->own_pid, &sa->own_uid, &sa->own_gid);
    sb->own_pid = sa->own_pid; sb->own_uid = sa->own_uid; sb->own_gid = sa->own_gid;
    sa->peer_pid = sb->peer_pid = sa->own_pid;
    sa->peer_uid = sb->peer_uid = sa->own_uid;
    sa->peer_gid = sb->peer_gid = sa->own_gid;
    sa->has_peercred = sb->has_peercred = 1;
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
    if (p->seqpacket) {
        /* One message: all of it with its header, or nothing (0 = no room). */
        if (n + SEQ_HDR > USOCK_BUF) { waitq_unlock(&p->readers, f); return -90; }  /* EMSGSIZE */
        if ((size_t)(USOCK_BUF - p->count) < n + SEQ_HDR) { waitq_unlock(&p->readers, f); return 0; }
        uint32_t len = (uint32_t)n;
        for (int i = 0; i < SEQ_HDR; i++)
            p->rx[(p->head + p->count + i) % USOCK_BUF] = (uint8_t)(len >> (8 * i));
        p->count += SEQ_HDR;
        for (size_t i = 0; i < n; i++) p->rx[(p->head + p->count + i) % USOCK_BUF] = src[i];
        p->count += (int)n;
        if (passfile && p->fdq_count < USOCK_FDQ) p->fdq[p->fdq_count++] = ofile_ref(passfile);
        waitq_wake_all(&p->readers);
        waitq_unlock(&p->readers, f);
        fd_readiness_changed(p->owner);
        return (long)n;                  /* an empty message is sent and counts 0 bytes */
    }
    while (wrote < n && p->count < USOCK_BUF) {
        p->rx[(p->head + p->count) % USOCK_BUF] = src[wrote++];
        p->count++;
    }
    if (passfile && p->fdq_count < USOCK_FDQ)
        p->fdq[p->fdq_count++] = ofile_ref(passfile);   /* travelling reference */
    if (wrote > 0 || passfile)
        waitq_wake_all(&p->readers);                     /* wake blocked recv(peer) */
    waitq_unlock(&p->readers, f);

    /* The PEER became readable, not us — name its description so a poller
     * watching a hundred other descriptors re-evaluates only this one. */
    if (wrote > 0 || passfile) fd_readiness_changed(p->owner);
    return (long)wrote;
}

/* §M59 — send ALL of `n` bytes or NONE of them (returns n, 0 = no room yet,
 * -1 = peer gone).  A message protocol over a byte ring cannot survive a short
 * write: half a Wayland event followed by the next event's header is a stream
 * the client can never resynchronise, and libwayland drops the connection.
 * Deciding under the peer's lock also makes two SENDERS safe — the Wayland
 * server's own task and the compositor (input events) both write the same
 * socket, and "check the space, then write" as two steps lets the other fill
 * the gap in between. */
long usock_send_whole(struct usock* s, const void* buf, size_t n, struct ofile* passfile) {
    if (!s || !s->peer) return -1;
    struct usock* p = s->peer;
    uint32_t f = waitq_lock(&p->readers);
    if ((size_t)(USOCK_BUF - p->count) < n || (passfile && p->fdq_count >= USOCK_FDQ)) {
        waitq_unlock(&p->readers, f);
        return 0;
    }
    const uint8_t* src = (const uint8_t*)buf;
    for (size_t i = 0; i < n; i++)
        p->rx[(p->head + p->count + i) % USOCK_BUF] = src[i];
    p->count += n;
    if (passfile) p->fdq[p->fdq_count++] = ofile_ref(passfile);
    waitq_wake_all(&p->readers);
    waitq_unlock(&p->readers, f);
    fd_readiness_changed(p->owner);
    return (long)n;
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
    int was_full = 0;

    /* Wait until there is something to receive — bytes or a passed fd — or the
     * peer has closed (then we return EOF/0), or the caller is non-blocking. */
    while (block && s->count == 0 && s->fdq_count == 0 && s->peer != NULL) {
        if (task_should_stop()) {                 /* §M90 — killed while waiting */
            waitq_unlock(&s->readers, f);
            if (passfile_out) *passfile_out = NULL;
            return -4;                            /* EINTR */
        }
        waitq_block(&s->readers);
    }

    uint8_t* dst = (uint8_t*)buf;
    size_t got = 0;
    was_full = s->count >= USOCK_BUF;
    int took_msg = 0;
    if (s->seqpacket) {
        if (s->count >= SEQ_HDR) {               /* exactly one message */
            uint32_t len = 0;
            for (int i = 0; i < SEQ_HDR; i++) {
                len |= (uint32_t)s->rx[s->head] << (8 * i);
                s->head = (s->head + 1) % USOCK_BUF;
            }
            s->count -= SEQ_HDR;
            for (uint32_t i = 0; i < len; i++) {
                if (got < n) dst[got++] = s->rx[s->head];   /* the rest is truncated */
                s->head = (s->head + 1) % USOCK_BUF;
            }
            s->count -= (int)len;
            took_msg = 1;
            was_full = 1;                        /* room for a whole message may have appeared */
        }
    } else
    while (got < n && s->count > 0) {
        dst[got++] = s->rx[s->head];
        s->head = (s->head + 1) % USOCK_BUF;
        s->count--;
    }

    if (got > 0 || took_msg) {           /* §M90 — room for a blocked writer */
        uint32_t wf = waitq_lock(&s->writers);
        waitq_wake_all(&s->writers);
        waitq_unlock(&s->writers, wf);
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
    /* §M90 — draining a FULL ring makes the PEER writable: that is a readiness
     * change too, and nothing announced it — a poll(POLLOUT) on a full pipe
     * slept until something unrelated woke it, and an edge-triggered epoll
     * (Go's netpoller) would never hear of it at all.  Only the full -> not
     * full transition matters: usock_can_write is "count < USOCK_BUF". */
    if ((got > 0 || took_msg) && was_full) fd_readiness_changed(NULL);
    return (long)got;
}

/* Readiness queries for poll(2). */
int usock_can_read(struct usock* s)  { return s && (s->count > 0 || s->acc_n > 0); }
int usock_can_write(struct usock* s) {
    if (!s || !s->peer) return 0;
    /* a SEQPACKET peer has room only for a header plus at least one byte */
    return s->peer->count < USOCK_BUF - (s->peer->seqpacket ? SEQ_HDR : 0);
}
/* §M56.1 — is the other end still there?  `peer` is cleared by usock_close, so
 * this is the whole hangup story for a pipe or socketpair.  Kept separate from
 * can_read so a reader can drain what is already buffered before it acts on
 * the close — collapsing the two would discard the tail of every conversation
 * whose writer closed promptly, which is most of them. */
int usock_peer_open(struct usock* s) { return s && s->peer != NULL; }

/* Close one endpoint: disconnect the peer, drop any still-queued passed fds
 * (their travelling references), and free.  Called by ofile_unref(FD_SOCK)
 * — this strong definition overrides the weak stub in fd.c.
 *
 * Waking the peer's readers (under its queue lock) is what lets a task blocked
 * in recv(peer) return EOF once we go away instead of hanging forever. */
static void reg_remove(struct usock* s);
void usock_close(struct usock* s) {
    if (!s) return;
    if (s->path[0] || s->listening) reg_remove(s);
    for (int i = 0; i < s->acc_n; i++) usock_close(s->accq[i]);   /* never accepted */
    s->acc_n = 0;
    {   /* a blocked writer must see the end, not sleep on freed memory */
        uint32_t wf = waitq_lock(&s->writers);
        waitq_wake_all(&s->writers);
        waitq_unlock(&s->writers, wf);
    }
    struct usock* p = s->peer;
    if (p) {
        uint32_t f = waitq_lock(&p->readers);
        p->peer = NULL;                      /* peer now sees us gone */
        waitq_wake_all(&p->readers);         /* unblock recv(peer) → EOF */
        waitq_unlock(&p->readers, f);
        fd_readiness_changed(p->owner);      /* the peer's poll → EOF/HUP */
    }
    for (int i = 0; i < s->fdq_count; i++) ofile_unref(s->fdq[i]);
    kfree(s);
}


/* =============================================================================
 * §M90 (2026-10-01) — NAMED UNIX SOCKETS: socket(AF_UNIX) + bind(path) +
 * listen + accept, and connect(path).  Docker is built on them: the CLI talks
 * to dockerd over /var/run/docker.sock, dockerd to containerd over its gRPC
 * socket — before this d-os had only socketpair(2), and dockerd stopped at
 * "socket: address family not supported by protocol".
 *
 * A bound name lives in a small registry (the authority for connect), and a
 * FILE is created at the path as well, because programs treat the socket as
 * a file: dockerd chmods it, a client tests that it exists, an old one is
 * removed before re-binding.  An abstract name (sun_path[0] == 0) has no file.
 * Only SOCK_STREAM — what Docker, containerd and gRPC use.
 * ============================================================================= */
#include "lock.h"
#include "vfs.h"

#define UREG_MAX 64
static struct { char path[108]; struct usock* s; } g_ureg[UREG_MAX];
static spinlock_t g_ureg_lock = SPINLOCK_INIT;

static int ueq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void reg_remove(struct usock* s) {
    uint32_t fl = spin_lock_irqsave(&g_ureg_lock);
    for (int i = 0; i < UREG_MAX; i++) if (g_ureg[i].s == s) { g_ureg[i].s = NULL; g_ureg[i].path[0] = 0; }
    spin_unlock_irqrestore(&g_ureg_lock, fl);
}

struct usock* usock_new(void) {
    struct usock* s = (struct usock*)kcalloc(1, sizeof *s);
    if (!s) return NULL;
    waitq_init(&s->readers);
    waitq_init(&s->writers);
    usock_my_creds(&s->own_pid, &s->own_uid, &s->own_gid);   /* §M90 */
    return s;
}

/* name: a path (absolute, as the caller canonicalised it) or "@abstract". */
int usock_bind(struct usock* s, const char* name) {
    if (!s || !name || !name[0]) return -22;                  /* EINVAL */
    if (s->path[0] || s->peer || s->listening) return -22;
    uint32_t fl = spin_lock_irqsave(&g_ureg_lock);
    int slot = -1;
    for (int i = 0; i < UREG_MAX; i++) {
        if (g_ureg[i].s && ueq(g_ureg[i].path, name)) { spin_unlock_irqrestore(&g_ureg_lock, fl); return -98; } /* EADDRINUSE */
        if (!g_ureg[i].s && slot < 0) slot = i;
    }
    if (slot < 0) { spin_unlock_irqrestore(&g_ureg_lock, fl); return -105; }   /* ENOBUFS */
    int k = 0; for (; name[k] && k < 107; k++) g_ureg[slot].path[k] = name[k];
    g_ureg[slot].path[k] = 0;
    g_ureg[slot].s = s;
    spin_unlock_irqrestore(&g_ureg_lock, fl);
    for (k = 0; name[k] && k < 107; k++) s->path[k] = name[k];
    s->path[k] = 0;
    if (name[0] == '/') {
        struct vfs_stat st;
        if (vfs_stat(name, &st) == 0) { reg_remove(s); s->path[0] = 0; return -98; }   /* the file exists */
        struct file* f = vfs_open(name, VFS_WRONLY | VFS_CREATE);
        if (!f) { reg_remove(s); s->path[0] = 0; return -2; }                        /* ENOENT (no dir) */
        vfs_close(f);
    }
    return 0;
}

int usock_listen(struct usock* s, int backlog) {
    if (!s || s->peer) return -22;
    if (!s->path[0]) return -22;              /* an unbound listener cannot be reached */
    s->listening = 1;
    s->backlog = backlog < 1 ? 1 : backlog > USOCK_ACCQ ? USOCK_ACCQ : backlog;
    usock_my_creds(&s->own_pid, &s->own_uid, &s->own_gid);   /* §M90 — at listen */
    return 0;
}

int usock_connect(struct usock* s, const char* name) {
    if (!s || s->listening) return -22;
    if (s->peer || s->ever_connected) return -106;           /* EISCONN */
    struct usock* l = NULL;
    uint32_t fl = spin_lock_irqsave(&g_ureg_lock);
    for (int i = 0; i < UREG_MAX; i++)
        if (g_ureg[i].s && ueq(g_ureg[i].path, name)) { l = g_ureg[i].s; break; }
    spin_unlock_irqrestore(&g_ureg_lock, fl);
    if (!l || !l->listening) return name[0] == '/' ? -111 : -111;   /* ECONNREFUSED */
    struct usock* srv = usock_new();
    if (!srv) return -12;
    uint32_t lf = waitq_lock(&l->readers);
    if (l->acc_n >= l->backlog) { waitq_unlock(&l->readers, lf); kfree(srv); return -11; }  /* EAGAIN */
    srv->peer = s; s->peer = srv;
    srv->ever_connected = s->ever_connected = 1;
    /* §M90 — the credentials, as of now: the client sees the listener's
     * owner, the server end sees the connecting process. */
    srv->own_pid = l->own_pid; srv->own_uid = l->own_uid; srv->own_gid = l->own_gid;
    s->peer_pid = l->own_pid;  s->peer_uid = l->own_uid;  s->peer_gid = l->own_gid;
    usock_my_creds(&srv->peer_pid, &srv->peer_uid, &srv->peer_gid);
    srv->has_peercred = s->has_peercred = 1;
    l->accq[l->acc_n++] = srv;
    waitq_wake_all(&l->readers);
    waitq_unlock(&l->readers, lf);
    fd_readiness_changed(l->owner);
    return 0;
}

/* The next pending connection, or NULL: -11 (EAGAIN) when non-blocking and
 * none is waiting. */
int usock_accept(struct usock* l, int block, struct usock** out) {
    *out = NULL;
    if (!l || !l->listening) return -22;
    uint32_t f = waitq_lock(&l->readers);
    while (l->acc_n == 0) {
        if (!block) { waitq_unlock(&l->readers, f); return -11; }
        if (task_should_stop()) { waitq_unlock(&l->readers, f); return -4; }   /* §M90 */
        waitq_block(&l->readers);
    }
    *out = l->accq[0];
    for (int i = 1; i < l->acc_n; i++) l->accq[i - 1] = l->accq[i];
    l->acc_n--;
    waitq_unlock(&l->readers, f);
    return 0;
}

int usock_is_listener(struct usock* s) { return s && s->listening; }
const char* usock_name(struct usock* s) { return s ? s->path : ""; }
int usock_connected(struct usock* s) { return s && s->ever_connected; }

/* A STREAM write: blocks while the peer's ring is full (or returns -11 when
 * non-blocking), -32 (EPIPE) once the peer is gone, -107 (ENOTCONN) before a
 * connection.  usock_send itself stays the non-blocking primitive the self-
 * tests and the Wayland server rely on. */
long usock_write(struct usock* s, const void* buf, size_t n, int block) {
    if (!s) return -9;
    if (!s->ever_connected) return -107;
    for (;;) {
        if (!s->peer) return -32;
        long w = usock_send(s, buf, n, NULL);
        if (w == -90) return -90;                                   /* EMSGSIZE */
        if (w != 0 || n == 0) return w < 0 ? -32 : w;
        if (!block) return -11;
        struct usock* p = s->peer;
        if (!p) return -32;
        uint32_t f = waitq_lock(&p->writers);
        if (task_should_stop()) { waitq_unlock(&p->writers, f); return -4; }   /* §M90 */
        if (s->peer && (s->peer->seqpacket
                        ? (size_t)(USOCK_BUF - s->peer->count) < n + SEQ_HDR
                        : s->peer->count >= USOCK_BUF))
            waitq_block(&p->writers);
        waitq_unlock(&p->writers, f);
    }
}

/* §M90 — SO_PEERCRED: 0 and the recorded peer, or -1 when this endpoint was
 * never connected (Linux then answers pid 0, uid/gid -1 — the caller does). */
int usock_peercred(struct usock* s, int* pid, int* uid, int* gid) {
    if (!s || !s->has_peercred) return -1;
    *pid = s->peer_pid; *uid = s->peer_uid; *gid = s->peer_gid;
    return 0;
}

/* §M90 — make a fresh pair SOCK_SEQPACKET (both ends), before any data. */
void usock_set_seqpacket(struct usock* a, struct usock* b) {
    if (a) a->seqpacket = 1;
    if (b) b->seqpacket = 1;
}
int usock_is_seqpacket(struct usock* s) { return s && s->seqpacket; }

/* §M90 — mark a pair as a PIPE (it is a usock pair underneath) so /proc can
 * name it as Linux does ("pipe:[n]" rather than "socket:[n]"). */
void usock_set_pipe(struct usock* a, struct usock* b) {
    static uint32_t next = 1;
    uint32_t id = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
    if (a) { a->is_pipe = 1; a->pipe_id = id; }
    if (b) { b->is_pipe = 1; b->pipe_id = id; }
}
int usock_is_pipe(struct usock* s) { return s && s->is_pipe; }
/* The number /proc shows: the PIPE's (shared by its two ends), else 0. */
uint32_t usock_pipe_id(struct usock* s) { return s ? s->pipe_id : 0; }

/* §M90 — recv with Linux's flags (recvfrom / recvmsg on a unix socket):
 *   MSG_PEEK     (0x02) copy without consuming;
 *   MSG_TRUNC    (0x20) on SOCK_SEQPACKET, return the message's REAL length
 *                       even when it did not fit (runc asks the size of the
 *                       next packet with recvfrom(fd, NULL, 0, PEEK|TRUNC));
 *                       on a stream, the bytes are discarded, not copied;
 *   MSG_DONTWAIT (0x40) never block (as non-blocking does).
 * Returns bytes (or the length above), 0 at EOF, -11 EAGAIN when nothing
 * waits and the call may not block. */
long usock_recv_flags(struct usock* s, void* buf, size_t n, int block, int flags) {
    if (!s) return -9;
    int peek = flags & 0x02, trunc = flags & 0x20;
    if (flags & 0x40) block = 0;
    uint32_t f = waitq_lock(&s->readers);
    while (block && s->count == 0 && s->peer != NULL) {
        if (task_should_stop()) { waitq_unlock(&s->readers, f); return -4; }   /* §M90 */
        waitq_block(&s->readers);
    }
    if (s->count == 0) {
        int open = s->peer != NULL;
        waitq_unlock(&s->readers, f);
        return open ? -11 : 0;
    }
    uint8_t* dst = (uint8_t*)buf;
    long ret = 0;
    int consumed = 0;
    if (s->seqpacket) {
        if (s->count < SEQ_HDR) { waitq_unlock(&s->readers, f); return -11; }
        uint32_t len = 0;
        for (int i = 0; i < SEQ_HDR; i++)
            len |= (uint32_t)s->rx[(s->head + i) % USOCK_BUF] << (8 * i);
        size_t take = len < n ? len : n;
        for (size_t i = 0; i < take; i++)
            dst[i] = s->rx[(s->head + SEQ_HDR + i) % USOCK_BUF];
        if (!peek) {
            s->head = (s->head + SEQ_HDR + (int)len) % USOCK_BUF;
            s->count -= SEQ_HDR + (int)len;
            consumed = 1;
        }
        ret = trunc ? (long)len : (long)take;
    } else {
        size_t take = (size_t)s->count < n ? (size_t)s->count : n;
        if (!trunc) for (size_t i = 0; i < take; i++) dst[i] = s->rx[(s->head + i) % USOCK_BUF];
        if (!peek) {
            s->head = (s->head + (int)take) % USOCK_BUF;
            s->count -= (int)take;
            consumed = take > 0;
        }
        ret = (long)take;
    }
    waitq_unlock(&s->readers, f);
    if (consumed) {
        uint32_t wf = waitq_lock(&s->writers);
        waitq_wake_all(&s->writers);
        waitq_unlock(&s->writers, wf);
        fd_readiness_changed(NULL);
    }
    return ret;
}
