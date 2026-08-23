/* =============================================================================
 * gui_app.h — GUI application registry (M22.2).
 *
 * Same self-registration pattern as MODULE()/DRIVER(): each app drops
 * a `struct gui_app_def` into the `gui_apps` linker section, and the
 * consumers walk [__start_gui_apps, __stop_gui_apps):
 *
 *   - the active desktop shell builds its launcher (Start menu) from it,
 *   - the `launch` shell command lists / starts entries by name.
 *
 * Nothing references an app by symbol — swapping the file manager for
 * a different implementation is purely a Makefile change (link the
 * other .c that registers under the same or a new name).
 *
 * `launch` runs either on the compositor task (menu click) or on a
 * shell task (`launch` command).  Both are ordinary task contexts, so
 * an app may freely use the VFS, kmalloc and the gui/widget APIs; it
 * must NOT assume it is single-instance unless it guards itself (see
 * fileman.c's singleton pattern).
 *
 * Alignment note: aligned(4) mirrors MODULE() — it can only raise
 * alignment, so on x86_64 the natural 8-byte member alignment still
 * wins and the section stride equals sizeof(struct gui_app_def) on
 * both archs (see the module.h lesson about section walking).
 * ============================================================================= */

#ifndef GUI_APP_H
#define GUI_APP_H

struct gui_app_def {
    const char* name;               /* launcher label, e.g. "File Manager" */
    void      (*launch)(void);      /* open (or raise) the app             */
    /* M22.5 — optional file-type association.  When non-NULL, the file
     * manager (or any other opener) may hand this app an absolute path
     * to open.  `extensions` is a space-separated, lowercase list of
     * extensions (no dots) the app claims, e.g. "txt conf md". */
    void      (*open_path)(const char* path);
    const char* extensions;
    /* §M64 — an `enum icon_id` (icons.h).  Zero (ICON_NONE) means "not
     * stated", and every consumer substitutes ICON_APP for it: adding the
     * field must not turn every existing registration into a blank tile. */
    int         icon;
};

extern struct gui_app_def __start_gui_apps[];
extern struct gui_app_def __stop_gui_apps[];

#define GUI_APP(_name, _launchfn)                                       \
    static const struct gui_app_def                                     \
    __attribute__((used, section("gui_apps"), aligned(4)))              \
    __gui_app_##_launchfn = {                                           \
        .name   = (_name),                                              \
        .launch = (_launchfn),                                          \
    }

/* §M64 — registration with an ICON.  A separate macro rather than an extra
 * argument to GUI_APP so the dozen existing call sites keep compiling: the
 * registry is a struct with designated initialisers, and an app that never
 * says anything about icons still gets a sensible one. */
#define GUI_APP_ICON(_name, _launchfn, _icon)                           \
    static const struct gui_app_def                                     \
    __attribute__((used, section("gui_apps"), aligned(4)))              \
    __gui_app_##_launchfn = {                                           \
        .name   = (_name),                                              \
        .launch = (_launchfn),                                          \
        .icon   = (_icon),                                              \
    }

/* M22.5 — registration WITH a file-type association. */
#define GUI_APP_ASSOC(_name, _launchfn, _openfn, _exts)                 \
    static const struct gui_app_def                                     \
    __attribute__((used, section("gui_apps"), aligned(4)))              \
    __gui_app_##_launchfn = {                                           \
        .name       = (_name),                                          \
        .launch     = (_launchfn),                                      \
        .open_path  = (_openfn),                                        \
        .extensions = (_exts),                                          \
    }

/* …and with both. */
#define GUI_APP_ASSOC_ICON(_name, _launchfn, _openfn, _exts, _icon)     \
    static const struct gui_app_def                                     \
    __attribute__((used, section("gui_apps"), aligned(4)))              \
    __gui_app_##_launchfn = {                                           \
        .name       = (_name),                                          \
        .launch     = (_launchfn),                                      \
        .open_path  = (_openfn),                                        \
        .extensions = (_exts),                                          \
        .icon       = (_icon),                                          \
    }

/* Registry walk helpers (implemented in gui.c — trivial, but keep the
 * boundary-symbol arithmetic in one place). */
int  gui_app_count(void);
const struct gui_app_def* gui_app_at(int idx);
const struct gui_app_def* gui_app_find(const char* name);   /* case-insens. */

/* M22.5 — find the app registered for `path`'s extension (matched
 * case-insensitively against each app's `extensions` list).  Returns
 * NULL if nothing claims it. */
const struct gui_app_def* gui_app_for_path(const char* path);

#endif
