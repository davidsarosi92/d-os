/* =============================================================================
 * pkg.h — package manager & isolation substrate (§M35.5): a content-addressed
 * store on the VFS.
 *
 * The porting-discipline gate that must exist before pulling in foreign code
 * (musl §M36 onward): every package lives in an IMMUTABLE, hash-named,
 * per-version path under /store, with an explicit pinned dependency closure —
 * so the system stays uncluttered, versions coexist without conflict, and a
 * package depends on exactly its declared deps.  Nix/Guix-shaped, NOT
 * dpkg/apt (which mutate a global /usr — the "accidental history" the project
 * rejects, convention #6).
 *
 * This first slice implements the STORE model — content-addressing,
 * immutability, version coexistence, symlink-free profiles, and mark-sweep GC
 * — over built-in recipes whose payload is an embedded blob.  Hermetic builds
 * from source (fetch + a §M33-sandboxed compile) and load-time RPATH isolation
 * (co-designed with §M37) layer on when the toolchain / dynamic linker / build
 * sandbox exist.  See PLAN.md §M35.5.
 *
 * On-disk layout:
 *   /store/<hash>-<name>-<version>/bin/<name>   the payload (immutable)
 *   /store/<hash>-<name>-<version>/.recipe      the text recipe that built it
 *   /store/<hash>-<name>-<version>/.closure     newline list of dep store dirs
 *   /etc/pkg/profile                            installed store dirs (the "view")
 * ============================================================================= */

#ifndef PKG_H
#define PKG_H

#include <stddef.h>

/* A recipe: how to produce one store path.  Built-in recipes register at boot;
 * a text recipe format + source fetch are follow-ups.  `id` is the unique
 * registry key; `deps` is a space-separated list of dep ids. */
struct pkg_recipe {
    const char* id;
    const char* name;
    const char* version;
    const char* deps;
    const void* content;            /* payload written to bin/<name>, or NULL */
    unsigned    content_len;
    /* The ABI/personality the payload targets ("native" | "linux" | future
     * "bsd"/…).  Written to <store>/.abi at build; the runner maps it to a
     * kernel personality at exec — the single swappable seam (see pkg_run).
     * NULL ⇒ "native".  This keeps the libc/ABI choice DATA-DRIVEN: a package
     * DECLARES its ABI; the exec path never hardcodes "musl"/"linux". */
    const char* abi;
    /* If non-NULL, this package is a SHARED LIBRARY, not a program: its payload
     * materialises at <store>/lib/<soname> and the "profile view" exposes it to
     * /lib/<soname> (not /bin/<name>).  This is how the runtime C library
     * (musl) itself becomes a versioned, content-addressed, pkg-managed package
     * — multiple libc versions coexist in the store, switching = re-point the
     * active view, updating = `pkg install` a newer recipe.  See
     * pkg_activate_libc + [[feedback-dos-swappable-layers]]. */
    const char* soname;
    /* If non-zero, this library is the C library / dynamic linker: activating
     * it ALSO writes the fixed PT_INTERP alias (/lib/ld-musl-<arch>.so.1). */
    int         is_libc;
    struct pkg_recipe* next;        /* registry link */
};

/* Switch the ACTIVE C library to an installed musl (or other libc) package:
 * re-provisions /lib/libc.so + the PT_INTERP alias FROM that store path.  With
 * two libc versions installed (coexisting in the store) this is how you switch
 * between them — the swappable/updatable-libc seam the store model gives us. */
int  pkg_libc_use(const char* id);

/* ---------------------------------------------------------------------------
 * Pluggable package-manager BACKEND.  The package manager is itself a modular,
 * swappable component (same philosophy as DRIVER()/SERVICE()/SHELL_PROVIDER()):
 * a stable `struct pkg_ops` contract with the content-addressed store as the
 * DEFAULT backend.  A different implementation (e.g. a remote/apt-style or a
 * networked backend) registers its own ops and becomes active; the user-facing
 * `pkg`/`pkgrun` commands dispatch through `pkg_backend_active()`.  `version`
 * versions the BACKEND itself (everything is versioned so it can be updated).
 * --------------------------------------------------------------------------- */
struct pkg_ops {
    const char* name;
    const char* version;
    int  (*build)(const char* id);
    int  (*install)(const char* id);
    int  (*remove)(const char* id);
    int  (*run)(int argc, const char* const argv[]);
    int  (*gc)(void);
    void (*list)(void);
    void (*why)(const char* id);
    int  (*libc_use)(const char* id);
};
/* Register a backend and make it the active one (last registration wins; a
 * `pkg.backend` config key can select among registered backends by name). */
void pkg_backend_register(const struct pkg_ops* ops);
const struct pkg_ops* pkg_backend_active(void);

/* Register a recipe (called from pkg_init for the built-ins). */
void pkg_register(struct pkg_recipe* r);

/* Set up the store (/store, /etc/pkg) + register the built-in recipes.  Call
 * once at boot after the VFS is up. */
void pkg_init(void);

/* Build recipe `id` (+ its deps) into the store if absent; content-addressed,
 * so identical inputs reuse the same immutable path.  Returns 0 on success. */
int  pkg_build(const char* id);

/* Build then add to the profile ("install" into the visible view). */
int  pkg_install(const char* id);

/* Execute an INSTALLED package's binary from the store, argv[0] = program
 * name (resolved to a store path via the profile), passing argc/argv through.
 * The package's declared ABI (its <store>/.abi) selects the exec personality.
 * Returns the program's exit code, or negative on failure. */
int  pkg_run(int argc, const char* const argv[]);

/* Drop from the profile (the store path survives until pkg_gc). */
int  pkg_remove(const char* id);

/* Mark-sweep: delete every /store path not reachable from the profile's
 * closure.  Returns the number of paths reclaimed. */
int  pkg_gc(void);

/* Diagnostics (shell `pkg`). */
void pkg_list(void);                /* store paths + which are installed  */
void pkg_why(const char* id);       /* the pinned dependency closure       */

#endif /* PKG_H */
