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
#include "printf.h"
#include "devfs.h"
#include <stddef.h>

#define CLIP_MAX (64 * 1024)

static spinlock_t clip_lock;            /* zero-initialized = unlocked */
static char* clip_buf = NULL;
static int   clip_cap = 0;
static int   clip_len = 0;

/* §M59 — A TYPED OFFER.  What is on a clipboard is not just bytes: a paste
 * target has to be able to ask "what IS this?" and decline what it cannot use.
 * Every windowing system that skipped this ended up guessing from the content,
 * which fails silently on exactly the interesting cases (a PNG that happens to
 * start with printable bytes, UTF-8 vs. a byte blob).
 *
 * One type per slot, a short MIME-shaped string, defaulting to text/plain —
 * so nothing that exists today has to say anything, and anything that carries
 * something else can. */
#define CLIP_TYPE_MAX 32
static char clip_type[CLIP_TYPE_MAX]  = "text/plain";
static char prim_type[CLIP_TYPE_MAX]  = "text/plain";

static void type_copy(char* dst, const char* src) {
    int i = 0;
    if (!src || !*src) src = "text/plain";
    for (; src[i] && i < CLIP_TYPE_MAX - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

int clipboard_set_typed(const char* text, int len, const char* type) {
    int rc = clipboard_set(text, len);
    if (rc == 0) {
        uint32_t fl = spin_lock_irqsave(&clip_lock);
        type_copy(clip_type, type);
        spin_unlock_irqrestore(&clip_lock, fl);
    }
    return rc;
}

int clipboard_set_primary_typed(const char* text, int len, const char* type) {
    int rc = clipboard_set_primary(text, len);
    if (rc == 0) {
        uint32_t fl = spin_lock_irqsave(&clip_lock);
        type_copy(prim_type, type);
        spin_unlock_irqrestore(&clip_lock, fl);
    }
    return rc;
}

const char* clipboard_type(void)         { return clip_type; }
const char* clipboard_primary_type(void) { return prim_type; }

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

/* ------------------------------------------------------------------- */
/* §M59 — the PRIMARY selection: a second, independent slot.            */
/*                                                                      */
/* X11 got this right by accident and everyone copied it since: the     */
/* thing you SELECTED and the thing you deliberately COPIED are two     */
/* different intentions, and folding them into one slot means every     */
/* selection destroys whatever the user had explicitly copied.  Two     */
/* slots, same shape, and the paste sites choose: Ctrl+V takes the      */
/* explicit clipboard, middle-click takes the primary.                  */
/*                                                                      */
/* Storage is deliberately a straight copy of the clipboard code rather */
/* than a shared parametrised core: at this size the duplication is     */
/* four short functions, while the parametrised version would need a    */
/* slot object with its own lifetime — and §M59's real answer (typed    */
/* offers, ownership, fd hand-off for Wayland) replaces both anyway.    */
/* ------------------------------------------------------------------- */

static spinlock_t prim_lock;
static char* prim_buf = NULL;
static int   prim_cap = 0;
static int   prim_len = 0;

int clipboard_set_primary(const char* text, int len) {
    if (!text) len = 0;
    if (len < 0) { len = 0; while (text[len]) len++; }
    if (len > CLIP_MAX) len = CLIP_MAX;

    char* nbuf = NULL;
    if (len > 0 && len > prim_cap) {
        nbuf = (char*)kmalloc((size_t)len);
        if (!nbuf) return -1;
    }
    char* old = NULL;
    uint32_t fl = spin_lock_irqsave(&prim_lock);
    if (nbuf) { old = prim_buf; prim_buf = nbuf; prim_cap = len; }
    if (len > prim_cap) len = prim_cap;
    for (int i = 0; i < len; i++) prim_buf[i] = text[i];
    prim_len = len;
    spin_unlock_irqrestore(&prim_lock, fl);
    if (old) kfree(old);
    return 0;
}

int clipboard_get_primary(char* dst, int cap) {
    if (!dst || cap <= 0) return 0;
    uint32_t fl = spin_lock_irqsave(&prim_lock);
    int n = prim_len < cap - 1 ? prim_len : cap - 1;
    for (int i = 0; i < n; i++) dst[i] = prim_buf[i];
    spin_unlock_irqrestore(&prim_lock, fl);
    dst[n] = 0;
    return n;
}

int clipboard_primary_len(void) { return prim_len; }

/* ------------------------------------------------------------------- */
/* §M59 — /dev/clipboard: the clipboard as a FILE.                      */
/*                                                                      */
/* `cat file > /dev/clipboard` and `cat /dev/clipboard` are the whole    */
/* point: a clipboard that only GUI widgets can reach is not a system    */
/* clipboard, and everything in this system that moves bytes already     */
/* knows how to talk to a file.  It also gives ring 3 a surface for      */
/* free — a musl program opens /dev/clipboard like any other device, so  */
/* the §M50 syscall work is not a prerequisite for using it.             */
/*                                                                      */
/* Semantics chosen to match what a shell user expects rather than what  */
/* is easiest: a WRITE at offset 0 REPLACES the contents (a redirection  */
/* is a new document, not an append), a write at a non-zero offset       */
/* appends (so `cat` of a large file through several write() calls       */
/* assembles one clipboard entry), and a read is a plain byte range.     */
/* ------------------------------------------------------------------- */

static ssize_t clip_dev_read(void* ctx, void* buf, size_t n, uint64_t off) {
    (void)ctx;
    if (!buf) return -1;
    uint32_t fl = spin_lock_irqsave(&clip_lock);
    int len = clip_len;
    if ((uint64_t)len <= off) { spin_unlock_irqrestore(&clip_lock, fl); return 0; }
    size_t avail = (size_t)(len - (int)off);
    if (n > avail) n = avail;
    for (size_t i = 0; i < n; i++) ((char*)buf)[i] = clip_buf[off + i];
    spin_unlock_irqrestore(&clip_lock, fl);
    return (ssize_t)n;
}

static ssize_t clip_dev_write(void* ctx, const void* buf, size_t n, uint64_t off) {
    (void)ctx;
    if (!buf) return -1;
    if (off == 0) {
        if (clipboard_set((const char*)buf, (int)n) != 0) return -1;
        return (ssize_t)n;
    }
    /* Append: read-modify-write through the public setter so the growth and
     * locking rules stay in ONE place rather than being reimplemented here. */
    int have = clipboard_len();
    int add  = (int)n;
    if (have + add > CLIP_MAX) add = CLIP_MAX - have;
    if (add <= 0) return 0;
    char* tmp = (char*)kmalloc((size_t)have + (size_t)add);
    if (!tmp) return -1;
    clipboard_get(tmp, have + 1);
    for (int i = 0; i < add; i++) tmp[have + i] = ((const char*)buf)[i];
    int rc = clipboard_set(tmp, have + add);
    kfree(tmp);
    return rc == 0 ? (ssize_t)add : -1;
}

/* ioctl on /dev/clipboard — the TYPE, readable and writable by ring 3 without
 * a syscall of its own.  Deliberately on the device rather than as a new
 * syscall number: the file works for BOTH personalities (a Linux-ABI binary has
 * no d-os syscall numbers, and there is no Linux clipboard call to borrow), so
 * the device is the only surface that reaches every program we can run. */
#define CLIP_IOC_GET_TYPE 0x4301
#define CLIP_IOC_SET_TYPE 0x4302

static int clip_dev_ioctl(void* ctx, int cmd, void* arg) {
    (void)ctx;
    if (!arg) return -1;
    if (cmd == CLIP_IOC_GET_TYPE) {
        const char* t = clipboard_type();
        char* out = (char*)arg;                 /* caller supplies >= 32 bytes */
        int i = 0;
        for (; t[i] && i < CLIP_TYPE_MAX - 1; i++) out[i] = t[i];
        out[i] = 0;
        return i;
    }
    if (cmd == CLIP_IOC_SET_TYPE) {
        uint32_t fl = spin_lock_irqsave(&clip_lock);
        type_copy(clip_type, (const char*)arg);
        spin_unlock_irqrestore(&clip_lock, fl);
        return 0;
    }
    return -1;
}

static struct devfs_node clip_dev = {
    .name = "clipboard", .kind = DEVFS_CHAR,
    .read = clip_dev_read, .write = clip_dev_write,
    .ioctl = clip_dev_ioctl, .ctx = NULL,
};

void clipboard_devfs_init(void) { devfs_register(&clip_dev); }

/* ------------------------------------------------------------------- */
/* `clip` — the shell view of both slots.  Here rather than in a shell  */
/* so the ARM serial REPL gets it too (§M24's rule).                    */
/* ------------------------------------------------------------------- */

static int streq_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static const char* word_(const char* s, char* out, int cap) {
    int n = 0;
    while (*s == ' ') s++;
    while (*s && *s != ' ' && n < cap - 1) out[n++] = *s++;
    out[n] = 0;
    while (*s == ' ') s++;
    return s;
}

void clipboard_cmd(const char* args) {
    char cmd[16];
    const char* rest = word_(args ? args : "", cmd, sizeof cmd);

    if (!cmd[0] || streq_(cmd, "show") || streq_(cmd, "status")) {
        kprintf("clipboard: %d byte(s) [%s]   primary (selection): %d byte(s) [%s]\n",
                clipboard_len(), clipboard_type(),
                clipboard_primary_len(), clipboard_primary_type());
        kprintf("  clip paste [primary] | clip copy <text> | clip promote\n");
        kprintf("  in a GUI terminal: DRAG selects, Ctrl+Shift+C copies,\n"
                "  Ctrl+Shift+V (or middle-click) pastes\n");
        return;
    }

    if (streq_(cmd, "paste")) {
        char which[16];
        word_(rest, which, sizeof which);
        int prim = which[0] && (which[0] == 'p');
        int n = prim ? clipboard_primary_len() : clipboard_len();
        if (n <= 0) { kprintf("clip: %s is empty\n", prim ? "primary" : "clipboard"); return; }
        char* buf = (char*)kmalloc((size_t)n + 1);
        if (!buf) { kprintf("clip: OOM\n"); return; }
        n = prim ? clipboard_get_primary(buf, n + 1) : clipboard_get(buf, n + 1);
        /* Printed with an explicit terminator on its own line: pasted terminal
         * text usually ends WITHOUT a newline, and without the marker the next
         * prompt lands mid-line and the paste looks truncated. */
        kprintf("--- %s (%d bytes) ---\n%s\n--- end ---\n",
                prim ? "primary" : "clipboard", n, buf);
        kfree(buf);
        return;
    }

    if (streq_(cmd, "copy")) {
        if (!*rest) { kprintf("clip: copy <text>\n"); return; }
        if (clipboard_set(rest, -1) == 0) kprintf("clip: copied %d byte(s)\n",
                                                  clipboard_len());
        else                              kprintf("clip: OOM\n");
        return;
    }

    if (streq_(cmd, "type")) {
        /* `clip type` reports the offer's type; `clip type <t>` sets it.  The
         * setter is here so the typed path can be exercised without a program:
         * a feature that only a not-yet-written client can reach is a feature
         * nobody has tested. */
        if (!*rest) {
            kprintf("clipboard type: %s   primary: %s\n",
                    clipboard_type(), clipboard_primary_type());
            return;
        }
        int n = clipboard_len();
        char* buf = (char*)kmalloc((size_t)n + 1);
        if (!buf) { kprintf("clip: OOM\n"); return; }
        n = clipboard_get(buf, n + 1);
        clipboard_set_typed(buf, n, rest);
        kfree(buf);
        kprintf("clipboard type: %s\n", clipboard_type());
        return;
    }

    if (streq_(cmd, "promote")) {
        /* Selection → clipboard, i.e. "I meant that one".  The two slots exist
         * precisely so this is a DECISION and not a side effect. */
        int n = clipboard_primary_len();
        if (n <= 0) { kprintf("clip: primary is empty\n"); return; }
        char* buf = (char*)kmalloc((size_t)n + 1);
        if (!buf) { kprintf("clip: OOM\n"); return; }
        n = clipboard_get_primary(buf, n + 1);
        clipboard_set(buf, n);
        kfree(buf);
        kprintf("clip: primary → clipboard (%d bytes)\n", n);
        return;
    }

    kprintf("clip: show | paste [primary] | copy <text> | type [mime] | promote\n");
}
