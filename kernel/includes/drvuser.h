/* =============================================================================
 * drvuser.h — §M33 Tier 1: the kernel side of a ring-3 driver.
 *
 * See drvuser.c for what is and is not isolation here, and for why a grant is
 * bounded by a kernel-side MANIFEST rather than by whatever the driver asks
 * for.
 * ============================================================================= */

#ifndef DRVUSER_H
#define DRVUSER_H

#include <stdint.h>
#include "drvrt.h"

struct drv_manifest;

/* Is there a manifest for this driver — i.e. may it be placed in ring 3 at
 * all?  `domain_enforceable` and this answer agree by construction: a driver
 * with no manifest has no bound, and a placement with no bound is not one. */
int  drvuser_placeable(const char* name);
const struct drv_manifest* drvuser_manifest(const char* name);

/* Mark `pid` as the ring-3 process for `driver_name`, so its resource syscalls
 * are honoured (within the manifest) instead of refused. */
int  drvuser_attach(int pid, const char* driver_name);
void drvuser_detach(int pid);

/* The syscall bodies.  Arch dispatchers call these; they check the caller. */
long drvuser_sys_ports(uint16_t base, uint16_t count);
long drvuser_sys_irq(int line);
long drvuser_sys_irq_wait(drv_handle h, int timeout_ms);
long drvuser_sys_input(int dx, int dy, unsigned buttons, int dz);

#endif /* DRVUSER_H */
