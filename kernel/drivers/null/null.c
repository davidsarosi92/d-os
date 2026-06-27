/* =============================================================================
 * null.c — the canonical "do-nothing" character device.
 *
 * Today this serves two purposes:
 *   1. It's the first concrete user of the new DRIVER() framework (M8) — its
 *      presence in `lsdrv` proves the linker section + iteration work.
 *   2. It's a placeholder for `/dev/null` once devfs (M9) lands.  At that
 *      point this file gains a `read` (always returns EOF) and `write`
 *      (always succeeds, drops bytes) adapter into the devfs node table.
 *
 * Because `/dev/null` has no hardware to detect, both probe and init are
 * trivially successful.  shutdown is left NULL — there's nothing to undo.
 * ============================================================================= */

#include "driver.h"
#include <stddef.h>

static int null_probe(void* ctx) { (void)ctx; return 0; }
static int null_init (void* ctx) { (void)ctx; return 0; }

static const struct driver_ops null_ops = {
    .probe    = null_probe,
    .init     = null_init,
    .shutdown = NULL,
};

DRIVER(null, "char", &null_ops, NULL);
