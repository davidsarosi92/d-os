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
    struct pkg_recipe* next;        /* registry link */
};

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
