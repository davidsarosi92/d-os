/* =============================================================================
 * storage.h — disk management (§M87).  Implementation and rationale in
 * kernel/core/storage.c.
 * ============================================================================= */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>

struct block_device;

struct storage_info {
    char        name[8];        /* "vda", "sda", "ram0"                        */
    uint64_t    bytes;          /* device size                                 */
    const char* fs;             /* "exfat", "empty", "unknown", "fat32", ...    */
    int         mounted;
    char        mount[64];      /* where, when mounted                         */
    const char* hold;           /* what depends on the mount, or NULL          */
    int         open_files;
    int         have_space;     /* total/free are valid                        */
    uint64_t    total, free;    /* bytes, of the mounted filesystem            */
    int         removable;      /* a RAM disk                                  */
};

/* Fill up to `max` rows, one per block device.  Returns how many. */
int  storage_query(struct storage_info* out, int max);

/* Read the disk's first sectors and name what is on it. */
const char* storage_probe_fs(struct block_device* dev);

/* Operations.  All return STOR_* — negative on refusal, with the reason
 * available as a sentence from storage_error(). */
#define STOR_OK               0
#define STOR_ENODEV          -1
#define STOR_EMOUNTED        -2
#define STOR_ENOTMOUNTED     -3
#define STOR_EBUSY_OPEN      -4
#define STOR_EBUSY_NEST      -5
#define STOR_EHELD           -6
#define STOR_ENOFS           -7
#define STOR_EIO             -8
#define STOR_ENOTREMOVABLE   -9
#define STOR_ENOMEM         -10
#define STOR_ECANNOT        -11

int  storage_mount(const char* dev, const char* path);   /* NULL = /media/<dev> */
int  storage_umount(const char* dev_or_path);
int  storage_format(const char* dev, const char* label);
int  storage_remove(const char* dev);                    /* RAM disks only      */
int  storage_sync_all(void);
const char* storage_error(int rc);

/* "12.3 MiB" */
void storage_fmt_size(uint64_t bytes, char* out, int cap);

#endif
