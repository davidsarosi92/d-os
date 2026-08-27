/* =============================================================================
 * module_abi.h — the contract between the kernel and a loadable module (§M67).
 *
 * Included by BOTH sides: the kernel compiles it into the loader, and a module
 * compiles it into its own object.  That is the entire mechanism — each side
 * computes the same facts from the headers IT was built against, and the loader
 * compares them before it runs a single instruction of module code.
 *
 * -----------------------------------------------------------------------------
 * WHY A VERSION CHECK IS THE FIRST THING BUILT, NOT THE LAST
 *
 * A module built against an older `struct driver` layout does not crash on
 * load.  It reads the wrong offsets — `name` where `class` used to be, an `ops`
 * pointer taken from the middle of another field — and then calls through it.
 * The machine dies somewhere unrelated, or worse, does not die and corrupts
 * something quietly.  §M51's lesson in a form the build cannot catch, because
 * the two halves are compiled at different times and possibly from different
 * source trees.
 *
 * So there are two checks, and NEITHER IS SUFFICIENT ALONE:
 *
 *   1. A STRUCTURAL FINGERPRINT — the sizes of every struct a module can see,
 *      computed by the compiler on both sides from the real headers.  Nobody
 *      has to remember to update it: adding a field to `struct driver` changes
 *      the number automatically.  It catches LAYOUT changes, which is the
 *      common case and the one with the nastiest failure mode.
 *
 *   2. `DOS_MODULE_ABI`, a number bumped BY HAND.  It exists because a
 *      fingerprint cannot see SEMANTICS: give a field the same size and a new
 *      meaning, or change what a function returns on failure, and every size
 *      still matches while every module built before the change is wrong.
 *
 * The fingerprint is the one that cannot be forgotten, which is why it carries
 * the weight.  The hand-bumped number is the one that covers what the compiler
 * cannot see, and its cost is honest: it is a convention that must be
 * remembered, and this project has written down twice what those are worth
 * (§M65: "a documented convention that must be remembered is a bug
 * generator").  It is here anyway, because the alternative to an imperfect
 * semantic check is no semantic check.
 *
 * -----------------------------------------------------------------------------
 * WHAT IS *NOT* COVERED, SAID PLAINLY
 *
 * Only the structs listed in MODULE_ABI_STRUCTS() below are fingerprinted.  A
 * module that registers into a class registry sees that class's structs too
 * (an audio driver sees `struct audio_dev`), so a class that wants coverage
 * ADDS ONE LINE to that list.  A class that does not appear there is protected
 * by nothing but `DOS_MODULE_ABI`.
 * ============================================================================= */

#ifndef MODULE_ABI_H
#define MODULE_ABI_H

#include <stdint.h>
#include "version.h"
#include "driver.h"
#include "ksym.h"
#include "audio.h"

/* -----------------------------------------------------------------------------
 * THE HAND-MAINTAINED HALF.
 *
 * Bump this when a module-visible thing changes MEANING without changing SIZE.
 * Examples of what requires a bump: a function that used to return 0 on failure
 * now returning a negative errno; a field that kept its width and changed
 * units; an exported symbol whose calling convention changed.
 *
 * You do NOT need to bump it for a struct listed below getting a new field —
 * the fingerprint catches that by itself.  Nor for adding an export; a module
 * that does not use it does not care, and one that does would not have built.
 *
 * ABI 2 (§M67, the day it shipped): `driver_ops.shutdown` changed from
 * `void (*)(void*)` to `int (*)(void*)` so a class that REFUSES to be withdrawn
 * can say so.  A textbook case for this number rather than the fingerprint: a
 * function pointer is a function pointer, so every struct size stayed identical
 * and the automatic check saw nothing at all.  A module built against ABI 1
 * would have had its refusal read as success.
 * -------------------------------------------------------------------------- */
#define DOS_MODULE_ABI  2

#define MODULE_ABI_MAGIC 0x444F534Du    /* "DOSM" */

/* -----------------------------------------------------------------------------
 * THE AUTOMATIC HALF.
 *
 * One line per struct a module can see.  Both sides expand this list with their
 * own `sizeof`, so a mismatch names WHICH struct moved rather than reporting an
 * opaque hash that differs — the difference between "rebuild your module" and
 * "rebuild your module because `struct audio_dev` grew".
 * -------------------------------------------------------------------------- */
/* The arch-specific tail.  A PCI driver sees `struct pci_device`, and PCI does
 * not exist on every target — so the list has an arch-conditional part, and the
 * COUNT legitimately differs between arches.  That costs nothing, because a
 * module built for another machine is refused by its ELF header long before the
 * fingerprint is consulted. */
#if defined(__i386__) || defined(__x86_64__)
#include "pci.h"
#  define MODULE_ABI_ARCH_STRUCTS(X)  X(pci_device)
#else
#  define MODULE_ABI_ARCH_STRUCTS(X)
#endif

#define MODULE_ABI_STRUCTS(X)   \
    X(driver)                   \
    X(driver_ops)               \
    X(ksym)                     \
    X(audio_dev)                \
    MODULE_ABI_ARCH_STRUCTS(X)

#define MODULE_ABI_STRUCT_COUNT_ONE(_s) + 1
#define MODULE_ABI_NSTRUCTS (0 MODULE_ABI_STRUCTS(MODULE_ABI_STRUCT_COUNT_ONE))

/* The fingerprint as the compiler sees it on THIS side of the boundary. */
#define MODULE_ABI_SIZEOF_ONE(_s) (uint16_t)sizeof(struct _s),
#define MODULE_ABI_FINGERPRINT { MODULE_ABI_STRUCTS(MODULE_ABI_SIZEOF_ONE) }

/* Names, for the diagnostic.  Kept next to the sizes so the two cannot get out
 * of step — a mismatch report that names the wrong struct is worse than one
 * that names none. */
#define MODULE_ABI_NAME_ONE(_s) #_s,
#define MODULE_ABI_NAMES { MODULE_ABI_STRUCTS(MODULE_ABI_NAME_ONE) }

/* -----------------------------------------------------------------------------
 * The descriptor every module carries, in its own `.dosmod` section.
 *
 * NOTE THE SPLIT, it is load-bearing.  Everything above `driver` is a SCALAR,
 * so the loader can read it straight out of the file with no relocation
 * applied.  That is what lets a module be REFUSED before any memory is
 * allocated for it and before any of its relocations are processed — the check
 * happens while the module is still inert bytes.  The two pointers at the end
 * are meaningless until relocation, and are read only afterwards.
 * -------------------------------------------------------------------------- */
struct module_abi {
    uint32_t magic;                             /* MODULE_ABI_MAGIC            */
    uint32_t abi;                               /* DOS_MODULE_ABI              */
    uint16_t word_bytes;                        /* sizeof(void*) — a 32/64 mix */
    uint16_t nstructs;                          /* MODULE_ABI_NSTRUCTS         */
    uint16_t sizes[MODULE_ABI_NSTRUCTS];        /* the fingerprint             */
    char     built_for[16];                     /* DOS_VERSION, informational  */
    char     modname[24];                       /* what `lsmod` shows          */

    /* --- relocated: do not read before the relocations are applied --- */
    struct driver* driver;                      /* the descriptor to attach    */
    int          (*mod_init)(void);             /* optional, may be NULL       */
    void         (*mod_exit)(void);             /* optional, may be NULL       */
};

/* -----------------------------------------------------------------------------
 * DOS_MODULE() — what a module writes exactly once.
 *
 * `_drv` is the module's `struct driver` (it may be NULL for a module that is
 * not a driver at all).  `_init` / `_exit` are optional hooks for a module that
 * needs to do something either side of registration; a plain driver leaves them
 * NULL and lets §M66's lifecycle do the work, which is the whole reason that
 * milestone came first.
 * -------------------------------------------------------------------------- */
#define DOS_MODULE(_name, _drv, _init, _exit)                             \
    const struct module_abi                                               \
    __attribute__((used, section(".dosmod")))                             \
    __dos_module_abi = {                                                  \
        .magic      = MODULE_ABI_MAGIC,                                   \
        .abi        = DOS_MODULE_ABI,                                     \
        .word_bytes = (uint16_t)sizeof(void*),                            \
        .nstructs   = (uint16_t)MODULE_ABI_NSTRUCTS,                      \
        .sizes      = MODULE_ABI_FINGERPRINT,                             \
        .built_for  = DOS_VERSION,                                        \
        .modname    = _name,                                              \
        .driver     = (_drv),                                             \
        .mod_init   = (_init),                                            \
        .mod_exit   = (_exit),                                            \
    }

#endif /* MODULE_ABI_H */
