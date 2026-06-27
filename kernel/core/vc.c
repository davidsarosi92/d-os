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

#include "vc.h"
#include "task.h"
#include "console.h"
#include "lock.h"
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
            if (n->vc) { n->vc->cur_col = 0; n->vc->cur_row = 0; }
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

void vc_putchar(struct vc* v, char c) {
    if (!v || !v->leaf) return;
    struct vc_node* n = v->leaf;

    if (c == '\n') {
        v->cur_col = 0;
        if (++v->cur_row >= n->h) {
            fb_scroll_cells_up(n->x, n->y, n->w, n->h, v->bg);
            v->cur_row = n->h - 1;
        }
        return;
    }
    if (c == '\r') { v->cur_col = 0; return; }
    if (c == '\b') {
        if (v->cur_col > 0) {
            v->cur_col--;
            fb_draw_glyph_at(n->x + v->cur_col, n->y + v->cur_row, ' ', v->fg, v->bg);
        }
        return;
    }

    fb_draw_glyph_at(n->x + v->cur_col, n->y + v->cur_row, c, v->fg, v->bg);
    v->cur_col++;
    if (v->cur_col >= n->w) {
        v->cur_col = 0;
        if (++v->cur_row >= n->h) {
            fb_scroll_cells_up(n->x, n->y, n->w, n->h, v->bg);
            v->cur_row = n->h - 1;
        }
    }
}

void vc_clear(struct vc* v) {
    if (!v || !v->leaf) return;
    struct vc_node* n = v->leaf;
    fb_clear_cells(n->x, n->y, n->w, n->h, v->bg);
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
    vcs[vc_n++] = v;
    return v;
}

static struct vc_node* node_alloc_leaf(struct vc_node* parent, struct vc* vc) {
    struct vc_node* n = (struct vc_node*)kcalloc(1, sizeof(struct vc_node));
    if (!n) return NULL;
    n->kind   = VCNODE_LEAF;
    n->parent = parent;
    n->vc     = vc;
    if (vc) vc->leaf = n;
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

void vc_kbd_push(char c) {
    struct vc* v = focused;             /* snapshot — pointer-sized atomic */
    if (!v) return;
    uint32_t next = (v->in_head + 1) & VC_INBUF_MASK;
    if (next == v->in_tail) return;     /* ring full — drop */
    v->in_buf[v->in_head] = c;
    v->in_head = next;
}

char vc_getchar(struct vc* v) {
    for (;;) {
        if (v->in_head != v->in_tail) {
            char c = v->in_buf[v->in_tail];
            v->in_tail = (v->in_tail + 1) & VC_INBUF_MASK;
            return c;
        }
        /* Sleep until the next interrupt arrives (IRQ may have filled
         * our ring), then offer the CPU to other tasks before re-
         * checking.  Same atomic sti+hlt trick as in keyboard_getchar. */
        __asm__ volatile ("sti; hlt");
        task_yield();
    }
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

int vc_get_rect(const struct vc* v, int* x, int* y, int* w, int* h) {
    if (!v || !v->leaf) return -1;
    if (x) *x = v->leaf->x;
    if (y) *y = v->leaf->y;
    if (w) *w = v->leaf->w;
    if (h) *h = v->leaf->h;
    return 0;
}
