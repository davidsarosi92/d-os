/* =============================================================================
 * shortcut.h — desktop shortcuts (§M64).  Icons on the wallpaper, and nothing
 * more: a shortcut is a POINTER to something that already exists — a name, a
 * target and a position.  No embedded app, no live tiles, no widgets.
 *
 * A SHORTCUT IS A FILE, not a config key.  `/desktop/<name>.lnk`, one
 * `key = value` per line (the config file format, deliberately, so there is
 * one text format in this system rather than two):
 *
 *     name  = NetSurf
 *     target= app:NetSurf
 *     icon  = globe
 *     x     = 0
 *     y     = 0
 *
 * Files, because the system already has tools for files: the file manager can
 * create one, `ls` lists them, `rm` deletes one, and the exFAT mount can hold
 * them so they survive a reboot (§M63 stage 0 did the same for settings).
 * Config keys would need a parallel CRUD and deleting one would mean editing a
 * key namespace by hand.
 *
 * FOUR TARGET KINDS, ONE RESOLVER — so "double-click a shortcut" and
 * "double-click in the file manager" cannot drift apart:
 *
 *     app:<name>     a registered GUI_APP, by NAME — no path, survives a
 *                    rebuild, and the common case
 *     run:<cmdline>  a shell command line
 *     file:<path>    open through the existing GUI_APP_ASSOC association
 *     store:<pkg>    a package in the §M35.5 store  (reserved; today it maps
 *                    onto run: so the spelling exists before the plumbing)
 *
 * Deliberately NOT done: auto-population from the GUI_APP registry.  A desktop
 * that re-creates icons the user deleted is the single most-complained-about
 * behaviour of every system that has tried it.  Ships empty; `shortcut add`
 * and the file manager put things there.
 * ============================================================================= */

#ifndef SHORTCUT_H
#define SHORTCUT_H

#define SHORTCUT_DIR    "/desktop"
#define SHORTCUT_MAX    64          /* bounded like every other table here */

struct item_model;

/* Re-read `/desktop` into the in-memory list.  Cheap and idempotent; called at
 * GUI start and after any change.  Returns the number of shortcuts. */
int  shortcut_reload(void);

/* The item model over the loaded shortcuts — hand this to any item_view. */
const struct item_model* shortcut_model(void);

/* Launch shortcut `index`.  Runs on an ordinary task (never the mouse IRQ):
 * it may spawn, allocate and touch the VFS. */
void shortcut_launch(int index);

/* Create / delete.  `spec` is the target string above.  Returns 0 on success. */
int  shortcut_add(const char* name, const char* target, const char* icon);
int  shortcut_remove(const char* name);

/* Persist the position of shortcut `index` (drag-to-move, once §M58's
 * press/motion/release plumbing exists).  Returns 0 on success. */
int  shortcut_set_pos(int index, int x, int y);

/* The `shortcut` shell command — implemented next to the model rather than in
 * a shell, so the ARM serial REPL runs the same one (§M24's rule). */
void shortcut_cmd(const char* args);

#endif
