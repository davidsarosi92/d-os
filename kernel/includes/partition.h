/* =============================================================================
 * partition.h — partition tables on block devices (MBR and GPT).
 *
 * A disk that carries a partition table gets one extra block device per
 * partition, registered right after the disk: vda -> vda1, vda2 … (a disk
 * whose name ends in a digit, nvme0n1, gets nvme0n1p1).  A partition device
 * is the disk seen through a window — reads and writes are offset by the
 * partition's first sector and REFUSED past its end — so everything above the
 * block layer (the cache, exFAT, `disk mount`, /dev) treats it as a disk.
 *
 * Tables are READ when a disk registers (blk_register calls part_scan).  A GPT
 * whose header or entry array fails its CRC is not believed: the backup at the
 * end of the disk is tried, and if that fails too the disk is left whole and
 * the log says why.  An MBR's extended partition is walked for logical
 * partitions (numbered from 5, the long-standing convention).
 *
 * WRITING a table is deliberately small: `part_create_one` lays down ONE
 * partition spanning the disk (1 MiB aligned), as GPT or MBR, refusing while
 * anything on the disk is mounted.  Resizing, several partitions and type
 * choices are not offered — a partition editor is a different tool, and a
 * half one is how disks get destroyed.
 * ============================================================================= */
#ifndef DOS_PARTITION_H
#define DOS_PARTITION_H

#include "block.h"

/* Read the disk's table and register its partitions; returns how many.  A
 * partition device is never scanned itself. */
int  part_scan(struct block_device* disk);

/* 1 if `dev` is a partition (a window onto another device). */
int  part_is(const struct block_device* dev);

/* The disk a partition belongs to, or NULL. */
struct block_device* part_parent(const struct block_device* dev);

/* 1 if the disk itself or any of its partitions is mounted. */
int  part_mounted(struct block_device* disk);

/* Unregister every partition of `disk` (blk_unregister calls this first). */
void part_forget(struct block_device* disk);

/* Write a new table with ONE partition covering the disk: gpt != 0 for GPT,
 * else MBR.  Refused (-2) while the disk or one of its partitions is mounted.
 * 0 on success; the new partition is registered before this returns. */
int  part_create_one(struct block_device* disk, int gpt);

#endif
