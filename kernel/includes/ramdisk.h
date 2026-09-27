/* =============================================================================
 * ramdisk.h — block devices made of RAM (§M87).  See kernel/drivers/block/
 * ramdisk.c for why they exist.
 * ============================================================================= */
#ifndef RAMDISK_H
#define RAMDISK_H

#include <stdint.h>

/* Create a zero-filled RAM disk of `mib` MiB (1..256), registered as a block
 * device.  Returns its name ("ram0".."ram3") or NULL (no slot, not enough
 * memory — a quarter of RAM is always kept free). */
const char* ramdisk_create(uint32_t mib);

/* Destroy one.  The CALLER checks it is not mounted.  0, or -1 if unknown. */
int ramdisk_destroy(const char* name);

/* Is `name` a RAM disk (and therefore removable)? */
int ramdisk_is(const char* name);

#endif
