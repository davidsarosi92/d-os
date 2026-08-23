/* =============================================================================
 * dosgui.c — d-os display bridge for ring-3 graphical clients (§M42).
 *
 * See dosgui.h for the contract.  A client (NetSurf's libnsfb "dos" surface)
 * creates a WM-managed WIN_APP window, renders into its own buffer, and pushes
 * frames in with gui_window_blit — the same present path the §M26 Wayland
 * bridge uses.  Input is collected via the window's input hook into a small
 * per-window ring the client drains with dosgui_poll.
 *
 * Concurrency: the client runs on its own ring-3 task; the input hook fires
 * from the compositor's input router.  The ring is guarded by a spinlock so the
 * two never corrupt head/tail.
 * ============================================================================= */
#include "dosgui.h"
#include "gui.h"
#include "task.h"
#include "lock.h"
#include "vmm.h"                 /* vmm_user_access_ok — validate the client px */
#include "ui.h"                  /* §M65 — the shared toolkit, built from a blob */
#include "kmalloc.h"
#include <stddef.h>
#include <stdint.h>

/* A handful of concurrent bridge windows is plenty (one per browser). */
#define DOSGUI_MAX      4
#define DOSGUI_EVQ      128

struct dosgui_win {
    int                used;
    int                owner_pid;   /* the ring-3 client that created it (§audit#4) */
    struct gui_window* win;
    /* The content size this client has been TOLD about.  A window's size is
     * changed by the user through the WM — the client is not consulted and
     * cannot poll for it — so the bridge remembers what it last reported and
     * raises a RESIZE when the two differ. */
    int                rep_w, rep_h;
    /* Input ring (single-producer compositor, single-consumer client). */
    struct dosgui_event evq[DOSGUI_EVQ];
    volatile int       head, tail;
    spinlock_t         lock;
};

static struct dosgui_win g_dg[DOSGUI_MAX];

/* §audit#4 — a handle may only be used by the process that created it, so one
 * package can't blit into / poll / destroy another's window via its handle.
 * Returns the window iff `handle` is valid AND owned by the caller, else NULL. */
static struct dosgui_win* dosgui_owned(int handle) {
    if (handle < 0 || handle >= DOSGUI_MAX || !g_dg[handle].used) return NULL;
    struct task* me = task_current();
    if (!me || g_dg[handle].owner_pid != me->pid) return NULL;
    return &g_dg[handle];
}

/* Compositor-side input sink: translate a gui_input into a dosgui_event and
 * enqueue it (dropping the oldest if the ring is full). */
static void dosgui_input_cb(struct gui_window* w, const struct gui_input* in, void* ctx) {
    (void)w;
    struct dosgui_win* d = (struct dosgui_win*)ctx;
    if (!d || !in) return;
    spin_lock(&d->lock);
    int nt = (d->tail + 1) % DOSGUI_EVQ;
    if (nt != d->head) {                        /* space available */
        struct dosgui_event* e = &d->evq[d->tail];
        e->type    = (int32_t)in->type;
        e->keycode = (int32_t)in->keycode;
        e->pressed = (int32_t)in->pressed;
        e->x       = (int32_t)in->x;
        e->y       = (int32_t)in->y;
        e->ch      = (int32_t)in->ch;
        d->tail = nt;
    }
    spin_unlock(&d->lock);
}

/* §M54 — the compositor has disposed this window (by ANY route: the client's
 * own DOSGUI_DESTROY, the title-bar X, or the client dying without releasing
 * anything).  Free the handle and drop the pointer.
 *
 * Before this existed, a handle was released only by dosgui_destroy — a call
 * that a CRASHED client never makes.  So every browser crash burned one of the
 * four handles permanently and left `win` pointing at a window struct the
 * compositor had already recycled for somebody else.  After four crashes
 * NetSurf simply could not open a window any more, and the reason was invisible
 * from the outside.
 *
 * Runs on the compositor task while the client (if any) is already gone, so the
 * ring's lock is not needed for the fields written here — but taking it costs
 * nothing and keeps the rule "every mutation of a slot is under its lock". */
static void dosgui_disposed_cb(struct gui_window* w, void* ctx) {
    (void)w;
    struct dosgui_win* d = (struct dosgui_win*)ctx;
    if (!d) return;
    spin_lock(&d->lock);
    d->win       = NULL;
    d->used      = 0;
    d->owner_pid = 0;
    d->head = d->tail = 0;
    spin_unlock(&d->lock);
}

int dosgui_create(int w, int h, const char* title) {
    if (w <= 0 || h <= 0) return -1;
    int handle = -1;
    for (int i = 0; i < DOSGUI_MAX; i++) {
        if (!g_dg[i].used) { handle = i; break; }
    }
    if (handle < 0) return -1;

    struct dosgui_win* d = &g_dg[handle];
    /* A WIN_APP surface window (bare pixel surface, no hosted shell) — the same
     * kind the Wayland bridge targets.  Needs the compositor to be running. */
    d->win = gui_app_window_create(title ? title : "NetSurf",
                                   60, 40, w, h, NULL, NULL);
    if (!d->win) return -1;

    d->head = d->tail = 0;
    d->rep_w = w; d->rep_h = h;          /* the size it asked for is the size
                                          * it already knows about            */
    spin_lock_init(&d->lock);
    gui_window_set_input_hook(d->win, dosgui_input_cb, d);
    /* §M54 — arm the disposal notification BEFORE the window can be disposed,
     * so there is no window of time in which the compositor could tear it down
     * without telling us. */
    gui_window_set_dispose_cb(d->win, dosgui_disposed_cb, d);

    /* Detach the window from the app-host reap machinery.  gui_app_window_create
     * bound the window to THIS task (the ring-3 client that issued DOSGUI_CREATE)
     * as its host_task, and the compositor's WIN_APP teardown (apply_pending)
     * would then read host_task->state after the task dies and reap it — on the
     * premise (true for in-kernel app-hosts) that the host is reap_owned so init
     * won't touch it.  A dosgui client (NetSurf) is a DETACHED task reaped by
     * init, so that premise is FALSE: init frees/recycles the struct while the
     * compositor still reads host_task->state and reap_gui_host()s a now-stale /
     * recycled pid → task-table corruption → the GUI wedges on the NEXT open.
     *
     * Instead this is a CLIENT-MANAGED window: host_task is cleared so the
     * compositor never reads or reaps the client task (init owns that), and the
     * disposal is driven purely by the client's DOSGUI_DESTROY (dosgui_destroy →
     * gui_window_client_release), never by observing the task's death. */
    gui_window_set_client_managed(d->win, task_current() ? task_current()->pid : 0);

    d->owner_pid = task_current() ? task_current()->pid : 0;
    d->used = 1;
    return handle;
}

int dosgui_present(int handle, const uint32_t* px, int w, int h, int stride) {
    struct dosgui_win* d = dosgui_owned(handle);      /* §audit#4 — ownership */
    if (!d) return -1;
    /* §audit#5 — the client controls px/w/h/stride; gui_window_blit reads
     * [px .. px + ((h-1)*stride + w) pixels].  Bound the geometry to sane
     * limits and VALIDATE the whole source range is mapped + user-readable in
     * the caller's address space BEFORE the compositor reads it — otherwise an
     * oversized stride/h reads far past the client buffer into unmapped memory
     * (a ring-0 #PF → whole-box halt) or kernel memory (info leak). */
    if (!px || w <= 0 || h <= 0 || stride < w) return -1;
    if (w > 8192 || h > 8192 || stride > 16384) return -1;
    uintptr_t bytes = ((uintptr_t)(h - 1) * (uintptr_t)stride + (uintptr_t)w)
                      * sizeof(uint32_t);
    if (!vmm_user_access_ok((uintptr_t)px, bytes, 0)) return -1;
    /* §M54 — the compositor can dispose the window under a live client (the X
     * button's force path), which clears d->win.  Present is the one call that
     * dereferences it, so it re-checks rather than trusting the handle lookup
     * it made a few instructions ago. */
    if (!d->win) return -1;
    gui_window_blit(d->win, 0, 0, px, w, h, stride);
    return 0;
}

int dosgui_poll(int handle, struct dosgui_event* out) {
    struct dosgui_win* d = dosgui_owned(handle);      /* §audit#4 — ownership */
    if (!d || !out) return -1;
    /* §audit#5 — `out` is the client's ring-3 pointer we write the event into;
     * validate it's mapped + user-WRITABLE before dereferencing (else a bad
     * pointer faults the kernel / clobbers kernel memory). */
    if (!vmm_user_access_ok((uintptr_t)out, sizeof(*out), 1)) return -1;
    /* Title-bar X clicked?  Report a close event so the client quits itself;
     * once its task dies the compositor disposes the window (M22.7). */
    if (d->win && gui_window_want_close(d->win)) {
        out->type = 3; out->keycode = 0; out->pressed = 0; out->x = 0; out->y = 0;
        out->ch = 0;
        return 1;
    }
    /* §M42 — the window has been RESIZED.
     *
     * Reported the same way the close is: by comparing window state on poll,
     * not by an enqueued event.  A resize is a LEVEL, not an edge — what the
     * client needs is the size the window is NOW, and a queue could hand it a
     * stale one after a drag produced fifty of them.  Comparing on poll means
     * the client always learns the current size and exactly once per change.
     *
     * Without this a client-managed window had no way to find out at all: it
     * has no app-host to re-lay it out (host_task is cleared by design), so the
     * WM grew the window, the compositor allocated a bigger content surface,
     * and the client went on presenting its original small image into the
     * corner of it.  That is exactly what a user sees as "the window resizes
     * but its contents do not". */
    if (d->win) {
        int cw = 0, ch = 0;
        if (gui_window_content_size(d->win, &cw, &ch) == 0 &&
            cw > 0 && ch > 0 && (cw != d->rep_w || ch != d->rep_h)) {
            spin_lock(&d->lock);
            d->rep_w = cw; d->rep_h = ch;
            spin_unlock(&d->lock);
            out->type = 4; out->keycode = 0; out->pressed = 0;
            out->x = (int32_t)cw; out->y = (int32_t)ch; out->ch = 0;
            return 1;
        }
    }

    int got = 0;
    spin_lock(&d->lock);
    if (d->head != d->tail) {
        *out = d->evq[d->head];
        d->head = (d->head + 1) % DOSGUI_EVQ;
        got = 1;
    }
    spin_unlock(&d->lock);
    return got;
}

void dosgui_destroy(int handle) {
    struct dosgui_win* d = dosgui_owned(handle);      /* §audit#4 — ownership */
    if (!d) return;
    /* The client is done with the window (this is its last dosgui call, from
     * dos_finalise just before it exits).  Release it to the compositor for
     * disposal — client_release marks want_close + host_released so apply_pending
     * tears it down WITHOUT touching the (init-owned) client task struct. */
    /* Only ASK for disposal here.  The slot is freed by dosgui_disposed_cb when
     * the compositor actually tears the window down — one release path for all
     * routes, instead of this one clearing the slot early and the crash route
     * never clearing it at all. */
    if (d->win) gui_window_client_release(d->win);
}

/* ===========================================================================
 * §M65 stage 4 — THE TOOLKIT, FROM RING 3.
 *
 * A client can already own a window and blit pixels into it.  What it could
 * not do is use the SAME widgets the kernel-side apps use — and building a
 * second toolkit in ring 3 would mean two of everything: two checkboxes, two
 * layout engines, two sets of bugs.
 *
 * So the toolkit is shared, and the boundary carries DATA:
 *
 *   in   — a blob: a header, N fixed-size spec records, and a string pool.
 *          Offsets, never pointers, because a pointer means nothing on the
 *          other side of an address space.  This is why `struct ui_spec` was
 *          designed as ints and strings in the first place (§M65 stage 1).
 *   out  — the events the widgets raise, on the SAME queue the client already
 *          drains for keys and clicks: type 5, carrying (id, type, value).
 *
 * REUSING THE EVENT STRUCT rather than widening it is deliberate: its layout
 * is an ABI shared with clients that are already built (NetSurf's libnsfb
 * backend), and a struct that grows breaks every one of them.  Three int32
 * fields it already has say everything a UI event needs.
 * ========================================================================= */

#define DG_UI_MAGIC   0x49554F44u        /* "DOUI" */
#define DG_UI_VERSION 1
#define DG_UI_MAX_SPECS 64
#define DG_UI_MAX_BLOB  (16 * 1024)

/* On-the-wire record.  Fixed width, little-endian, no padding surprises: every
 * field is int32 on purpose so the layout is identical on all three arches
 * (§M56's epoll_event lesson — a struct whose size follows the word width is a
 * struct that behaves differently per arch). */
struct dg_ui_rec {
    int32_t id, parent;
    int32_t cls_off, text_off;           /* byte offsets into the string pool */
    int32_t value, min, max, weight, flags;
};

struct dg_ui_hdr {
    uint32_t magic, version;
    int32_t  count;                      /* number of records                 */
    int32_t  str_off, str_len;           /* string pool, relative to the blob  */
};

/* The event sink handed to ui_build: a widget event becomes a queue entry the
 * client's existing poll loop returns. */
static void dosgui_ui_event(struct gui_window* w, int id, int type, int value,
                            void* ctx) {
    (void)w;
    struct dosgui_win* d = (struct dosgui_win*)ctx;
    if (!d) return;
    spin_lock(&d->lock);
    int nt = (d->tail + 1) % DOSGUI_EVQ;
    if (nt != d->head) {
        struct dosgui_event* e = &d->evq[d->tail];
        e->type    = 5;                  /* UI event                          */
        e->keycode = (int32_t)id;
        e->pressed = (int32_t)type;
        e->x       = (int32_t)value;
        e->y       = 0;
        e->ch      = 0;
        d->tail = nt;
    }
    spin_unlock(&d->lock);
}

int dosgui_ui_build(int handle, const void* blob, int len) {
    struct dosgui_win* d = dosgui_owned(handle);
    if (!d || !d->win || !blob) return -1;
    if (len < (int)sizeof(struct dg_ui_hdr) || len > DG_UI_MAX_BLOB) return -1;

    /* The blob is a RING-3 buffer: copy it in before looking at a single field,
     * because every offset in it is about to be used as an index (§M46's rule —
     * validate where the pointer's origin is known, then work on a kernel
     * copy). */
    char* buf = (char*)kmalloc((size_t)len);
    if (!buf) return -1;
    if (copy_from_user(buf, (uintptr_t)blob, (size_t)len) != 0) { kfree(buf); return -1; }

    struct dg_ui_hdr* h = (struct dg_ui_hdr*)buf;
    if (h->magic != DG_UI_MAGIC || h->version != DG_UI_VERSION) { kfree(buf); return -1; }
    if (h->count < 0 || h->count > DG_UI_MAX_SPECS)             { kfree(buf); return -1; }
    if (h->str_off < 0 || h->str_len < 0 ||
        h->str_off > len || h->str_off + h->str_len > len)      { kfree(buf); return -1; }

    int64_t need = (int64_t)sizeof *h + (int64_t)h->count * (int64_t)sizeof(struct dg_ui_rec);
    if (need > len) { kfree(buf); return -1; }

    /* Make the pool NUL-terminated at its end, so a truncated offset can only
     * ever yield a short string — never a walk off the buffer. */
    if (h->str_len > 0) buf[h->str_off + h->str_len - 1] = 0;

    struct dg_ui_rec* rec = (struct dg_ui_rec*)(buf + sizeof *h);
    struct ui_spec* sp = (struct ui_spec*)kcalloc((size_t)(h->count ? h->count : 1),
                                                  sizeof *sp);
    if (!sp) { kfree(buf); return -1; }

    for (int i = 0; i < h->count; i++) {
        const char* cls  = "";
        const char* text = "";
        if (rec[i].cls_off  >= 0 && rec[i].cls_off  < h->str_len)
            cls  = buf + h->str_off + rec[i].cls_off;
        if (rec[i].text_off >= 0 && rec[i].text_off < h->str_len)
            text = buf + h->str_off + rec[i].text_off;
        sp[i].id     = rec[i].id;
        sp[i].parent = rec[i].parent;
        sp[i].cls    = cls;
        sp[i].text   = text;
        sp[i].value  = rec[i].value;
        sp[i].min    = rec[i].min;
        sp[i].max    = rec[i].max;
        sp[i].weight = rec[i].weight;
        sp[i].flags  = rec[i].flags;
    }

    /* The controls COPY every string they are given (checkbox caption, radio
     * options, text content), so the pool may die with this call — checked,
     * because the alternative is a widget pointing into freed memory. */
    int built = ui_build(d->win, sp, h->count, dosgui_ui_event, d);
    kfree(sp);
    kfree(buf);
    return built;
}
