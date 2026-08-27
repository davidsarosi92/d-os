/* =============================================================================
 * modload.h — loading a driver that is not in the kernel image (§M67).
 *
 * §M66 made every driver operation work through a slot table rather than an
 * index into the linker's `drivers` section, and added `driver_attach()` as the
 * way in.  This is what fills it: a relocatable ELF object on disk becomes a
 * `struct driver` the rest of the system cannot distinguish from a built-in
 * one.
 *
 * SCOPE, STATED UP FRONT.  A module runs in ring 0 with no isolation — see the
 * long note at the top of modload.c.  The version check refuses a STALE module,
 * not a hostile one, and nothing here is a security boundary.  §M33's execution
 * domains are what would change that.
 * ============================================================================= */

#ifndef MODLOAD_H
#define MODLOAD_H

#include <stdint.h>
#include <stddef.h>

/* Load `path` (a .ko — a relocatable object built against this kernel's
 * headers).  Returns 0 on success.  Every failure path prints a line naming
 * what was wrong; there is no silent refusal, because "insmod did nothing" and
 * "insmod worked" must not look the same.
 *
 * A successful load ATTACHES the driver but does not START it — `drv start`
 * does that, or the next hot-plug rescan when the hardware turns up.  Loading a
 * driver for absent hardware is therefore not an error. */
int  modload_load(const char* path);

/* Stop → detach → free, in that order.  Refuses if the driver will not stop
 * (which means the class registry has a live user), because freeing the image
 * would pull the code out from under whoever is running it. */
int  modload_unload(const char* name);

/* `lsmod`. */
void modload_list(void);

int  modload_is_loaded(const char* name);

/* The `insmod`/`rmmod`/`lsmod` command surface, implemented next to the loader
 * rather than in a shell so the aarch64 serial REPL runs the same one — §M24's
 * rule, which this tree has had to re-learn twice (§4.63's `setconf` and
 * §M24's own network commands). */
void modload_cmd_insmod(const char* args);
void modload_cmd_rmmod(const char* args);

/* Load every .ko under /modules at boot, unless `modules.autoload` says not
 * to.  Idempotent — aarch64 has two boot paths, and §4.63 already paid for a
 * feature that was wired into only one of them. */
void modload_autoload(void);

/* ----------------------------------------------------------------------
 * NOT BUILT, AND THE TRIGGER FOR BUILDING IT.
 *
 * aarch64's B/BL relocation carries a 26-bit word displacement — +-128 MiB.
 * The kernel is linked low in RAM and a module lands wherever the heap is, so
 * on a machine with enough memory a call from a module to an exported kernel
 * function can be out of range.  Today that is REFUSED with a message naming
 * the symbol, not silently truncated.
 *
 * The fix, if a real machine ever hits it, is a veneer pool: allocate a few
 * pages next to the module image, and for each out-of-range target emit
 * `ldr x16, .+8 / br x16 / .quad target`, then point the call at the veneer.
 * It is written down here rather than built because it is untestable on the
 * configurations this tree runs (`-m 512M` puts the whole heap inside the
 * window) — and an untested fallback path is a fallback that does not work.
 * ---------------------------------------------------------------------- */

#endif /* MODLOAD_H */
