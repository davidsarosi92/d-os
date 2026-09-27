/* =============================================================================
 * exfat.h — what the rest of the kernel may ask the exFAT driver directly
 * (§M87).  Mounting goes through the VFS (`vfs_mount("exfat", ...)`); this is
 * only for the operation that happens to a disk BEFORE it has a filesystem.
 * ============================================================================= */
#ifndef EXFAT_H
#define EXFAT_H

struct block_device;

/* Write a fresh, empty exFAT filesystem over the WHOLE of `dev` (no partition
 * table — the same whole-disk layout mkfs.exfat gives this project's test
 * images).  `label` may be NULL; at most 11 characters are kept.
 *
 * The caller guarantees `dev` is not mounted.  0 on success; -1 unsupported
 * sector size, -2 too small, -3 out of memory, -4 I/O error (the disk is then
 * in an undefined state and should be formatted again). */
int exfat_format(struct block_device* dev, const char* label);

#endif
