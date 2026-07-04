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

#endif
