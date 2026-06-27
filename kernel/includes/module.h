/* =============================================================================
 * module.h — driver registration framework.
 *
 * The goal is the rule from PLAN.md §North-star: adding a new driver
 * means dropping a `.c` file with one `MODULE(...)` line — never editing
 * `kernel_main`.  We achieve that with a Linux-style trick:
 *
 *   1. Each `MODULE(...)` instantiation places a `struct module_def`
 *      into a dedicated linker section called `modules`.
 *   2. linker.ld places that section at a known location and exposes
 *      `__start_modules` / `__stop_modules` symbols for its bounds.
 *   3. At boot, `module_init_all()` walks the array and calls each
 *      module's init function.
 *
 * The class field is metadata.  Per-class registries (console sinks,
 * input drivers, ...) are independent — a module's init function is
 * what wires it into them.  This keeps the module framework itself
 * tiny while letting each class evolve at its own pace.
 *
 * Init order is the link order of the object files.  In practice this
 * is the order they appear in the Makefile, which is stable.
 * ============================================================================= */

#ifndef MODULE_H
#define MODULE_H

#include <stdint.h>

struct module_def {
    const char* name;           /* short identifier, e.g. "vesa-fb" */
    const char* class;          /* "console", "input", "fs", "shell", "core" */
    int       (*init)(void);    /* return 0 on success */
};

/* Boundary symbols emitted by linker.ld around the modules section. */
extern struct module_def __start_modules[];
extern struct module_def __stop_modules[];

/* Walk the module array and run every init function in declaration
 * order.  Failures (non-zero return) are logged but don't abort — a
 * dead module shouldn't take down the whole boot. */
void module_init_all(void);

/* Print every registered module to the console — used by the `lsmod`
 * shell command. */
void module_list(void);

/* Register a `struct module_def` with the linker section.
 *
 * Macro hygiene notes:
 *   - The wrapper struct is `static` so each TU gets its own copy.
 *   - `__used` keeps it through link-time garbage collection.
 *   - `aligned(8)` matches the padding the kernel iteration uses; without
 *     it, packing differences across object files would misalign the
 *     stride.
 *   - `_initfn` (not `_class`) is concatenated into the symbol name so
 *     two modules in the same class don't collide in the same TU. */
/* aligned(4) (not 8) is critical here: `sizeof(struct module_def) == 12`
 * on 32-bit, and the kernel iterates the section with a `++` of stride
 * `sizeof(*m)`.  An `aligned(8)` would round each entry up to 16 bytes
 * in the section — and the iterator would walk 12, then read garbage at
 * an unaligned offset, then page-fault.  Keep the natural 4-byte
 * alignment so stride and sizeof agree. */
#define MODULE(_name, _class, _initfn)                                  \
    static const struct module_def                                      \
    __attribute__((used, section("modules"), aligned(4)))               \
    __module_def_##_initfn = {                                          \
        .name  = (_name),                                               \
        .class = (_class),                                              \
        .init  = (_initfn),                                             \
    }

#endif
