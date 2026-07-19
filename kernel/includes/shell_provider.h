/* =============================================================================
 * shell_provider.h — command-shell provider registry (§S.1).
 *
 * The deferred half of PLAN §S: the console side (struct vc + per-task
 * emit routing) shipped with M14, but every spawn site kept hard-wiring
 * `extern void shell_task_entry(void)`.  This registry finishes the
 * cut: a shell implementation is a REGISTRATION, and the three spawn
 * paths (boot shell in kernel.c, `pane split`, GUI terminal windows)
 * resolve the active provider through the `shell.provider` config key.
 *
 * A provider entry function is an ordinary task entry: it finds its
 * console via task_current()->out_console (a struct vc*, works for
 * both panes and GUI terminal windows) and never returns.
 *
 * Same linker-section pattern as MODULE()/GUI_APP()/DESKTOP_SHELL().
 * ============================================================================= */

#ifndef SHELL_PROVIDER_H
#define SHELL_PROVIDER_H

#include "version.h"        /* DOS_VERSION — the default per-provider version */

struct shell_provider {
    const char* name;               /* config value, e.g. "d-os", "rescue" */
    void      (*entry)(void);       /* task entry — never returns          */
    const char* version;            /* defaults to DOS_VERSION (see SHELL_PROVIDER) */
};

extern struct shell_provider __start_shell_providers[];
extern struct shell_provider __stop_shell_providers[];

#define SHELL_PROVIDER(_name, _entryfn)                                  \
    static const struct shell_provider                                   \
    __attribute__((used, section("shell_providers"), aligned(4)))        \
    __shell_provider_##_entryfn = {                                      \
        .name    = (_name),                                              \
        .entry   = (_entryfn),                                           \
        .version = DOS_VERSION,                                          \
    }

/* Registry walk (trivial inlines — boundary arithmetic in one place). */
static inline int shell_provider_count(void) {
    return (int)(__stop_shell_providers - __start_shell_providers);
}
static inline const struct shell_provider* shell_provider_at(int i) {
    if (i < 0 || i >= shell_provider_count()) return (const struct shell_provider*)0;
    return &__start_shell_providers[i];
}

/* The active provider: `shell.provider` config value matched by name
 * (case-sensitive — config values are typed by the user as-is), with
 * "d-os" then entry 0 as fallbacks.  Implemented in shell.c (it owns
 * the config dependency).  Never returns NULL once shell.c is linked. */
const struct shell_provider* shell_provider_active(void);

#endif
