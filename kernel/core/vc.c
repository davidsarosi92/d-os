/* =============================================================================
 * vc.c — virtual consoles + binary split tree on the framebuffer (M14).
 *
 * Data model.  The screen is partitioned by a binary tree of `vc_node`s
 * rooted at `root`.  A leaf owns a `struct vc` (cursor, input ring,
 * shell task); a split node owns two child rects + a direction.  All
 * coordinates are in CELLS, not pixels — the fb_terminal rect API does
 * the cell-to-pixel translation.
 *
 *   root      = full screen
 *   leaf      = one VC
 *   split H   = top child + bottom child (each gets half the height)
 *   split V   = left child + right child (each gets half the width)
 *
 * Layout.  After every tree mutation we walk the tree top-down and
 * recompute every node's (x,y,w,h).  No incremental layout: it's a 3-
 * line recursion and the tree is tiny (≤ VC_MAX leaves), so simplicity
 * wins over a delta-update.
 *
 * Output.  vc_putchar is the only renderer; everything else delegates
 * to it.  console.c calls into it via the per-task emit hook installed
 * in vc_init — that's how each shell task's kprintf lands in its own
 * pane without the shell touching VC APIs directly.
 *
 * Input.  The PS/2 IRQ calls vc_kbd_push(c) with each decoded char and
 * vc_focused() decides which VC's ring to push into.  The focused-VC's
 * shell task wakes via the cooperative sti+hlt+yield pattern in
 * vc_getchar.
 *
 * Concurrency.
 *   - The split tree is mutated only from a shell task (`pane split`
 *     command) and is read by vc_kbd_push from IRQ context.  We
 *     bracket mutations in preempt_disable + cli/sti to keep the IRQ
 *     side from observing a half-mutated tree.
 *   - Per-VC input ring is single-producer (IRQ) / single-consumer
 *     (owning task) — no lock needed on x86 for aligned 32-bit indices.
 *
 * Allocation.  vc_node structs come from kmalloc; never freed today
 * (no `pane kill` yet — tracked as an M14 follow-up in PLAN.md).
 * ============================================================================= */

#include "tty.h"
#include "klog.h"
#include "vc.h"
#include "lnx_signal.h"   /* §M90 rung 6 — a signal ends a wait for input */
#include "splash.h"
#include "fd.h"                       /* fd_readiness_signal (§M56) */
#include "task.h"
#include "console.h"
#include "lock.h"
#include "hal_api.h"
#include "kmalloc.h"
#include "printf.h"
#include <stddef.h>
#include <stdint.h>

/* fb_terminal rect API — declared here rather than in a header to keep
 * the dependency one-way (the FB driver doesn't know about VCs). */
extern int  fb_cell_cols(void);
extern int  fb_cell_rows(void);
extern void fb_clear_cells(int col, int row, int w, int h, uint32_t bg);
extern void fb_draw_glyph_at(int col, int row, char ch, uint32_t fg, uint32_t bg);
extern void fb_scroll_cells_up(int col, int row, int w, int h, uint32_t bg);
extern void fb_sink_disable(void);

/* Default colors.  Mirror fb_terminal's choices so the boot log and
 * post-vc_init screen look consistent. */
#define VC_FG  0xFFE0E0E0u
#define VC_BG  0xFF101828u

/* ---------------------------------------------------------------------------
 * Tree node — internal to this file.
 * --------------------------------------------------------------------------- */

enum vc_node_kind { VCNODE_LEAF, VCNODE_SPLIT };

struct vc_node {
    enum vc_node_kind kind;
    struct vc_node*   parent;
    int               x, y, w, h;       /* recomputed by layout pass */
    /* LEAF */
    struct vc*        vc;
    /* SPLIT */
    enum vc_split_dir dir;
    struct vc_node*   a;
    struct vc_node*   b;
};

/* Singletons. */
static struct vc_node* root      = NULL;
static struct vc*      focused   = NULL;
static struct vc*      vcs[VC_MAX];
static int             vc_n      = 0;
static int             next_id   = 1;

/* M22: while non-zero, leaf VCs skip FB rendering and Alt-N focus
 * switching is disabled — the GUI compositor owns the screen. */
static volatile int    screen_suppressed = 0;
/* 2026-10-16 — every pane's cell store has the screen's geometry (see the
 * cell store section below). */
static int g_store_stride, g_store_rows;

/* ---------------------------------------------------------------------------
 * Layout pass — recursively assign rects to every node.
 *
 * Split is 50/50.  The first child gets the floor half, the second the
 * remainder — keeps total = parent regardless of odd dimensions.
 * --------------------------------------------------------------------------- */

static void layout(struct vc_node* n, int x, int y, int w, int h) {
    n->x = x; n->y = y; n->w = w; n->h = h;
    if (n->kind == VCNODE_LEAF) {
        /* If our cursor escaped the new (smaller) rect, snap it back
         * inside.  Lets a previously-large VC shrink without crashing. */
        if (n->vc) {
            if (n->vc->cur_col >= w) n->vc->cur_col = (w > 0) ? w - 1 : 0;
            if (n->vc->cur_row >= h) n->vc->cur_row = (h > 0) ? h - 1 : 0;
        }
        return;
    }
    if (n->dir == VC_SPLIT_HORIZ) {
        int top_h = h / 2;
        layout(n->a, x, y,         w, top_h);
        layout(n->b, x, y + top_h, w, h - top_h);
    } else {                                 /* VC_SPLIT_VERT */
        int left_w = w / 2;
        layout(n->a, x,          y, left_w, h);
        layout(n->b, x + left_w, y, w - left_w, h);
    }
}

/* ---------------------------------------------------------------------------
 * Rendering helpers.
 * --------------------------------------------------------------------------- */

static void repaint_all(void) {
    /* Just clear every leaf — content is not preserved across splits.
     * (Could be improved later with a scrollback buffer per VC.) */
    if (!root) return;
    /* Walk: a simple stack is overkill — use recursive lambda-ish. */
    struct vc_node* stack[VC_MAX * 2];
    int sp = 0;
    stack[sp++] = root;
    while (sp) {
        struct vc_node* n = stack[--sp];
        if (n->kind == VCNODE_LEAF) {
            fb_clear_cells(n->x, n->y, n->w, n->h, VC_BG);
            if (n->vc) {
                n->vc->cur_col = 0; n->vc->cur_row = 0;
                /* the cell store follows the screen: the pane is blank now */
                if (n->vc->cell)
                    for (int i = 0; i < g_store_stride * g_store_rows; i++)
                        n->vc->cell[i] = ' ';
            }
        } else {
            stack[sp++] = n->a;
            stack[sp++] = n->b;
        }
    }
}

/* ---------------------------------------------------------------------------
 * vc_putchar — single renderer that everything else funnels through.
 *
 * Handles \n, \r, \b, wraparound and scroll.  No tab support yet
 * (deferred follow-up; the existing terminal drivers don't expand
 * tabs either).
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * THE PANE'S CELL STORE (2026-10-16).
 *
 * A pane used to draw straight into the framebuffer and remember nothing, so
 * it could not shift a line (insert/delete character), move part of the screen
 * (a scroll region, insert/delete lines) or put a screen back (the alternate
 * screen vim and less switch to) — all of which need to know what is on the
 * screen.  Now every cell is one 32-bit word: the character in the low byte,
 * its rendition above it (the same packing gterm.c uses: foreground and
 * background as palette index + 1, 0 = default; bold; reverse), and the
 * framebuffer is drawn FROM the store.  Fixed geometry — the whole screen's
 * cell count with the screen's width as stride — so a pane resize never
 * reallocates (a layout change clears the pane anyway, see repaint_all).
 * 144 KiB per pane at 1920x1200, allocated on a pane's first output.
 * While the GUI owns the screen the store keeps being written and nothing is
 * drawn, so leaving the GUI repaints the shell as it is (vc_screen_suppress).
 * --------------------------------------------------------------------------- */
#define PA_FG(a)   ((int)((a) & 0x1FF) - 1)
#define PA_BG(a)   ((int)(((a) >> 9) & 0x1FF) - 1)
#define PA_BOLD    (1u << 18)
#define PA_REV     (1u << 19)
#define PA_BGMASK  (0x1FFu << 9)

/* Allocated when the VC gets its pane (node_alloc_leaf, task context); the
 * lazy path here only covers a pane created before the framebuffer size was
 * known.  Never from the output path's first byte if it can be helped: output
 * can come from contexts that should not allocate. */
static uint32_t* pane_store(struct vc* v) {
    if (v->cell) return v->cell;
    if (!g_store_stride) { g_store_stride = fb_cell_cols(); g_store_rows = fb_cell_rows(); }
    if (g_store_stride <= 0 || g_store_rows <= 0) return NULL;
    v->cell = (uint32_t*)kcalloc((size_t)g_store_stride * g_store_rows, sizeof(uint32_t));
    return v->cell;
}
static inline uint32_t* pcell(struct vc* v, int r, int c) {
    return v->cell + (size_t)r * g_store_stride + c;
}
static uint32_t pane_blank(const struct vc* v) { return ' ' | ((v->cur_attr & PA_BGMASK) << 8); }
static void pane_colors(uint32_t a, uint32_t* fg, uint32_t* bg) {
    int f = PA_FG(a), b = PA_BG(a);
    if ((a & PA_BOLD) && f >= 0 && f < 8) f += 8;          /* bold = bright */
    uint32_t F = f < 0 ? VC_FG : 0xFF000000u | vt_rgb(f);
    uint32_t B = b < 0 ? VC_BG : 0xFF000000u | vt_rgb(b);
    if (a & PA_REV) { uint32_t t = F; F = B; B = t; }
    *fg = F; *bg = B;
}
/* Draw one cell from the store (nothing while the GUI owns the screen). */
static void pane_draw(struct vc* v, int r, int c) {
    if (screen_suppressed || !v->leaf || !v->cell) return;
    struct vc_node* n = v->leaf;
    if (r < 0 || c < 0 || r >= n->h || c >= n->w) return;
    uint32_t w = *pcell(v, r, c), fg, bg;
    pane_colors(w >> 8, &fg, &bg);
    char ch = (char)(w & 0xFF);
    fb_draw_glyph_at(n->x + c, n->y + r, ch ? ch : ' ', fg, bg);
}
static void pane_draw_rows(struct vc* v, int r0, int r1) {
    if (screen_suppressed || !v->leaf) return;
    for (int r = r0; r <= r1; r++)
        for (int c = 0; c < v->leaf->w; c++) pane_draw(v, r, c);
}
static void pane_set(struct vc* v, int r, int c, uint32_t w) {
    if (!pane_store(v)) return;
    if (r < 0 || c < 0 || r >= g_store_rows || c >= g_store_stride) return;
    *pcell(v, r, c) = w;
    pane_draw(v, r, c);
}
/* Blank the whole pane, in the store and (when it is ours) on screen. */
static void pane_wipe(struct vc* v) {
    if (pane_store(v))
        for (int i = 0; i < g_store_stride * g_store_rows; i++) v->cell[i] = pane_blank(v);
    if (!screen_suppressed && v->leaf) {
        uint32_t fg, bg; pane_colors(v->cur_attr, &fg, &bg);
        fb_clear_cells(v->leaf->x, v->leaf->y, v->leaf->w, v->leaf->h, bg);
    }
}
/* Rows top..bot by n (n > 0 up), blanks entering at the freed edge.  One row
 * up over the whole width is a pixel move (fb_scroll_cells_up); everything
 * else is redrawn from the store, which is correct for any shape. */
static void pane_scroll(struct vc* v, int top, int bot, int n) {
    if (!v->leaf || !pane_store(v) || !n) return;
    int h = v->leaf->h, w = v->leaf->w;
    if (top < 0) top = 0;
    if (bot >= h) bot = h - 1;
    if (top > bot) return;
    int span = bot - top + 1, k = n > 0 ? n : -n;
    if (k > span) k = span;
    uint32_t blank = pane_blank(v);
    if (n > 0) {
        for (int r = top; r <= bot; r++)
            for (int c = 0; c < w; c++)
                *pcell(v, r, c) = (r + k <= bot) ? *pcell(v, r + k, c) : blank;
    } else {
        for (int r = bot; r >= top; r--)
            for (int c = 0; c < w; c++)
                *pcell(v, r, c) = (r - k >= top) ? *pcell(v, r - k, c) : blank;
    }
    if (screen_suppressed) return;
    if (n == 1) {
        uint32_t fg, bg; pane_colors(blank >> 8, &fg, &bg);
        fb_scroll_cells_up(v->leaf->x, v->leaf->y + top, w, span, bg);
        pane_draw_rows(v, bot, bot);
    } else {
        pane_draw_rows(v, top, bot);
    }
}

/* The framebuffer pane renderer: one byte at the cursor (no escapes here —
 * vt.c has taken them out already). */
static void pane_put(struct vc* v, char c) {
    if (!v->leaf) return;
    struct vc_node* n = v->leaf;

    if (c == '\f') {
        pane_wipe(v);
        v->cur_col = 0; v->cur_row = 0;
        return;
    }
    if (c == '\n') {
        v->cur_col = 0;
        if (++v->cur_row >= n->h) {
            pane_scroll(v, 0, n->h - 1, 1);
            v->cur_row = n->h - 1;
        }
        return;
    }
    if (c == '\r') { v->cur_col = 0; return; }
    if (c == '\b') {
        if (v->cur_col > 0) {
            v->cur_col--;
            pane_set(v, v->cur_row, v->cur_col, pane_blank(v));
        }
        return;
    }
    if ((unsigned char)c < 0x20) return;          /* other controls draw nothing */

    pane_set(v, v->cur_row, v->cur_col, (unsigned char)c | (v->cur_attr << 8));
    v->cur_col++;
    if (v->cur_col >= n->w) {
        v->cur_col = 0;
        if (++v->cur_row >= n->h) {
            pane_scroll(v, 0, n->h - 1, 1);
            v->cur_row = n->h - 1;
        }
    }
}

/* §M90 rung 6 — the pane's side of the VT100 subset, over the cell store. */
static void pv_put(void* x, char c) { pane_put((struct vc*)x, c); }
static void pv_get(void* x, int* r, int* c) { struct vc* v = x; *r = v->cur_row; *c = v->cur_col; }
static void pv_set(void* x, int r, int c) { struct vc* v = x; v->cur_row = r; v->cur_col = c; }
static void pv_dims(void* x, int* r, int* c) {
    struct vc* v = x;
    *r = v->leaf ? v->leaf->h : 24;
    *c = v->leaf ? v->leaf->w : 80;
}
static void pv_erase(void* x, int row, int c0, int c1) {
    struct vc* v = x;
    if (!v->leaf) return;
    for (int c = c0 < 0 ? 0 : c0; c < c1 && c < v->leaf->w; c++) pane_set(v, row, c, pane_blank(v));
}
/* insert (n > 0) blanks / delete (n < 0) characters at `col` on `row` */
static void pv_shift(void* x, int row, int col, int n) {
    struct vc* v = x;
    if (!v->leaf || !pane_store(v) || row < 0 || row >= v->leaf->h || col < 0) return;
    int w = v->leaf->w;
    uint32_t* rr = pcell(v, row, 0), blank = pane_blank(v);
    if (n > 0) for (int c = w - 1; c >= col; c--) rr[c] = (c - n >= col) ? rr[c - n] : blank;
    else { int k = -n; for (int c = col; c < w; c++) rr[c] = (c + k < w) ? rr[c + k] : blank; }
    for (int c = col; c < w; c++) pane_draw(v, row, c);
}
static void pv_reply(void* x, const char* s) { for (; *s; s++) vc_kbd_push_to((struct vc*)x, *s); }
/* §M90 — SGR: the rendition the next cells are stored with. */
static void pv_attr(void* x, int fg, int bg, int bold, int rev) {
    struct vc* v = x;
    v->cur_attr = (fg == VT_DEFAULT && bg == VT_DEFAULT && !bold && !rev) ? 0
                : (uint32_t)(fg + 1) | ((uint32_t)(bg + 1) << 9) | (bold ? PA_BOLD : 0) | (rev ? PA_REV : 0);
    uint32_t f, b; pane_colors(v->cur_attr, &f, &b);
    v->fg = f; v->bg = b;
}
static void pv_lines(void* x, int row, int n) {
    struct vc* v = x;
    if (v->leaf) pane_scroll(v, row, v->leaf->h - 1, -n);
}
static void pv_scroll(void* x, int top, int bot, int n) { pane_scroll((struct vc*)x, top, bot, n); }
/* The alternate screen: the main screen's cells are kept aside (a copy of the
 * store) and the pane is blanked; leaving puts the copy back and redraws. */
static void pv_alt(void* x, int on) {
    struct vc* v = x;
    if (!pane_store(v)) return;
    size_t n = (size_t)g_store_stride * g_store_rows;
    if (on) {
        if (!v->alt_keep) v->alt_keep = (uint32_t*)kmalloc(n * sizeof(uint32_t));
        if (!v->alt_keep) return;                  /* no memory: draw over, as before */
        for (size_t i = 0; i < n; i++) v->alt_keep[i] = v->cell[i];
        pane_wipe(v);
    } else if (v->alt_keep) {
        for (size_t i = 0; i < n; i++) v->cell[i] = v->alt_keep[i];
        kfree(v->alt_keep);
        v->alt_keep = NULL;
        if (v->leaf) pane_draw_rows(v, 0, v->leaf->h - 1);
    }
}
static const struct vt_ops pane_vt_ops = {
    .put = pv_put, .get_cursor = pv_get, .set_cursor = pv_set, .dims = pv_dims,
    .erase = pv_erase, .shift = pv_shift, .reply = pv_reply, .attr = pv_attr,
    .lines = pv_lines, .scroll = pv_scroll, .altscreen = pv_alt,
};

void vc_putchar(struct vc* v, char c) {
    if (!v) return;
    /* §M90 rung 6 — through the VT100 subset when the VC has a renderer for
     * it (every pane, every GUI terminal window) */
    if (v->vtops) { vt_feed(&v->vt, c, v->vtops, v->vtctx); return; }
    /* M22: offscreen VCs render through their emit hook (GUI terminal
     * window), never through the FB cell grid. */
    if (v->emit) { v->emit(v->emit_ctx, c); return; }
    pane_put(v, c);
}

void vc_clear(struct vc* v) {
    if (!v) return;
    if (v->emit) { v->emit(v->emit_ctx, '\f'); return; }   /* GUI: form feed */
    if (!v->leaf) return;
    pane_wipe(v);                       /* the store too, drawn only if ours */
    v->cur_col = 0;
    v->cur_row = 0;
}

/* ---------------------------------------------------------------------------
 * Per-task emit adapter installed into console.c.
 * --------------------------------------------------------------------------- */

static void vc_emit_adapter(void* opaque, char c) {
    vc_putchar((struct vc*)opaque, c);
}

/* ---------------------------------------------------------------------------
 * VC allocation.
 * --------------------------------------------------------------------------- */

static struct vc* vc_alloc_new(void) {
    if (vc_n >= VC_MAX) return NULL;
    struct vc* v = (struct vc*)kcalloc(1, sizeof(struct vc));
    if (!v) return NULL;
    v->id      = next_id++;
    v->fg      = VC_FG;
    v->bg      = VC_BG;
    v->in_head = 0;
    v->in_tail = 0;
    waitq_init(&v->inq);                /* §M49 — readers park, they don't spin */
    vcs[vc_n++] = v;
    vt_init(&v->vt);
    v->vtops = &pane_vt_ops; v->vtctx = v;          /* §M90 rung 6 */
    return v;
}

static struct vc_node* node_alloc_leaf(struct vc_node* parent, struct vc* vc) {
    struct vc_node* n = (struct vc_node*)kcalloc(1, sizeof(struct vc_node));
    if (!n) return NULL;
    n->kind   = VCNODE_LEAF;
    n->parent = parent;
    n->vc     = vc;
    if (vc) { vc->leaf = n; (void)pane_store(vc); }   /* 2026-10-16 — its cell store */
    return n;
}

/* ---------------------------------------------------------------------------
 * vc_init — root VC = whole screen.
 * --------------------------------------------------------------------------- */

void vc_init(void) {
    int W = fb_cell_cols();
    int H = fb_cell_rows();
    if (W <= 0 || H <= 0) {
        kprintf("vc: no framebuffer — VC subsystem disabled\n");
        return;
    }

    struct vc* root_vc = vc_alloc_new();
    if (!root_vc) { kprintf("vc: OOM allocating root VC\n"); return; }

    root = node_alloc_leaf(NULL, root_vc);
    if (!root) { kprintf("vc: OOM allocating root node\n"); return; }

    layout(root, 0, 0, W, H);

    /* Take over the screen: paint everything in our bg, deactivate the
     * legacy fb sink so kprintf flows only through per-task routing. */
    fb_sink_disable();
    fb_clear_cells(0, 0, W, H, VC_BG);

    focused = root_vc;

    /* Wire console.c → vc_putchar so per-task kprintf lands here. */
    console_set_per_task_emit(vc_emit_adapter);

    kprintf("vc: ready, root VC = %dx%d cells (id=%d)\n", W, H, root_vc->id);
}

struct vc* vc_root(void)    { return root ? root->vc : NULL; }
struct vc* vc_focused(void) { return focused; }

/* ---------------------------------------------------------------------------
 * Focus.
 * --------------------------------------------------------------------------- */

void vc_focus(struct vc* v) {
    if (!v || v == focused) return;
    focused = v;
}

int vc_focus_by_id(int n) {
    /* Alt-N pane hotkeys are meaningless (and confusing — keys would
     * flow to an invisible pane) while the GUI owns the screen. */
    if (screen_suppressed) return -1;
    for (int i = 0; i < vc_n; i++) {
        if (vcs[i] && vcs[i]->id == n) {
            vc_focus(vcs[i]);
            return 0;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * Split — the heart of M14.
 *
 * Steps (all inside preempt_disable so the IRQ side can't see a partial
 * tree):
 *   1. Allocate the new VC + two new leaf nodes.
 *   2. Locate v->leaf in the tree.
 *   3. Convert v->leaf into a SPLIT node in-place: its two children
 *      become the leaf for v (now `a`) and the leaf for the new VC
 *      (now `b`).  We can't just swap the node pointer because v's
 *      parent points at the old node — we MUST keep that address and
 *      mutate the existing node in place.
 *   4. Recompute layout from root.
 *   5. Repaint every leaf.
 *
 * On the (rare) failure paths after partial allocation, we free what we
 * managed to allocate and bail.
 * --------------------------------------------------------------------------- */

struct vc* vc_split(struct vc* v, enum vc_split_dir dir) {
    if (!v || !v->leaf || !root) return NULL;
    if (vc_n >= VC_MAX) {
        kprintf("vc_split: max %d VCs reached\n", VC_MAX);
        return NULL;
    }

    struct vc* nv = vc_alloc_new();
    if (!nv) return NULL;

    struct vc_node* split_node = v->leaf;            /* will become SPLIT in place */
    struct vc_node* a = node_alloc_leaf(split_node, v);
    struct vc_node* b = node_alloc_leaf(split_node, nv);
    if (!a || !b) {
        /* Roll back: free whatever stuck and remove nv from the table. */
        if (a) kfree(a);
        if (b) kfree(b);
        vcs[--vc_n] = NULL;
        kfree(nv);
        kprintf("vc_split: OOM\n");
        return NULL;
    }

    preempt_disable();
    split_node->kind = VCNODE_SPLIT;
    split_node->vc   = NULL;
    split_node->dir  = dir;
    split_node->a    = a;
    split_node->b    = b;
    /* v->leaf is now `a`; nv->leaf is `b` (set by node_alloc_leaf). */

    /* Recompute the whole tree's rects.  Cheap — VC_MAX = 9. */
    layout(root, root->x, root->y, root->w, root->h);
    preempt_enable();

    repaint_all();
    focused = nv;       /* land focus on the newly created pane */
    return nv;
}

/* ---------------------------------------------------------------------------
 * Input ring.
 * --------------------------------------------------------------------------- */

/* M22.1: optional IRQ-side intercept — see vc.h. */
static int (*kbd_hook)(char c) = NULL;

void vc_set_kbd_hook(int (*fn)(char c)) { kbd_hook = fn; }

/* M22.3: raw keycode intercept, pre-translation (Alt-Tab). */
static int (*raw_kbd_hook)(uint8_t, uint8_t) = NULL;

void vc_set_raw_kbd_hook(int (*fn)(uint8_t, uint8_t)) { raw_kbd_hook = fn; }

int vc_raw_kbd_dispatch(uint8_t keycode, uint8_t mods) {
    /* §M62 rule 4 — ANY key drops the boot splash, and this is the one place
     * every input driver already passes through BEFORE it decides where the
     * key should go (PS/2, USB HID, virtio-input all call it pre-translation).
     *
     * It used to live in `vc_kbd_push` alone, which looked equivalent and was
     * not: the drivers only call that when a VC is FOCUSED, and during boot
     * there is no VC yet — `vc_init` runs near the end.  So for the entire
     * period the splash is actually up, the escape hatch was unreachable, and
     * ESC in particular never produced anything at all (a bare Escape has no
     * shell meaning, so nothing else noticed either).  Reported from use.
     *
     * Pre-translation also means it does not depend on the key having a
     * character: Escape, a function key and Shift+something all dismiss it. */
    if (splash_active()) { splash_key(); return 1; }

    return raw_kbd_hook ? raw_kbd_hook(keycode, mods) : 0;
}

/* Push one character into a NAMED console's input ring.
 *
 * Split out of vc_kbd_push because a keystroke and an injected command are two
 * different things: a keystroke belongs to whoever has focus, while "run this
 * in that window" names its destination.  Everything below the destination —
 * the ring, the wake, the end-of-line readiness signal — is identical, and
 * duplicating it would be two chances to get the lost-wakeup rule wrong. */
void vc_kbd_push_to(struct vc* v, char c) {
    if (!v) return;
    /* §M90 — ^C / ^\ / ^Z for the console's foreground group (tty.c) */
    if ((c == 3 || c == 28 || c == 26) && tty_console_isig(v, c)) return;
    uint32_t next = (v->in_head + 1) & VC_INBUF_MASK;
    if (next == v->in_tail) return;     /* ring full — drop */
    v->in_buf[v->in_head] = c;
    v->in_head = next;
    uint32_t fl = waitq_lock(&v->inq);
    waitq_wake_all(&v->inq);
    waitq_unlock(&v->inq, fl);
    /* 2026-10-16 — EVERY byte, not only '\n'.  The end-of-line rule is the
     * COOKED answer and fd_readiness_of still gives it (a cooked reader is
     * readable at Enter); but a program that put the console into raw mode
     * and waits in poll() — busybox `less` does — was never told a key had
     * arrived and drew its first page forever.  A wake that finds nothing
     * new costs one re-check. */
    fd_readiness_signal();
}

/* §M82 (2026-09-27) — THE CONSOLE BEHIND A DESKTOP IS SEALED while an
 * ordinary account holds that desktop.  Keystrokes no window wants fall
 * through to the focused VC, which behind the GUI is the boot console — a
 * shell running as SYSTEM.  That is how the harness types with the desktop
 * up, and it was also how anybody signed in to the desktop could type
 * commands, unseen, into a shell with the machine's privileges.  An
 * administrator's session leaves it open (the harness, and nothing an admin
 * could not do anyway); a non-admin's drops those keys and says so once. */
static volatile int console_sealed;
static int sealed_said;
void vc_console_seal(int on) {
    console_sealed = on;
    sealed_said = 0;
}

void vc_kbd_push(char c) {
    /* §M62 — any key drops the boot splash.  A boot screen you cannot get out
     * of hides the answer exactly when it is wanted; the key is consumed so it
     * does not also land in whatever is reading. */
    if (splash_active()) { splash_key(); return; }

    if (kbd_hook && kbd_hook(c)) return;        /* consumed by the GUI */
    if (console_sealed) {
        if (!sealed_said) {
            sealed_said = 1;
            klog(KLOG_NOTICE, "vc", "key for the system console dropped - "
                 "the desktop belongs to an ordinary account\n");
        }
        return;
    }
    struct vc* v = focused;             /* snapshot — pointer-sized atomic */
    vc_kbd_push_to(v, c);
}

/* §M49 — block until a byte arrives, instead of polling for one.
 *
 * This was `hlt` + `task_yield` in a loop, which kept the caller RUNNABLE
 * and queued for as long as it waited: one runqueue slot per idle shell,
 * a core halted on every turn through the queue, and — after §M49 started
 * measuring demand as time spent runnable — a shell that measured
 * identical to a CPU hog.  An otherwise idle d-os sat at 100% of a core.
 *
 * The kill contract is preserved deliberately.  `task.h` and the GUI
 * window teardown both rely on a killed shell dying "at its next
 * vc_getchar yield", so the wait breaks on `task_should_stop()` and exits
 * through `task_yield()` — the documented cooperative kill point, which
 * calls `task_exit()` for us.  The other half of that contract lives in
 * the scheduler: `task_kill` now wakes a task parked on a waitq
 * (wake_waitq_sleeper), because a blocked task cannot poll a flag. */
char vc_getchar(struct vc* v) {
    for (;;) {
        uint32_t fl = waitq_lock(&v->inq);
        while (v->in_head == v->in_tail && !task_should_stop())
            waitq_block(&v->inq);

        if (v->in_head != v->in_tail) {
            char c = v->in_buf[v->in_tail];
            v->in_tail = (v->in_tail + 1) & VC_INBUF_MASK;
            waitq_unlock(&v->inq, fl);
            return c;
        }
        waitq_unlock(&v->inq, fl);
        /* Woken with an empty ring => a kill is pending.  Never exit
         * while holding the waitq lock. */
        task_yield();
    }
}

/* See vc.h: readable means a whole LINE is available, because that is what a
 * cooked read will consume.  Scanning the ring costs a few dozen byte
 * comparisons and is only done from poll/epoll, never per keystroke. */
/* §M90 rung 6 — the console as a terminal: tty.c's primitives. */
int vc_read_nb(struct vc* v, char* buf, int n) {
    if (!v) return 0;
    int k = 0;
    while (k < n && v->in_tail != v->in_head) {
        buf[k++] = v->in_buf[v->in_tail];
        v->in_tail = (v->in_tail + 1) & VC_INBUF_MASK;
    }
    return k;
}
int vc_in_count(struct vc* v) { return v ? (int)((v->in_head - v->in_tail) & VC_INBUF_MASK) : 0; }
void vc_in_flush(struct vc* v) { if (v) v->in_tail = v->in_head; }
int vc_wait_input(struct vc* v) {
    if (!v) return -1;
    struct task* me = task_current();
    uint32_t fl = waitq_lock(&v->inq);
    while (v->in_head == v->in_tail) {
        if (task_should_stop() || (me && lnx_sig_deliverable(me))) { waitq_unlock(&v->inq, fl); return -1; }
        waitq_block(&v->inq);
    }
    waitq_unlock(&v->inq, fl);
    return 0;
}
void vc_cells(struct vc* v, int* rows, int* cols) {
    *rows = 24; *cols = 80;
    if (v && v->vtops && v->vtops->dims) v->vtops->dims(v->vtctx, rows, cols);
}

int vc_can_read_line(struct vc* v) {
    if (!v) return 0;
    uint32_t head = v->in_head, tail = v->in_tail;
    while (tail != head) {
        char c = v->in_buf[tail];
        if (c == '\n') return 1;
        tail = (tail + 1) & VC_INBUF_MASK;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Iteration / diagnostics.
 * --------------------------------------------------------------------------- */

void vc_for_each(vc_iter_fn fn, void* ctx) {
    if (!fn) return;
    for (int i = 0; i < vc_n; i++) {
        if (vcs[i]) fn(vcs[i], ctx);
    }
}

int vc_count(void) { return vc_n; }

/* M22.4 — is a task with this pid bound to any VC (see vc.h)?  Reads
 * vc->task->pid without a lock: callers only use this as an advisory
 * "don't reap" filter on the compositor task, and the bound task
 * pointer is only cleared/reassigned on that same task (window close)
 * or at pane creation. */
int vc_task_bound(int pid) {
    for (int i = 0; i < vc_n; i++)
        if (vcs[i] && vcs[i]->task && vcs[i]->task->pid == pid) return 1;
    return 0;
}

int vc_get_rect(const struct vc* v, int* x, int* y, int* w, int* h) {
    if (!v || !v->leaf) return -1;
    if (x) *x = v->leaf->x;
    if (y) *y = v->leaf->y;
    if (w) *w = v->leaf->w;
    if (h) *h = v->leaf->h;
    return 0;
}

/* ---------------------------------------------------------------------------
 * M22 GUI hooks.
 * --------------------------------------------------------------------------- */

struct vc* vc_create_offscreen(void (*emit)(void* ctx, char c), void* ctx) {
    struct vc* v = vc_alloc_new();
    if (!v) return NULL;
    v->emit     = emit;
    v->emit_ctx = ctx;
    v->vtops    = NULL;      /* §M90 rung 6 — raw until the owner installs a renderer */
    /* No leaf: this VC lives outside the split tree.  vc_kbd_push and
     * vc_getchar work unchanged because they only touch the ring. */
    return v;
}

/* 2026-10-16 — handing the screen back REPAINTS every pane from its cell
 * store: the shell comes back showing what it showed (and whatever it printed
 * while the GUI was up), not whatever the GUI left in the framebuffer. */
void vc_screen_suppress(int on) {
    int was = screen_suppressed;
    screen_suppressed = on ? 1 : 0;
    if (was && !on)
        for (int i = 0; i < vc_n; i++)
            if (vcs[i] && vcs[i]->leaf && vcs[i]->cell && !vcs[i]->emit)
                pane_draw_rows(vcs[i], 0, vcs[i]->leaf->h - 1);
}
int  vc_screen_suppressed(void) { return screen_suppressed; }

void vc_destroy(struct vc* v) {
    if (!v || v->leaf) return;              /* pane VCs are not destroyable */
    tty_console_forget(v);                  /* §M90 rung 6 — its termios go with it */
    /* Keep the IRQ side (vc_kbd_push / vc_focus_by_id) from touching a
     * dying VC: fix `focused` first, then compact the table.  Same
     * preempt_disable discipline as the split-tree mutations; the
     * pointer writes themselves are atomic. */
    preempt_disable();
    if (focused == v) focused = NULL;       /* GUI refocuses explicitly */
    for (int i = 0; i < vc_n; i++) {
        if (vcs[i] == v) {
            for (; i < vc_n - 1; i++) vcs[i] = vcs[i + 1];
            vcs[--vc_n] = NULL;
            break;
        }
    }
    preempt_enable();
    if (v->cell) kfree(v->cell);
    if (v->alt_keep) kfree(v->alt_keep);
    kfree(v);
}
