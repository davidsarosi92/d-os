/* =============================================================================
 * tty.c — terminals, §M90 rung 6: pseudo-terminal pairs, the line discipline,
 * devpts instances and the controlling terminal.
 *
 * WHY.  `docker run -it` is a terminal from end to end: the docker CLI puts
 * ITS terminal into raw mode and streams it; runc opens /dev/ptmx inside the
 * container's own devpts, passes the MASTER to containerd's shim over a unix
 * socket, and makes the SLAVE the stdin/stdout/stderr and the CONTROLLING
 * terminal of the container's init — so that ^C reaches the shell inside as
 * SIGINT, a full-screen program can ask its size, and a shell's job control
 * works.  None of that existed: every ioctl answered ENOTTY, /dev/ptmx was a
 * name with nothing behind it, and "controlling terminal" was a comment.
 *
 * THE PAIR.  One `struct tty` per pty: the slave's termios (the full line
 * discipline below), the master's termios (only its output post-processing
 * is applied — the master's own INPUT side is raw, as it is by default on
 * Linux; a program that turns canonical mode on for a MASTER gets no line
 * editing, said here), one window size, and two queues:
 *
 *   master write ──► [line discipline: ICRNL, ISIG, ICANON editing, ECHO]
 *                        │                      │ echo
 *                        ▼                      ▼
 *                   read queue (rq)        output queue (oq) ──► master read
 *   slave read  ◄────────┘                      ▲
 *   slave write ──► [OPOST: ONLCR, OCRNL] ──────┘
 *
 * THE LINE DISCIPLINE is Linux's n_tty in the parts programs observe:
 *   ICRNL/INLCR/IGNCR/ISTRIP; ISIG — VINTR/VQUIT/VSUSP signal the FOREGROUND
 *   process group (and flush, unless NOFLSH); IXON — VSTOP/VSTART stop and
 *   resume output; ICANON — a read returns one LINE, VERASE/VWERASE/VKILL edit
 *   it, VEOF ends it (an empty line read as 0 = end of file), VLNEXT quotes,
 *   VREPRINT reprints; ECHO/ECHOE/ECHOK/ECHOKE/ECHONL/ECHOCTL as specified;
 *   non-canonical reads honour VMIN and VTIME (VTIME is measured in steps of
 *   10 ms, a tenth of its unit).  NOT here, said plainly: parity/break
 *   handling, IUCLC/OLCUC/XCASE, IMAXBEL's bell, delays, and TIOCPKT packet
 *   mode (refused).
 *
 * HANGUP (Linux's rules, which the copy loops depend on):
 *   - the last MASTER close hangs the slave up: the session leader gets SIGHUP
 *     and SIGCONT, the foreground group SIGHUP; slave reads return 0, writes
 *     EIO, poll says HUP; the /dev/pts/N name disappears (devpts_pty_kill);
 *   - the last SLAVE close (after one was opened) makes the master's read
 *     return what is left and then EIO, its poll HUP — containerd's console
 *     copy ends on exactly that.
 *
 * THE CONTROLLING TERMINAL belongs to a SESSION: `tty->session` is the
 * leader's pid, `task->ctty_id` the terminal a task was given (inherited by
 * fork).  A task's ctty is valid only while the tty is alive AND still
 * belongs to the task's session — so a hangup or TIOCNOTTY dissociates the
 * whole session at once without visiting its tasks, and a stale id after the
 * pty is gone resolves to nothing (ids carry a generation).  A session leader
 * opening a slave without O_NOCTTY acquires it (if it has none and the tty is
 * free), TIOCSCTTY does so explicitly; /dev/tty opens it.  Job control: a
 * BACKGROUND process reading its ctty gets SIGTTIN (EIO if it ignores or
 * blocks it), writing with TOSTOP SIGTTOU, and so does a background
 * tcsetpgrp/tcsetattr — each returns EINTR after the signal (there is no
 * syscall restart here; libc and shells retry).
 *
 * DEVPTS.  Each devpts mount is its own INSTANCE (Linux since 4.7): a ptmx
 * node, and a node per pty opened through it, numbered from 0 in that
 * instance — the container's /dev/pts/0 is not the host's.  The machine's own
 * instance is /dev/pts, opened through /dev/ptmx as well.  Slave nodes are the
 * opener's uid, group 5 (tty), mode 0620; ptmx 0666; device numbers 136:N and
 * 5:2, which is what runc's device allow-list names.
 *
 * LOCKING.  A pty's waitq lock guards everything in it (waitq.h's condvar
 * discipline, as in fifo.c); every change wakes all of its waiters to
 * re-check.  Signals are SENT after the lock is dropped — posting may take the
 * task list's lock.  `g_tab_lock` guards the id table and the open counts, so
 * an open through /dev/tty cannot race the free.
 * ============================================================================= */

#include "tty.h"
#include "fd.h"
#include "vfs.h"
#include "devfs.h"
#include "kmalloc.h"
#include "waitq.h"
#include "lock.h"
#include "task.h"
#include "cred.h"
#include "nsproxy.h"
#include "syscall.h"
#include "printf.h"
#include "lnx_signal.h"
#include <stdint.h>
#include <stddef.h>

#define TTY_MAX     128            /* ptys alive at once, machine-wide        */
#define DEVPTS_MAX  64             /* devpts instances                        */
#define RQ_SIZE     4096           /* slave input (N_TTY_BUF_SIZE)            */
#define OQ_SIZE     8192           /* slave output → master                   */

/* termios bits (Linux, octal as in <asm-generic/termbits.h>) */
#define T_IGNBRK  0000001
#define T_ISTRIP  0000040
#define T_INLCR   0000100
#define T_IGNCR   0000200
#define T_ICRNL   0000400
#define T_IXON    0002000
#define T_IXANY   0004000
#define T_OPOST   0000001
#define T_ONLCR   0000004
#define T_OCRNL   0000010
#define T_ISIG    0000001
#define T_ICANON  0000002
#define T_ECHO    0000010
#define T_ECHOE   0000020
#define T_ECHOK   0000040
#define T_ECHONL  0000100
#define T_NOFLSH  0000200
#define T_TOSTOP  0000400
#define T_ECHOCTL 0001000
#define T_ECHOKE  0004000
#define T_IEXTEN  0100000
enum { VINTR, VQUIT, VERASE, VKILL, VEOF, VTIME, VMIN, VSWTC, VSTART, VSTOP, VSUSP,
       VEOL, VREPRINT, VDISCARD, VWERASE, VLNEXT, VEOL2 };

/* errnos */
#define E_PERM 1
#define E_INTR 4
#define E_IO   5
#define E_NXIO 6
#define E_AGAIN 11
#define E_BUSY 16
#define E_INVAL 22
#define E_NOTTY 25

/* read-queue entries: the byte, plus flags */
#define RQ_EOL   0x100              /* ends a canonical line (delivered)      */
#define RQ_EOF   0x200              /* ends a line, NOT delivered (VEOF)      */

struct devpts {
    int            live;
    struct dentry* dir;
    char           path[96];        /* where it was made (for the host: /dev/pts) */
    uint8_t        used[TTY_MAX];   /* index taken                            */
    struct devfs_node ptmx;
};

struct tty {
    struct waitq    wq;
    uint32_t        id;             /* (generation << 8) | (slot + 1)          */
    int             slot, index;
    struct devpts*  inst;
    char            spath[112];     /* /dev/pts/N as the opener's namespace sees it */
    struct ktermios t;              /* slave termios: the line discipline     */
    struct ktermios mt;             /* master termios: output post-processing  */
    uint16_t        ws[4];          /* rows, cols, xpixel, ypixel             */
    uint16_t        rq[RQ_SIZE];    uint32_t rhead, rcount, lines;
    uint8_t         oq[OQ_SIZE];    uint32_t ohead, ocount;
    int             lnext, stopped, locked, excl, hung;
    int             master_open, slave_open, slave_ever;
    int             pins;           /* releases in progress: no free under them */
    int             session, pgrp;  /* global ids; 0 = none                   */
    int             owner_uid;
    struct devfs_node snode;        /* /dev/pts/N                              */
    char            sname[8];
    int             snode_live;
};

static struct tty*   g_tab[TTY_MAX];
static uint32_t      g_gen[TTY_MAX];
static spinlock_t    g_tab_lock = SPINLOCK_INIT;
static struct devpts g_pts[DEVPTS_MAX];
static spinlock_t    g_pts_lock = SPINLOCK_INIT;

/* ---- small helpers -------------------------------------------------------------- */

static void scpy(char* d, const char* s, size_t cap) {
    size_t i = 0;
    for (; s && s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}
static void put_dec(char* d, size_t cap, unsigned v) {
    char t[12]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    size_t i = 0;
    while (d[i] && i + 1 < cap) i++;
    while (k && i + 1 < cap) d[i++] = t[--k];
    d[i] = 0;
}
static void sapp(char* d, const char* s, size_t cap) {
    size_t i = 0;
    while (d[i] && i + 1 < cap) i++;
    for (; *s && i + 1 < cap; s++) d[i++] = *s;
    d[i] = 0;
}

static void termios_defaults(struct ktermios* t, int master) {
    for (unsigned i = 0; i < sizeof *t; i++) ((uint8_t*)t)[i] = 0;
    /* Linux tty_std_termios; a pty master starts raw (pty_init) */
    t->c_cflag = 0000017 /* B38400 */ | 0000060 /* CS8 */ | 0000200 /* CREAD */;
    if (master) return;
    t->c_iflag = T_ICRNL | T_IXON;
    t->c_oflag = T_OPOST | T_ONLCR;
    t->c_cflag |= 0002000;                                     /* HUPCL */
    t->c_lflag = T_ISIG | T_ICANON | T_ECHO | T_ECHOE | T_ECHOK | T_ECHOCTL | T_ECHOKE | T_IEXTEN;
    static const uint8_t cc[TTY_NCCS] = { 3, 0x1c, 0x7f, 0x15, 4, 0, 1, 0, 0x11, 0x13, 0x1a,
                                          0, 0x12, 0x0f, 0x17, 0x16, 0 };
    for (int i = 0; i < TTY_NCCS; i++) t->c_cc[i] = cc[i];
}

/* Is the special character `v` enabled and equal to c?  (0 disables it —
 * _POSIX_VDISABLE on Linux.) */
static int is_cc(const struct tty* p, int v, uint8_t c) { return p->t.c_cc[v] && p->t.c_cc[v] == c; }

/* ---- the output queue (slave → master), lock held ----------------------------- */

static int oq_room(const struct tty* p) { return OQ_SIZE - (int)p->ocount; }
static void oq_put(struct tty* p, uint8_t c) {
    if (p->ocount >= OQ_SIZE) return;                          /* echo dropped when full */
    p->oq[(p->ohead + p->ocount) % OQ_SIZE] = c;
    p->ocount++;
}
/* One byte of SLAVE output through OPOST. */
static void out_post(struct tty* p, uint8_t c) {
    if (p->t.c_oflag & T_OPOST) {
        if (c == '\n' && (p->t.c_oflag & T_ONLCR)) { oq_put(p, '\r'); oq_put(p, '\n'); return; }
        if (c == '\r' && (p->t.c_oflag & T_OCRNL)) c = '\n';
    }
    oq_put(p, c);
}
/* An echo: control characters as ^X under ECHOCTL (not TAB or NL). */
static void echo_char(struct tty* p, uint8_t c) {
    if (!(p->t.c_lflag & T_ECHO)) return;
    if ((p->t.c_lflag & T_ECHOCTL) && (c < 0x20 || c == 0x7f) && c != '\t' && c != '\n') {
        out_post(p, '^');
        out_post(p, c == 0x7f ? '?' : (uint8_t)(c + 0x40));
        return;
    }
    out_post(p, c);
}
static void echo_erase1(struct tty* p, uint8_t c) {
    /* the columns the erased character took on the screen */
    int w = ((p->t.c_lflag & T_ECHOCTL) && (c < 0x20 || c == 0x7f) && c != '\t') ? 2 : 1;
    for (int i = 0; i < w; i++) { out_post(p, '\b'); out_post(p, ' '); out_post(p, '\b'); }
}

/* ---- the read queue (master → slave), lock held ------------------------------- */

static uint16_t rq_at(const struct tty* p, uint32_t i) { return p->rq[(p->rhead + i) % RQ_SIZE]; }
static void rq_put(struct tty* p, uint16_t v) {
    p->rq[(p->rhead + p->rcount) % RQ_SIZE] = v;
    p->rcount++;
    if (v & (RQ_EOL | RQ_EOF)) p->lines++;
}
/* The length of the line being edited (after the last terminator). */
static uint32_t rq_partial(const struct tty* p) {
    uint32_t n = 0;
    while (n < p->rcount && !(rq_at(p, p->rcount - 1 - n) & (RQ_EOL | RQ_EOF))) n++;
    return n;
}
static void rq_flush(struct tty* p) { p->rcount = 0; p->rhead = 0; p->lines = 0; }
static void rq_recount(struct tty* p) {
    p->lines = 0;
    for (uint32_t i = 0; i < p->rcount; i++) if (rq_at(p, i) & (RQ_EOL | RQ_EOF)) p->lines++;
}
/* Bytes a reader can take now. */
static uint32_t rq_avail(const struct tty* p) {
    if (!(p->t.c_lflag & T_ICANON)) return p->rcount;
    if (!p->lines) return 0;
    uint32_t n = 0;
    while (n < p->rcount && !(rq_at(p, n) & (RQ_EOL | RQ_EOF))) n++;
    return n + 1;
}

/* One byte from the master into the slave's line discipline.  Returns 0
 * consumed, 1 no room (the writer waits), and sets *sig when it must signal
 * the foreground group. */
static int ldisc_receive(struct tty* p, uint8_t c, int* sig) {
    uint32_t i = p->t.c_iflag, l = p->t.c_lflag;
    int canon = (l & T_ICANON) != 0;
    if (i & T_ISTRIP) c &= 0x7f;
    if (p->lnext) {                                            /* a quoted character */
        if (p->rcount >= RQ_SIZE - 1) return 1;
        p->lnext = 0;
        if ((l & T_ECHO) && (l & T_ECHOCTL)) { out_post(p, '\b'); out_post(p, '\b'); }
        rq_put(p, c);
        echo_char(p, c);
        return 0;
    }
    if (c == '\r') {
        if (i & T_IGNCR) return 0;
        if (i & T_ICRNL) c = '\n';
    } else if (c == '\n' && (i & T_INLCR)) c = '\r';
    if (i & T_IXON) {
        if (is_cc(p, VSTOP, c)) { p->stopped = 1; return 0; }
        if (is_cc(p, VSTART, c)) { p->stopped = 0; return 0; }
        if (p->stopped && (i & T_IXANY)) p->stopped = 0;
    }
    if (l & T_ISIG) {
        int s = is_cc(p, VINTR, c) ? 2 : is_cc(p, VQUIT, c) ? 3 : is_cc(p, VSUSP, c) ? 20 : 0;
        if (s) {
            if (!(l & T_NOFLSH)) { rq_flush(p); p->ocount = 0; }
            echo_char(p, c);
            *sig = s;
            return 0;
        }
    }
    if (canon) {
        if ((l & T_IEXTEN) && is_cc(p, VLNEXT, c)) {
            p->lnext = 1;
            if ((l & T_ECHO) && (l & T_ECHOCTL)) { out_post(p, '^'); out_post(p, '\b'); }
            return 0;
        }
        if (is_cc(p, VERASE, c) || ((l & T_IEXTEN) && is_cc(p, VWERASE, c)) || is_cc(p, VKILL, c)) {
            int word = !is_cc(p, VERASE, c) && !is_cc(p, VKILL, c);
            int kill = is_cc(p, VKILL, c);
            if (kill && (l & T_ECHO) && !(l & T_ECHOKE) && (l & T_ECHOK)) {
                echo_char(p, c); out_post(p, '\n');
            }
            int seen_word = 0;
            while (rq_partial(p)) {
                uint8_t last = (uint8_t)rq_at(p, p->rcount - 1);
                int space = last == ' ' || last == '\t';
                if (word) { if (space && seen_word) break; if (!space) seen_word = 1; }
                p->rcount--;
                if ((l & T_ECHO) && ((l & T_ECHOE) || (kill && (l & T_ECHOKE)))) echo_erase1(p, last);
                if (!kill && !word) break;
            }
            return 0;
        }
        if ((l & T_IEXTEN) && is_cc(p, VREPRINT, c)) {
            echo_char(p, c); out_post(p, '\n');
            uint32_t n = rq_partial(p);
            for (uint32_t k = p->rcount - n; k < p->rcount; k++) echo_char(p, (uint8_t)rq_at(p, k));
            return 0;
        }
        if (c == '\n' || is_cc(p, VEOL, c) || ((l & T_IEXTEN) && is_cc(p, VEOL2, c))) {
            if (p->rcount >= RQ_SIZE) return 1;
            rq_put(p, (uint16_t)(c | RQ_EOL));
            if ((l & T_ECHO) || (c == '\n' && (l & T_ECHONL))) echo_char(p, c);
            return 0;
        }
        if (is_cc(p, VEOF, c)) {
            if (p->rcount >= RQ_SIZE) return 1;
            rq_put(p, RQ_EOF);
            return 0;
        }
        /* an ordinary character: one slot stays free for the line's end */
        if (p->rcount >= RQ_SIZE - 1) return p->lines ? 1 : 0;   /* no reader can drain a full partial line: drop */
        rq_put(p, c);
        echo_char(p, c);
        return 0;
    }
    if (p->rcount >= RQ_SIZE) return 1;
    rq_put(p, c);
    echo_char(p, c);
    return 0;
}

/* ---- the id table --------------------------------------------------------------- */

static struct tty* tab_get_locked(uint32_t id) {
    if (!id) return NULL;
    int slot = (int)(id & 0xFF) - 1;
    if (slot < 0 || slot >= TTY_MAX) return NULL;
    struct tty* p = g_tab[slot];
    return (p && p->id == id) ? p : NULL;
}
/* The caller's controlling terminal: alive, and still its session's. */
static struct tty* ctty_locked(const struct task* t) {
    if (!t || !t->sid) return NULL;
    struct tty* p = tab_get_locked(t->ctty_id);
    return (p && p->session && p->session == t->sid) ? p : NULL;
}
static int is_ctty(const struct task* t, const struct tty* p) {
    uint32_t f = spin_lock_irqsave(&g_tab_lock);
    int r = ctty_locked(t) == p;
    spin_unlock_irqrestore(&g_tab_lock, f);
    return r;
}

/* Drop a release's pin; free when nothing holds the tty any more — no end,
 * no release still working on it.  Deciding under the same lock as the
 * counts is what stops a master teardown and the last slave close from both
 * freeing it (or one freeing it under the other). */
static void tty_unpin(struct tty* p) {
    uint32_t f = spin_lock_irqsave(&g_tab_lock);
    int dead = --p->pins == 0 && p->master_open == 0 && p->slave_open == 0;
    if (dead) {
        g_tab[p->slot] = NULL;
        g_gen[p->slot]++;                          /* stale ctty ids now miss */
    }
    spin_unlock_irqrestore(&g_tab_lock, f);
    if (dead) kfree(p);
}

/* ---- devpts --------------------------------------------------------------------- */

static int ptmx_open_hook(void* ctx, struct ofile* o, int noctty) {
    return tty_open_ptmx(ctx, o, noctty);
}
static int slave_open_hook(void* ctx, struct ofile* o, int noctty) {
    return tty_open_slave(ctx, o, noctty);
}
static int ctty_open_hook(void* ctx, struct ofile* o, int noctty) {
    (void)ctx; (void)noctty;
    return tty_open_ctty(o);
}

static struct devpts* devpts_alloc(void) {
    uint32_t f = spin_lock_irqsave(&g_pts_lock);
    struct devpts* d = NULL;
    for (int i = 0; i < DEVPTS_MAX; i++)
        if (!g_pts[i].live) { d = &g_pts[i]; d->live = 1; break; }
    spin_unlock_irqrestore(&g_pts_lock, f);
    if (!d) return NULL;
    for (int i = 0; i < TTY_MAX; i++) d->used[i] = 0;
    d->ptmx.name = "ptmx";
    d->ptmx.kind = DEVFS_CHAR;
    d->ptmx.ctx = d;
    d->ptmx.open = ptmx_open_hook;
    d->ptmx.has_devnum = 1; d->ptmx.dev_major = 5; d->ptmx.dev_minor = 2;
    d->ptmx.ino = 2;                               /* devpts: root 1, ptmx 2, pty N is N+3 */
    return d;
}

int devpts_instance_new(const char* path) {
    struct devpts* d = devpts_alloc();
    if (!d) return -12;
    d->dir = vfs_resolve(path);
    scpy(d->path, path, sizeof d->path);
    struct inode* in = d->dir ? devfs_attach_into(d->dir, &d->ptmx) : NULL;
    if (!in) { d->live = 0; return -12; }
    in->mode = 0666;
    return 0;
}

/* /dev/tty, /dev/ptmx and the machine's /dev/pts. */
static struct devfs_node g_devtty = {
    .name = "tty", .kind = DEVFS_CHAR, .open = ctty_open_hook,
    .has_devnum = 1, .dev_major = 5, .dev_minor = 0,
};
static struct devfs_node g_devptmx = {
    .name = "ptmx", .kind = DEVFS_CHAR, .open = ptmx_open_hook,
    .has_devnum = 1, .dev_major = 5, .dev_minor = 2,
};
void tty_init(void) {
    vfs_mkdir("/dev/pts");
    if (devpts_instance_new("/dev/pts") != 0) { kprintf("tty: /dev/pts could not be made\n"); return; }
    g_devptmx.ctx = &g_pts[0];                    /* the first instance is the machine's */
    devfs_register(&g_devtty);
    devfs_register(&g_devptmx);
    struct inode* in = devfs_node_inode(&g_devptmx);
    if (in) in->mode = 0666;
    in = devfs_node_inode(&g_devtty);
    if (in) in->mode = 0666;
}

/* ---- opening ---------------------------------------------------------------------- */

static void make_ctty(struct task* me, struct tty* p) {
    /* caller holds nothing; table lock for the session fields */
    uint32_t f = spin_lock_irqsave(&g_tab_lock);
    if (me && me->sid && me->pid == me->sid && !ctty_locked(me) && !p->session) {
        p->session = me->sid;
        p->pgrp = me->pgid ? me->pgid : me->pid;
        me->ctty_id = p->id;
    }
    spin_unlock_irqrestore(&g_tab_lock, f);
}

int tty_open_ptmx(void* inst, struct ofile* o, int noctty) {
    (void)noctty;                                  /* the master never becomes a ctty */
    struct devpts* d = (struct devpts*)inst;
    if (!d || !d->live || !o) return -E_NXIO;
    struct tty* p = (struct tty*)kcalloc(1, sizeof *p);
    if (!p) return -12;
    waitq_init(&p->wq);
    termios_defaults(&p->t, 0);
    termios_defaults(&p->mt, 1);
    p->locked = 1;                                 /* until TIOCSPTLCK(0): unlockpt */
    p->inst = d;
    struct task* me = task_current();
    int uid = me ? cred_uid(&me->cred) : 0;
    p->owner_uid = uid < 0 ? 0 : uid;

    /* a slot in the machine's table and an index in the instance */
    uint32_t f = spin_lock_irqsave(&g_tab_lock);
    int slot = -1, idx = -1;
    for (int i = 0; i < TTY_MAX; i++) if (!g_tab[i]) { slot = i; break; }
    for (int i = 0; i < TTY_MAX && slot >= 0; i++) if (!d->used[i]) { idx = i; break; }
    if (slot >= 0 && idx >= 0) {
        g_tab[slot] = p;
        d->used[idx] = 1;
        p->slot = slot; p->index = idx;
        p->id = ((g_gen[slot] & 0xFFFFFF) << 8) | (uint32_t)(slot + 1);
        p->master_open = 1;
    }
    spin_unlock_irqrestore(&g_tab_lock, f);
    if (slot < 0 || idx < 0) { kfree(p); return -28; }   /* ENOSPC: no pty free */

    /* /dev/pts/N as the opener's namespace names it: next to the ptmx it
     * opened, or the machine's /dev/pts for /dev/ptmx */
    char mp[112];
    if (o->file && vfs_file_path(o->file, mp, sizeof mp) == 0 && d != &g_pts[0]) {
        size_t n = 0, cut = 0;
        while (mp[n]) { if (mp[n] == '/') cut = n; n++; }
        mp[cut] = 0;
        scpy(p->spath, cut ? mp : "", sizeof p->spath);
    } else scpy(p->spath, d->path, sizeof p->spath);
    sapp(p->spath, "/", sizeof p->spath);
    put_dec(p->spath, sizeof p->spath, (unsigned)idx);

    /* the slave node */
    p->sname[0] = 0;
    put_dec(p->sname, sizeof p->sname, (unsigned)idx);
    p->snode.name = p->sname;
    p->snode.kind = DEVFS_CHAR;
    p->snode.ctx = p;
    p->snode.open = slave_open_hook;
    p->snode.has_devnum = 1; p->snode.dev_major = 136; p->snode.dev_minor = (uint32_t)idx;
    p->snode.ino = (uint64_t)idx + 3;
    struct inode* in = d->dir ? devfs_attach_into(d->dir, &p->snode) : NULL;
    if (in) {
        in->owner_uid = p->owner_uid;
        in->owner_gid = 5;                         /* tty */
        in->mode = 0620;
        p->snode_live = 1;
    }
    o->kind = FD_PTY;
    o->tty = p;
    o->tty_side = TTY_MASTER;
    return 0;
}

static int attach_slave(struct tty* p, struct ofile* o, int noctty, int via_ctty) {
    struct task* me = task_current();
    uint32_t f = spin_lock_irqsave(&g_tab_lock);
    int err = 0;
    if (!p->master_open || p->hung) err = -E_IO;
    else if (p->locked && !via_ctty) err = -E_IO;
    else if (p->excl && me && !cred_capable(&me->cred, 21 /* CAP_SYS_ADMIN */)) err = -E_BUSY;
    if (!err) { p->slave_open++; p->slave_ever = 1; }
    spin_unlock_irqrestore(&g_tab_lock, f);
    if (err) return err;
    o->kind = FD_PTY;
    o->tty = p;
    o->tty_side = TTY_SLAVE;
    if (!noctty) make_ctty(me, p);
    return 0;
}
int tty_open_slave(void* ctx, struct ofile* o, int noctty) {
    struct tty* p = (struct tty*)ctx;
    /* the node can outlive nothing: it is removed at the last master close,
     * and the table is the authority on whether `p` is alive */
    uint32_t f = spin_lock_irqsave(&g_tab_lock);
    int alive = p && tab_get_locked(p->id) == p;
    spin_unlock_irqrestore(&g_tab_lock, f);
    if (!alive) return -E_IO;
    return attach_slave(p, o, noctty, 0);
}
int tty_open_ctty(struct ofile* o) {
    struct task* me = task_current();
    uint32_t f = spin_lock_irqsave(&g_tab_lock);
    struct tty* p = ctty_locked(me);
    spin_unlock_irqrestore(&g_tab_lock, f);
    if (!p) return -E_NXIO;                        /* no controlling terminal */
    return attach_slave(p, o, 1, 1);
}

/* ---- signals ------------------------------------------------------------------------ */

/* After the lock: the foreground group, and on a hangup the session leader. */
static void send_fg(int pgrp, int sig) { if (pgrp && sig) lnx_sig_pgrp(pgrp, sig, 0); }
static void hangup_session(struct tty* p) {
    uint32_t f = spin_lock_irqsave(&g_tab_lock);
    int sess = p->session, pg = p->pgrp;
    p->session = 0; p->pgrp = 0;
    spin_unlock_irqrestore(&g_tab_lock, f);
    if (sess) {
        struct task* lead = task_find(sess);
        if (lead) { lnx_sig_post(lead, 1 /* SIGHUP */, NULL); lnx_sig_pgrp(sess, 18 /* SIGCONT */, 0); }
    }
    if (pg && pg != sess) send_fg(pg, 1);
}

/* A signal that interrupts a blocking read or write. */
static int interrupted(struct task* me) {
    if (!me) return 0;
    if (task_should_stop()) return 1;
    return lnx_sig_deliverable(me);
}

/* Job control: may `me` (a member of a background group) touch its ctty `p`?
 * 0 = yes; otherwise the error to return (the signal has been sent). */
static int bg_check(struct tty* p, int sig) {
    struct task* me = task_current();
    if (!me || !is_ctty(me, p)) return 0;
    int mypg = me->pgid ? me->pgid : 0;
    if (!p->pgrp || mypg == p->pgrp) return 0;
    if (lnx_sig_ignored_or_blocked(me, sig)) return sig == 21 ? -E_IO : 0;
    lnx_sig_pgrp(mypg, sig, 0);
    return -E_INTR;
}

/* ---- release ----------------------------------------------------------------------- */

void tty_release(struct ofile* o) {
    struct tty* p = o ? o->tty : NULL;
    if (!p) return;
    o->tty = NULL;
    uint32_t f = spin_lock_irqsave(&g_tab_lock);
    p->pins++;
    int last = 0;
    if (o->tty_side == TTY_MASTER) last = --p->master_open == 0;
    else p->slave_open--;
    spin_unlock_irqrestore(&g_tab_lock, f);

    uint32_t w = waitq_lock(&p->wq);
    if (last) p->hung = 1;
    waitq_wake_all(&p->wq);
    waitq_unlock(&p->wq, w);
    fd_readiness_changed(NULL);
    if (last) {
        hangup_session(p);
        /* the name goes with the master (Linux's devpts_pty_kill) */
        if (p->snode_live && p->inst && p->inst->dir) devfs_detach_from(p->inst->dir, &p->snode);
        p->snode_live = 0;
        uint32_t g = spin_lock_irqsave(&g_tab_lock);
        if (p->inst) p->inst->used[p->index] = 0;
        spin_unlock_irqrestore(&g_tab_lock, g);
    }
    tty_unpin(p);
}

/* ---- read / write -------------------------------------------------------------------- */

static long slave_read(struct tty* p, uint8_t* buf, size_t n, int block) {
    struct task* me = task_current();
    int e = bg_check(p, 21 /* SIGTTIN */);
    if (e) return e;
    size_t got = 0;
    uint32_t vtime_left = 0;
    int first = 1;
    uint32_t f = waitq_lock(&p->wq);
    for (;;) {
        int canon = (p->t.c_lflag & T_ICANON) != 0;
        uint32_t vmin = p->t.c_cc[VMIN], vtime = p->t.c_cc[VTIME];
        if (first) { vtime_left = vtime * 10; first = 0; }    /* steps of 10 ms */
        if (canon && p->lines) {
            while (got < n && p->rcount) {
                uint16_t v = rq_at(p, 0);
                p->rhead = (p->rhead + 1) % RQ_SIZE;
                p->rcount--;
                if (v & (RQ_EOL | RQ_EOF)) {
                    p->lines--;
                    if (!(v & RQ_EOF)) buf[got++] = (uint8_t)v;
                    break;
                }
                buf[got++] = (uint8_t)v;
            }
            break;
        }
        if (!canon && p->rcount) {
            while (got < n && p->rcount) {
                uint16_t v = rq_at(p, 0);
                p->rhead = (p->rhead + 1) % RQ_SIZE;
                p->rcount--;
                if (v & (RQ_EOL | RQ_EOF)) p->lines--;
                buf[got++] = (uint8_t)v;
            }
            size_t want = vmin < n ? vmin : n;
            if (got >= want || vmin == 0) break;
            if (vtime) vtime_left = vtime * 10;          /* inter-byte timer restarts */
        }
        if (p->hung) break;                              /* end of file */
        if (!canon && vmin == 0 && vtime == 0) break;    /* a poll: whatever is there */
        if (!canon && vtime && got && !p->rcount && vtime_left == 0) break;
        if (!canon && vmin == 0 && vtime && vtime_left == 0) break;
        if (!block) { waitq_unlock(&p->wq, f); return got ? (long)got : -E_AGAIN; }
        if (interrupted(me)) { waitq_unlock(&p->wq, f); return got ? (long)got : -E_INTR; }
        if (!canon && vtime) {                           /* a timed wait, in 10 ms steps */
            waitq_unlock(&p->wq, f);
            task_msleep(10);
            vtime_left = vtime_left > 1 ? vtime_left - 1 : 0;
            f = waitq_lock(&p->wq);
            continue;
        }
        waitq_block(&p->wq);
    }
    waitq_wake_all(&p->wq);                              /* room for the master's writer */
    waitq_unlock(&p->wq, f);
    fd_readiness_changed(NULL);
    return (long)got;
}

static long slave_write(struct tty* p, const uint8_t* buf, size_t n, int block) {
    struct task* me = task_current();
    if (p->t.c_lflag & T_TOSTOP) {
        int e = bg_check(p, 22 /* SIGTTOU */);
        if (e) return e;
    }
    size_t done = 0;
    uint32_t f = waitq_lock(&p->wq);
    while (done < n) {
        if (p->hung) { waitq_unlock(&p->wq, f); return done ? (long)done : -E_IO; }
        if (!p->stopped && oq_room(p) >= 2) {            /* ONLCR may need two */
            out_post(p, buf[done++]);
            continue;
        }
        waitq_wake_all(&p->wq);                          /* let the master drain */
        if (!block) break;
        if (interrupted(me)) break;
        waitq_block(&p->wq);
    }
    waitq_wake_all(&p->wq);
    waitq_unlock(&p->wq, f);
    fd_readiness_changed(NULL);
    if (!done) return !block ? -E_AGAIN : -E_INTR;
    return (long)done;
}

static long master_read(struct tty* p, uint8_t* buf, size_t n, int block) {
    struct task* me = task_current();
    uint32_t f = waitq_lock(&p->wq);
    for (;;) {
        if (p->ocount) {
            size_t k = p->ocount < n ? p->ocount : n;
            for (size_t i = 0; i < k; i++) buf[i] = p->oq[(p->ohead + i) % OQ_SIZE];
            p->ohead = (p->ohead + (uint32_t)k) % OQ_SIZE;
            p->ocount -= (uint32_t)k;
            waitq_wake_all(&p->wq);                      /* a slave writer has room */
            waitq_unlock(&p->wq, f);
            fd_readiness_changed(NULL);
            return (long)k;
        }
        if (p->slave_ever && !p->slave_open) { waitq_unlock(&p->wq, f); return -E_IO; }
        if (!block) { waitq_unlock(&p->wq, f); return -E_AGAIN; }
        if (interrupted(me)) { waitq_unlock(&p->wq, f); return -E_INTR; }
        waitq_block(&p->wq);
    }
}

static long master_write(struct tty* p, const uint8_t* buf, size_t n, int block) {
    struct task* me = task_current();
    size_t done = 0;
    int sig = 0, pg = 0;
    uint32_t f = waitq_lock(&p->wq);
    while (done < n) {
        if (p->slave_ever && !p->slave_open) { waitq_unlock(&p->wq, f); return done ? (long)done : -E_IO; }
        uint8_t c = buf[done];
        /* the master's own output post-processing (off by default) */
        if ((p->mt.c_oflag & T_OPOST) && (p->mt.c_oflag & T_OCRNL) && c == '\r') c = '\n';
        int s = 0;
        if (ldisc_receive(p, c, &s) == 0) {
            done++;
            if (s) { sig = s; pg = p->pgrp; break; }     /* deliver before more input */
            continue;
        }
        waitq_wake_all(&p->wq);                          /* the slave reader drains */
        if (!block) break;
        if (interrupted(me)) break;
        waitq_block(&p->wq);
    }
    waitq_wake_all(&p->wq);
    waitq_unlock(&p->wq, f);
    fd_readiness_changed(NULL);
    if (sig) {
        send_fg(pg, sig);
        /* the rest of the write goes on after the signal, as typed */
        if (done < n) {
            long r = master_write(p, buf + done, n - done, block);
            return r > 0 ? (long)done + r : (long)done;
        }
    }
    if (!done && n) return !block ? -E_AGAIN : -E_INTR;
    return (long)done;
}

long tty_read(struct ofile* o, void* buf, size_t n, int block) {
    struct tty* p = o ? o->tty : NULL;
    if (!p) return -E_IO;
    if (!n) return 0;
    return o->tty_side == TTY_MASTER ? master_read(p, (uint8_t*)buf, n, block)
                                     : slave_read(p, (uint8_t*)buf, n, block);
}
long tty_write(struct ofile* o, const void* buf, size_t n, int block) {
    struct tty* p = o ? o->tty : NULL;
    if (!p) return -E_IO;
    if (!n) return 0;
    return o->tty_side == TTY_MASTER ? master_write(p, (const uint8_t*)buf, n, block)
                                     : slave_write(p, (const uint8_t*)buf, n, block);
}

uint32_t tty_readiness(struct ofile* o) {
    struct tty* p = o ? o->tty : NULL;
    if (!p) return POLLNVAL;
    uint32_t r = 0;
    uint32_t f = waitq_lock(&p->wq);
    if (o->tty_side == TTY_MASTER) {
        if (p->ocount) r |= POLLIN;
        if (p->slave_ever && !p->slave_open) r |= POLLHUP | POLLIN;
        if (p->rcount < RQ_SIZE - 1) r |= POLLOUT;
    } else {
        if (p->hung) r |= POLLIN | POLLOUT | POLLHUP | POLLERR;
        else {
            uint32_t need = (p->t.c_lflag & T_ICANON) ? 1 : (p->t.c_cc[VMIN] ? p->t.c_cc[VMIN] : 1);
            if (rq_avail(p) >= need || ((p->t.c_lflag & T_ICANON) && p->lines)) r |= POLLIN;
            if (!p->stopped && oq_room(p) >= 2) r |= POLLOUT;
        }
    }
    waitq_unlock(&p->wq, f);
    return r;
}

/* ---- ioctl ------------------------------------------------------------------------- */

#define TCGETS      0x5401u
#define TCSETS      0x5402u
#define TCSETSW     0x5403u
#define TCSETSF     0x5404u
#define TCSBRK      0x5409u
#define TCXONC      0x540Au
#define TCFLSH      0x540Bu
#define TIOCEXCL    0x540Cu
#define TIOCNXCL    0x540Du
#define TIOCSCTTY   0x540Eu
#define TIOCGPGRP   0x540Fu
#define TIOCSPGRP   0x5410u
#define TIOCOUTQ    0x5411u
#define TIOCSTI     0x5412u
#define TIOCGWINSZ  0x5413u
#define TIOCSWINSZ  0x5414u
#define FIONREAD    0x541Bu
#define TIOCPKT     0x5420u
#define TIOCNOTTY   0x5422u
#define TCSBRKP     0x5425u
#define TIOCGSID    0x5429u
#define TCGETS2     0x802C542Au
#define TCSETS2     0x402C542Bu
#define TCSETSW2    0x402C542Cu
#define TCSETSF2    0x402C542Du
#define TIOCGPTN    0x80045430u
#define TIOCSPTLCK  0x40045431u
#define TIOCGDEV    0x80045432u
#define TIOCVHANGUP 0x5437u
#define TIOCGPTLCK  0x80045439u
#define TIOCGEXCL   0x80045440u
#define TIOCGPTPEER 0x5441u
#define TIOCGLCKTRMIOS 0x5456u
#define TIOCSLCKTRMIOS 0x5457u

int tty_ioctl_size(unsigned cmd, int* dir) {
    switch (cmd) {
    case TCGETS: case TIOCGLCKTRMIOS:                   *dir = 1; return 36;
    case TCGETS2:                                       *dir = 1; return 44;
    case TIOCGWINSZ:                                    *dir = 1; return 8;
    case TIOCGPTN: case TIOCGPTLCK: case TIOCGDEV: case TIOCGPGRP: case TIOCGSID:
    case FIONREAD: case TIOCOUTQ: case TIOCGEXCL:       *dir = 1; return 4;
    case TCSETS: case TCSETSW: case TCSETSF: case TIOCSLCKTRMIOS: *dir = 2; return 36;
    case TCSETS2: case TCSETSW2: case TCSETSF2:         *dir = 2; return 44;
    case TIOCSWINSZ:                                    *dir = 2; return 8;
    case TIOCSPTLCK: case TIOCSPGRP: case TIOCPKT:      *dir = 2; return 4;
    case TIOCSCTTY: case TIOCNOTTY: case TCFLSH: case TCXONC: case TCSBRK: case TCSBRKP:
    case TIOCEXCL: case TIOCNXCL: case TIOCGPTPEER: case TIOCVHANGUP: case TIOCSTI:
                                                        *dir = 0; return 0;
    default: return -1;
    }
}

static int r32(const void* b) { const uint8_t* p = (const uint8_t*)b; return (int)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24); }
static void w32(void* b, uint32_t v) { uint8_t* p = (uint8_t*)b; p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* A group id the caller names (in its pid namespace) → global; -1 none. */
static int pg_global(struct task* me, int vnr) {
    int g = ns_pid_resolve(me, vnr);
    return g > 0 ? g : -1;
}
static int pg_vnr(struct task* me, int g) {
    struct task* t = g ? task_find(g) : NULL;
    int v = t ? ns_vnr(me, t) : 0;
    return v > 0 ? v : g;
}
struct pg_probe { int pgid, sid, found; };
static void pg_probe_fn(const struct task* t, int cur, void* ctx) {
    (void)cur;
    struct pg_probe* q = (struct pg_probe*)ctx;
    if (t->state != TASK_DEAD && (t->pgid ? t->pgid : 0) == q->pgid && t->sid == q->sid) q->found = 1;
}

long tty_ioctl(struct ofile* o, unsigned cmd, void* k, unsigned long val) {
    struct tty* p = o ? o->tty : NULL;
    if (!p) return -E_IO;
    struct task* me = task_current();
    int master = o->tty_side == TTY_MASTER;
    switch (cmd) {
    case TCGETS: case TCGETS2: {
        uint32_t f = waitq_lock(&p->wq);
        const struct ktermios* t = master ? &p->mt : &p->t;
        for (unsigned i = 0; i < sizeof *t; i++) ((uint8_t*)k)[i] = ((const uint8_t*)t)[i];
        waitq_unlock(&p->wq, f);
        if (cmd == TCGETS2) { w32((uint8_t*)k + 36, 38400); w32((uint8_t*)k + 40, 38400); }
        return 0;
    }
    case TCSETS: case TCSETSW: case TCSETSF: case TCSETS2: case TCSETSW2: case TCSETSF2: {
        if (!master) { int e = bg_check(p, 22); if (e) return e; }
        uint32_t f = waitq_lock(&p->wq);
        struct ktermios* t = master ? &p->mt : &p->t;
        int was_canon = (t->c_lflag & T_ICANON) != 0;
        for (unsigned i = 0; i < sizeof *t; i++) ((uint8_t*)t)[i] = ((const uint8_t*)k)[i];
        if (!master) {
            int canon = (t->c_lflag & T_ICANON) != 0;
            if (cmd == TCSETSF || cmd == TCSETSF2) rq_flush(p);
            else if (canon != was_canon) rq_recount(p);
            if (!(t->c_iflag & T_IXON)) p->stopped = 0;
        }
        waitq_wake_all(&p->wq);
        waitq_unlock(&p->wq, f);
        fd_readiness_changed(NULL);
        return 0;
    }
    case TIOCGWINSZ: {
        uint32_t f = waitq_lock(&p->wq);
        for (int i = 0; i < 4; i++) { ((uint8_t*)k)[2 * i] = (uint8_t)p->ws[i]; ((uint8_t*)k)[2 * i + 1] = (uint8_t)(p->ws[i] >> 8); }
        waitq_unlock(&p->wq, f);
        return 0;
    }
    case TIOCSWINSZ: {
        int changed = 0;
        uint32_t f = waitq_lock(&p->wq);
        for (int i = 0; i < 4; i++) {
            uint16_t v = (uint16_t)(((uint8_t*)k)[2 * i] | ((uint8_t*)k)[2 * i + 1] << 8);
            if (p->ws[i] != v) { p->ws[i] = v; changed = 1; }
        }
        int pg = p->pgrp;
        waitq_unlock(&p->wq, f);
        if (changed) send_fg(pg, 28 /* SIGWINCH */);
        return 0;
    }
    case TIOCGPTN:   if (!master) return -E_NOTTY; w32(k, (uint32_t)p->index); return 0;
    case TIOCSPTLCK: if (!master) return -E_NOTTY; p->locked = r32(k) != 0; return 0;
    case TIOCGPTLCK: if (!master) return -E_NOTTY; w32(k, (uint32_t)p->locked); return 0;
    case TIOCGDEV:   w32(k, (136u << 8) | ((uint32_t)p->index & 0xFF) | (((uint32_t)p->index & ~0xFFu) << 12)); return 0;
    case TIOCEXCL:   p->excl = 1; return 0;
    case TIOCNXCL:   p->excl = 0; return 0;
    case TIOCGEXCL:  w32(k, (uint32_t)p->excl); return 0;
    case TIOCGLCKTRMIOS:
        for (int i = 0; i < 36; i++) ((uint8_t*)k)[i] = 0;
        return 0;
    case TIOCSLCKTRMIOS:
        return me && cred_capable(&me->cred, 21) ? 0 : -E_PERM;   /* accepted, not enforced */
    case TIOCPKT:
        if (r32(k) == 0) return 0;
        kprintf("tty: TIOCPKT (packet mode) refused - not implemented\n");
        return -E_INVAL;
    case TIOCSTI:
        return -E_IO;                                  /* as Linux with legacy_tiocsti off */
    case FIONREAD: {
        uint32_t f = waitq_lock(&p->wq);
        uint32_t v = master ? p->ocount : rq_avail(p);
        if (!master && (p->t.c_lflag & T_ICANON) && v && (rq_at(p, v - 1) & RQ_EOF)) v--;
        waitq_unlock(&p->wq, f);
        w32(k, v);
        return 0;
    }
    case TIOCOUTQ: {
        uint32_t f = waitq_lock(&p->wq);
        uint32_t v = master ? 0 : p->ocount;
        waitq_unlock(&p->wq, f);
        w32(k, v);
        return 0;
    }
    case TCFLSH: {
        if (val > 2) return -E_INVAL;
        uint32_t f = waitq_lock(&p->wq);
        int in = val == 0 || val == 2, out = val == 1 || val == 2;
        if (master) { int t = in; in = out; out = t; }   /* the master's input is the slave's output */
        if (in) rq_flush(p);
        if (out) p->ocount = 0;
        waitq_wake_all(&p->wq);
        waitq_unlock(&p->wq, f);
        fd_readiness_changed(NULL);
        return 0;
    }
    case TCXONC: {
        uint32_t f = waitq_lock(&p->wq);
        if (val == 0) p->stopped = 1;                  /* TCOOFF */
        else if (val == 1) p->stopped = 0;             /* TCOON  */
        waitq_wake_all(&p->wq);
        waitq_unlock(&p->wq, f);
        return val <= 3 ? 0 : -E_INVAL;
    }
    case TCSBRK: case TCSBRKP:
        return 0;                                      /* no line to break, no drain on a pty */
    case TIOCSCTTY: {
        if (master) return -E_NOTTY;
        if (!me || !me->sid || me->pid != me->sid) return -E_PERM;   /* only a session leader */
        uint32_t f = spin_lock_irqsave(&g_tab_lock);
        int err = 0;
        if (ctty_locked(me) == p) err = 0;
        else if (ctty_locked(me)) err = -E_PERM;
        else if (p->session && p->session != me->sid && !(val == 1 && cred_capable(&me->cred, 21))) err = -E_PERM;
        else {
            p->session = me->sid;
            p->pgrp = me->pgid ? me->pgid : me->pid;
            me->ctty_id = p->id;
        }
        spin_unlock_irqrestore(&g_tab_lock, f);
        return err;
    }
    case TIOCNOTTY: {
        if (master || !is_ctty(me, p)) return -E_NOTTY;
        if (me->pid == me->sid) hangup_session(p);
        me->ctty_id = 0;
        return 0;
    }
    case TIOCGPGRP: {
        if (!master && !is_ctty(me, p)) return -E_NOTTY;
        int g = p->pgrp;
        w32(k, (uint32_t)(g ? pg_vnr(me, g) : 0));
        return 0;
    }
    case TIOCSPGRP: {
        if (master || !is_ctty(me, p)) return -E_NOTTY;
        int e = bg_check(p, 22);
        if (e) return e;
        int v = r32(k);
        if (v <= 0) return -E_INVAL;
        int g = pg_global(me, v);
        if (g < 0) return -E_PERM;
        struct pg_probe q = { g, p->session, 0 };
        task_for_each(pg_probe_fn, &q);
        if (!q.found) return -E_PERM;                  /* not a group of this session */
        p->pgrp = g;
        return 0;
    }
    case TIOCGSID: {
        if (!master && !is_ctty(me, p)) return -E_NOTTY;
        if (!p->session) return -E_NOTTY;
        w32(k, (uint32_t)pg_vnr(me, p->session));
        return 0;
    }
    case TIOCVHANGUP: {
        if (!me || !cred_capable(&me->cred, 21)) return -E_PERM;
        uint32_t f = waitq_lock(&p->wq);
        p->hung = 1;
        waitq_wake_all(&p->wq);
        waitq_unlock(&p->wq, f);
        hangup_session(p);
        fd_readiness_changed(NULL);
        return 0;
    }
    case TIOCGPTPEER: {
        /* open the slave through its name, as the opener's namespace has it */
        if (!master) return -E_NOTTY;
        long fd = sys_open_ex_k(p->spath, VFS_RDWR | ((val & 0400) ? VFS_NOCTTY : 0),
                                (val & 04000) != 0, 0);
        return fd < 0 ? -E_IO : fd;
    }
    default:
        return -E_NOTTY;
    }
}

/* ---- naming ------------------------------------------------------------------------- */

int tty_name(struct ofile* o, char* out, size_t cap) {
    struct tty* p = o ? o->tty : NULL;
    if (!p || cap < 2) return -1;
    scpy(out, o->tty_side == TTY_MASTER ? "/dev/ptmx" : p->spath, cap);
    return 0;
}
uint32_t tty_devnum(struct ofile* o, int* is_master) {
    struct tty* p = o ? o->tty : NULL;
    if (is_master) *is_master = o && o->tty_side == TTY_MASTER;
    if (!p) return 0;
    if (o->tty_side == TTY_MASTER) return (5u << 8) | 2u;
    return (136u << 8) | ((uint32_t)p->index & 0xFF) | (((uint32_t)p->index & ~0xFFu) << 12);
}
uint32_t tty_ctty_devnum(struct task* t) {
    uint32_t f = spin_lock_irqsave(&g_tab_lock);
    struct tty* p = ctty_locked(t);
    uint32_t v = p ? ((136u << 8) | ((uint32_t)p->index & 0xFF) | (((uint32_t)p->index & ~0xFFu) << 12)) : 0;
    spin_unlock_irqrestore(&g_tab_lock, f);
    return v;
}

/* ---- process lifecycle ----------------------------------------------------------------- */

void tty_task_exit(struct task* t) {
    if (!t || !t->ctty_id || t->pid != t->sid) return;
    uint32_t f = spin_lock_irqsave(&g_tab_lock);
    struct tty* p = ctty_locked(t);
    spin_unlock_irqrestore(&g_tab_lock, f);
    if (p) hangup_session(p);                          /* the session lost its terminal */
}
