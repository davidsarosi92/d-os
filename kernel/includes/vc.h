/* =============================================================================
 * vc.h — virtual consoles (panes) on top of the framebuffer.
 *
 * A VC is a rectangular region of the FB cell grid plus a cursor, a tiny
 * input ring, and a back pointer to the shell task that owns it.  VCs
 * never overlap — the screen is partitioned by a binary split tree
 * rooted at `root`:
 *
 *   - Leaf node  → one VC occupying its rect.
 *   - Split node → two children (a, b) and a direction (horiz | vert).
 *                  Layout pass divides the parent's rect 50/50.
 *
 * Operations:
 *   - vc_init()              — create the root VC = full screen.
 *   - vc_split(v, dir)       — split v's leaf into (v, new_vc); spawns
 *                              no shell — caller handles that.
 *   - vc_focus(v)            — direct keyboard input to v.
 *   - vc_focus_by_id(n)      — focus VC whose `id == n`.
 *   - vc_focused()           — current input target (NULL if none).
 *   - vc_putchar / vc_clear  — render into v's rect.
 *   - vc_getchar(v)          — blocking read from v's input ring.
 *   - vc_kbd_push(c)         — IRQ-side: push char into focused VC's ring.
 *   - vc_list_each(...)      — iterate leaves for `pane` shell command.
 *
 * Concurrency:
 *   - Tree mutations (split, focus change) happen on a shell-task and
 *     are bracketed by preempt_disable/enable to keep the IRQ side
 *     (vc_kbd_push) from seeing torn pointers.
 *   - Per-VC input ring head/tail are accessed by the IRQ (writer) and
 *     the owning task (reader) — relaxed access on x86 is safe for a
 *     single producer and single consumer with aligned 32-bit indices.
 * ============================================================================= */

#ifndef VC_H
#define VC_H

#include <stdint.h>
#include <stddef.h>

#define VC_INBUF_SZ   64                /* must be power-of-two */
#define VC_INBUF_MASK (VC_INBUF_SZ - 1)
#define VC_MAX        9                 /* Alt-1 .. Alt-9 cap */

enum vc_split_dir {
    VC_SPLIT_HORIZ = 0,                 /* top / bottom */
    VC_SPLIT_VERT  = 1,                 /* left / right */
};

struct vc_node;                         /* opaque to callers */
struct task;                            /* fwd */

struct vc {
    int               id;               /* 1..VC_MAX, assigned at creation */
    struct vc_node*   leaf;             /* back pointer; reset by layout pass */
    int               cur_col;          /* cursor inside leaf->w */
    int               cur_row;          /* cursor inside leaf->h */
    uint32_t          fg, bg;
    /* SPSC ring — IRQ side writes head, owner reads tail. */
    volatile char     in_buf[VC_INBUF_SZ];
    volatile uint32_t in_head;
    volatile uint32_t in_tail;
    /* Shell task bound to this VC (NULL until spawned). */
    struct task*      task;
    /* M22: optional output override.  When non-NULL, vc_putchar hands
     * every byte to this hook instead of rendering into the FB cell
     * grid — that's how a GUI terminal window reuses the whole shell +
     * console plumbing without owning a pane.  `vc_clear` sends '\f'
     * through the same hook.  Offscreen VCs have leaf == NULL. */
    void (*emit)(void* ctx, char c);
    void*             emit_ctx;
};

/* Bring up the VC subsystem.  Creates the root VC covering the whole FB,
 * marks it focused, and disables the legacy fb_sink so kprintf no
 * longer prints across pane boundaries. */
void vc_init(void);

/* The root VC — used at boot to bind to the first shell task. */
struct vc* vc_root(void);

/* The currently focused VC (or NULL during early boot). */
struct vc* vc_focused(void);

/* Set focus to `v`.  Safe to call any time after vc_init.  No-op if
 * `v` is NULL or already focused. */
void vc_focus(struct vc* v);

/* Focus the VC whose `id == n`.  Returns 0 on success, -1 if no such
 * VC exists. */
int  vc_focus_by_id(int n);

/* Split `v`'s leaf into two halves along `dir`.  `v` keeps the first
 * half (top for HORIZ, left for VERT); the second half is the new VC,
 * returned.  Both VCs' rects are recomputed and their content cleared.
 * Caller is responsible for spawning a shell task on the returned VC.
 * Returns NULL on failure (VC_MAX exceeded, OOM, etc.). */
struct vc* vc_split(struct vc* v, enum vc_split_dir dir);

/* Render `c` inside `v`'s rect at the current cursor position.  Handles
 * \n, \r, \b, scrolling. */
void vc_putchar(struct vc* v, char c);

/* Clear `v`'s rect and home the cursor. */
void vc_clear(struct vc* v);

/* Block on `v`'s input ring until a character is available.  Uses
 * sti+hlt + task_yield to be polite to other tasks. */
char vc_getchar(struct vc* v);

/* IRQ-side: push `c` into the focused VC's input ring.  Drops the byte
 * if no VC is focused or the ring is full.  Lock-free under SPSC. */
void vc_kbd_push(char c);

/* Iterate every leaf VC.  Used by the `pane` shell command. */
typedef void (*vc_iter_fn)(struct vc* v, void* ctx);
void vc_for_each(vc_iter_fn fn, void* ctx);
int  vc_count(void);

/* Read the cell-rect of `v`'s current leaf.  Any out pointer may be NULL.
 * Returns 0 on success, -1 if v has no leaf bound. */
int  vc_get_rect(const struct vc* v, int* x, int* y, int* w, int* h);

/* ---- M22 GUI hooks ------------------------------------------------------- */

/* Create a VC that is NOT part of the split tree: no leaf, no rect —
 * all output flows through `emit(ctx, c)`.  Input still uses the normal
 * ring (vc_getchar / vc_kbd_push), so a shell task binds to it exactly
 * like to a pane VC.  Returns NULL when VC_MAX is exhausted. */
struct vc* vc_create_offscreen(void (*emit)(void* ctx, char c), void* ctx);

/* Screen suppression: while on, leaf VCs stop painting the framebuffer
 * (their input rings and shells keep working) and Alt-N pane switching
 * is ignored.  The GUI turns this on when the compositor owns the
 * screen, so background pane output can't scribble over windows. */
void vc_screen_suppress(int on);
int  vc_screen_suppressed(void);

/* Keyboard intercept (M22.1).  Both keyboard drivers (PS/2, USB HID)
 * funnel decoded chars through vc_kbd_push; when a hook is installed
 * and returns non-zero, the byte is consumed BEFORE reaching the
 * focused VC's ring.  The GUI uses this to route typing to widget
 * (non-terminal) windows.  Runs in IRQ context — keep it short. */
void vc_set_kbd_hook(int (*fn)(char c));

#endif
