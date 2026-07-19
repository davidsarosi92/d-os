/* =============================================================================
 * module.c — driver registry init iteration.
 *
 * Walks `__start_modules`..`__stop_modules` (provided by linker.ld) and
 * invokes each `struct module_def`'s init function.  See module.h and
 * PLAN.md §M2 for design rationale.
 *
 * Right now there is exactly one init phase.  When more boot ordering
 * becomes necessary (e.g. fs modules after VFS skeleton, devices after
 * resource discovery), this file grows phase tags and per-phase walks.
 * ============================================================================= */

#include "module.h"
#include "printf.h"

void module_init_all(void) {
    int total = 0, ok = 0, failed = 0;
    for (struct module_def* m = __start_modules; m < __stop_modules; m++) {
        total++;
        if (!m->init) continue;
        int r = m->init();
        if (r == 0) {
            ok++;
        } else {
            failed++;
            kprintf("module: %s (%s) init failed: %d\n", m->name, m->class, r);
        }
    }
    kprintf("modules: %d registered, %d ok, %d failed\n", total, ok, failed);
}

void module_list(void) {
    kprintf("registered modules (%d total):\n",
            (int)(__stop_modules - __start_modules));
    for (struct module_def* m = __start_modules; m < __stop_modules; m++) {
        kprintf("  [%s] %s v%s\n", m->class ? m->class : "?", m->name,
                m->version ? m->version : "?");
    }
}
