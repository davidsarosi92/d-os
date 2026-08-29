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

struct driver;

/* Spawn this driver's ring-3 image and attach it to its manifest.  Called from
 * `drv_init` INSTEAD OF the driver's own init when the resolved domain is
 * DOMAIN_USER — the placement is not a wrapper around the kernel path, it
 * replaces it. */
int  drvuser_launch(const struct driver* d);

/* Mark `pid` as the ring-3 process for `driver_name`, so its resource syscalls
 * are honoured (within the manifest) instead of refused. */
int  drvuser_attach(int pid, const char* driver_name);
void drvuser_detach(int pid);

/* ----------------------------------------------------------------------
 * §M33 Tier 2 — supervision.
 *
 * A ring-3 driver that dies takes its device with it, which is a better failure
 * than taking the machine but is still one.  The supervisor notices, hands the
 * grants back, tells the driver's clients that nothing is happening any more,
 * and re-spawns it — up to `driver.restart_max` times inside 30 s, after which
 * §M66's quarantine applies.
 * ---------------------------------------------------------------------- */

/* Start the watcher task.  Idempotent, and called from the first placement
 * rather than from boot: a machine with every driver in the kernel should not
 * carry a task that watches an empty table. */
void drvuser_supervisor_start(void);

/* Stop a placed driver ON PURPOSE — kill it and do NOT restart it.  This is
 * what `drv stop` must reach for a DOMAIN_USER driver: the in-kernel shutdown
 * hook belongs to a driver that is not the one running. */
int  drvuser_stop(const char* name);

/* Diagnostics.  The pid is what makes "placed in ring 3" checkable from
 * outside, and the restart count is what tells a driver that came back from one
 * that never went away. */
int  drvuser_pid(const char* name);
int  drvuser_restarts(const char* name);

/* Events the CURRENT process has published.  Zeroed by a restart, which is
 * what makes it evidence: "the device works again" is traffic through the new
 * process, and a running total would be satisfied by the dead one's. */
int  drvuser_events(const char* name);

/* 1 once the placed driver holds every grant its bring-up asks for — i.e. it
 * is driving the device, not merely running.  A pid alone is not that. */
int  drvuser_ready(const char* name);

/* 1 when this placed driver's DEVICE is confined to an IOMMU domain holding
 * only that driver's buffers.  The one fact that lets a DMA driver in ring 3 be
 * reported as isolated rather than advisory. */
int  drvuser_confined(const char* name);

/* The syscall bodies.  Arch dispatchers call these; they check the caller. */
long drvuser_sys_ports(uint16_t base, uint16_t count);
long drvuser_sys_irq(int line);
long drvuser_sys_irq_wait(drv_handle h, int timeout_ms);
long drvuser_sys_ports_lock(drv_handle h, int max_ms);
long drvuser_sys_ports_unlock(drv_handle h);
long drvuser_sys_input(int dx, int dy, unsigned buttons, int dz);
long drvuser_sys_log(const char* msg);
long drvuser_sys_window(int bar, uint64_t* out_phys, uint64_t* out_len);
long drvuser_sys_mmio(uint64_t phys, uint64_t len);
long drvuser_sys_dma(int bytes, int addr_bits, uint64_t* out_dev);

#endif /* DRVUSER_H */
