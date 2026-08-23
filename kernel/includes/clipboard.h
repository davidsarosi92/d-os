/* =============================================================================
 * clipboard.h — kernel-global text clipboard (M22.5).
 *
 * One shared text slot for the whole system: the editor widget, the
 * single-line textinput and any future consumer copy/paste through it
 * (Ctrl+C/X/V).  Deliberately tiny — no history, no formats, text
 * only.  A userland clipboard protocol arrives with §M25/§M26; this
 * API is shaped so those can layer on top (set/get with explicit
 * lengths, no global buffer exposure).
 *
 * Concurrency: guarded by an internal spinlock.  Today every caller
 * runs on the compositor task, but per the SMP-ready convention the
 * lock is real from day one.
 * ============================================================================= */

#ifndef CLIPBOARD_H
#define CLIPBOARD_H

/* Replace the clipboard content with `text[0..len)`.  len < 0 means
 * "text is NUL-terminated, measure it".  Returns 0, or -1 on OOM. */
int clipboard_set(const char* text, int len);

/* Copy up to cap-1 bytes into dst, always NUL-terminating.  Returns
 * the number of bytes copied (clipboard may be longer — check
 * clipboard_len when exactness matters). */
int clipboard_get(char* dst, int cap);

/* Current content length in bytes (0 = empty). */
int clipboard_len(void);

/* ---------------------------------------------------------------------
 * §M59 — the PRIMARY SELECTION: a second slot holding whatever was last
 * SELECTED, as opposed to what was deliberately COPIED.
 *
 * They are different intentions, and one slot cannot hold both: every
 * drag across a terminal would destroy whatever the user had copied on
 * purpose.  So §M58's selection fills PRIMARY, Ctrl+C fills the
 * clipboard, and a paste site chooses which it wants (middle-click =
 * primary, Ctrl+V = clipboard).  `clip promote` moves one to the other,
 * because "I meant that one" should be a decision, not a side effect.
 * --------------------------------------------------------------------- */
int clipboard_set_primary(const char* text, int len);

/* §M59 — the same setters, carrying the offer's TYPE (a short MIME-shaped
 * string; NULL or "" means text/plain).  A paste target that cannot use a type
 * should refuse rather than guess: guessing from the bytes fails silently on
 * exactly the cases that matter. */
int clipboard_set_typed(const char* text, int len, const char* type);
int clipboard_set_primary_typed(const char* text, int len, const char* type);
const char* clipboard_type(void);
const char* clipboard_primary_type(void);
int clipboard_get_primary(char* dst, int cap);
int clipboard_primary_len(void);

/* The `clip` command — implemented in clipboard.c so both shells run one
 * copy (§M24's rule). */
void clipboard_cmd(const char* args);

/* Register /dev/clipboard.  Called from devfs bring-up; a write at offset 0
 * replaces the contents, a write further on appends, a read is a byte range. */
void clipboard_devfs_init(void);

#endif
