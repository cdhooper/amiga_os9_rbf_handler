/*
 * Block size mapping
 * ------------------
 * Presents a block device as a device with a different (power of two)
 * sector size, so that a filesystem whose logical sectors differ from
 * the device's blocks can be used, e.g. an OS-9 disk with 256-byte
 * sectors on a 512-byte device.  Logical sectors smaller than a device
 * block are written by read-modify-write of the block.
 *
 * A sparse map instead puts each logical sector at the start of its own
 * device block and ignores the rest of the block (zeroed when written).
 * CoCo OS-9/6809 hard disk drivers store 256-byte sectors this way.
 *
 * An offset map starts logical sector 0 a number of bytes into the
 * device, e.g. an x86 OS-9000 disk whose ID block follows a 512-byte
 * PC boot sector.
 */

#ifndef _BLOCKMAP_H
#define _BLOCKMAP_H

#include "blockdev.h"

typedef struct blockmap {
    blockdev_t  bd;             /* Must be first: the mapped device */
    blockdev_t *dev;            /* Underlying device */
    uint8_t     shift;          /* log2(bd.sector_size) */
    uint8_t     dev_shift;      /* log2(dev->sector_size) */
    uint8_t     sparse;         /* One logical sector per device block */
    uint64_t    base;           /* Byte offset of logical sector 0 */
    uint8_t    *buf;            /* One device block (logical < device) */
} blockmap_t;

/* Returns 0, or nonzero if the sizes are unusable or memory is short */
int  blockmap_init(blockmap_t *bm, blockdev_t *dev, uint32_t sector_size);
/* Sparse map: sector_size must be smaller than the device's */
int  blockmap_init_sparse(blockmap_t *bm, blockdev_t *dev,
                          uint32_t sector_size);
/*
 * Offset map: logical sector 0 is base bytes into the device, and the
 * map covers length bytes from there (0: to the end of the device)
 */
int  blockmap_init_offset(blockmap_t *bm, blockdev_t *dev,
                          uint32_t sector_size, uint64_t base,
                          uint64_t length);
void blockmap_destroy(blockmap_t *bm);

/*
 * Reads len bytes at byte offset off of the device, whatever its block
 * size.  tmp must hold one device block.  Returns 0 on success.
 */
int  blockdev_read_bytes(blockdev_t *dev, uint64_t off, uint32_t len,
                         void *buf, uint8_t *tmp);

#endif /* _BLOCKMAP_H */
