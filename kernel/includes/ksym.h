/* =============================================================================
 * ksym.h — the kernel's exported symbol table (§M67).
 *
 * A loadable module calls `kprintf`, `kmalloc`, `audio_register`.  Those names
 * are undefined in the module's object file, so the loader has to turn each of
 * them into an address.  This is where the addresses come from.
 *
 * WHY THIS IS A REGISTRY AND NOT A GENERATED TABLE.  §M67's plan said the table
 * would be generated at build time by scraping the linked kernel with `nm`,
 * the way Linux's kallsyms works.  That approach has two problems this one does
 * not.  It needs a MULTI-PASS LINK (the table's size changes every address it
 * records, so the second link invalidates the first), and — the real objection
 * — it makes the export surface ACCIDENTAL: every non-static function in the
 * tree becomes part of a contract nobody decided on, and removing a helper
 * silently breaks a module.
 *
 * `EXPORT_SYMBOL()` is the same linker-section trick every other registry here
 * uses (DRIVER(), CONFIG_KEY(), SETTINGS_PANEL(), CRASH_SINK()), and it makes
 * the answer to "what may a module call?" a LIST SOMEBODY WROTE.  Deleting an
 * export is then a visible act with a visible consequence, which is the whole
 * point: this table is a promise that has to be kept for as long as modules
 * exist.  See ksym.c for what is in it and why.
 *
 * NOTE ON SCOPE: this table is for MODULES, not for debugging.  Symbolising a
 * crash address is a different job with a different requirement (it wants EVERY
 * symbol, including static ones), and that already lives outside the kernel in
 * scripts/dos-sym.sh.  Conflating the two would force one of them to carry the
 * other's cost.
 * ============================================================================= */

#ifndef KSYM_H
#define KSYM_H

#include <stddef.h>

/* One exported name.  Two pointers, so `sizeof` is 2 * sizeof(void*) on every
 * arch and the section stride matches naturally — see the alignment note on the
 * macro below. */
struct ksym {
    const char* name;
    void*       addr;
};

/* Section bounds emitted by the linker script (all three of them). */
extern const struct ksym __start_ksyms[];
extern const struct ksym __stop_ksyms[];

/* Resolve an exported name to its address.  Returns NULL if the name is not
 * exported — which the loader must treat as a hard failure, never as zero:
 * a module that calls through a NULL pointer takes the machine down at the
 * first use, arbitrarily far from the load that caused it. */
void* ksym_lookup(const char* name);

/* How many symbols are exported (diagnostics, `ksyms` shell command). */
int ksym_count(void);

/* Print the table.  Backs `ksyms` — worth having because "the module failed to
 * load, symbol not found" is only actionable next to the list of what IS
 * available. */
void ksym_list(const char* filter);

/* -----------------------------------------------------------------------------
 * The macro.
 *
 * `aligned(sizeof(void*))` rather than a literal: the struct is two pointers,
 * so its size is 8 on i386 and 16 on the 64-bit arches, and the alignment must
 * DIVIDE that or the iterator's stride and the linker's padding disagree.  M2's
 * lesson — alignment greater than sizeof leaves gaps the walk reads as entries.
 *
 * `&(_sym)` and not `(_sym)`, which matters for the two kinds of export: on a
 * FUNCTION the two are the same thing, but on a DATA object `(_sym)` is its
 * VALUE and only `&(_sym)` is its address.  Writing the version that happens to
 * work for functions would export every variable's contents as if they were a
 * pointer — silently, since the cast makes it legal.
 *
 * The trip through `unsigned long` is deliberate too: converting a function
 * pointer directly to `void*` is a constraint violation the compiler may
 * complain about, even though every arch we target makes it meaningful.
 * -------------------------------------------------------------------------- */
#define EXPORT_SYMBOL(_sym)                                               \
    static const struct ksym                                              \
    __attribute__((used, section("ksyms"), aligned(sizeof(void*))))       \
    __ksym_def_##_sym = {                                                 \
        .name = #_sym,                                                    \
        .addr = (void*)(unsigned long)&(_sym),                            \
    }

#endif /* KSYM_H */
