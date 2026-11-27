/*
 * OS-9000 RBF filesystem
 * ----------------------
 * On-disk layout.  Multi-byte numbers are stored in the byte order given
 * by the volume's ID block ('B' big-endian, 'L' little-endian); names,
 * file data and the allocation bitmap are byte streams and are never
 * swapped.  The sync codes are byte palindromes, so they read the same
 * in either order.
 *
 *   block 0        ID block: geometry, total blocks, block size, the FD
 *                  blocks of the bitmap, boot files and root directory,
 *                  dates, volume name, byte order.
 *   elsewhere      The allocation bitmap, kept as a file (one contiguous
 *                  segment): bit 7 of byte 0 is block 0, a set bit means
 *                  the block is in use.  File descriptors and file data.
 *
 * A file descriptor (FD) block starts with a 48-byte header (sync,
 * parity, flags and permissions, owners, link count, size, five
 * timestamps, revision).  The rest of the block holds 8-byte segments
 * (start block, block count), except that the last 8 bytes link the FD
 * blocks of a file into a ring (previous, next); a file with more
 * segments than fit in one block continues in further FD blocks.  A
 * "small file" instead keeps its data in the FD block after the header.
 *
 * The parity word makes the XOR of all 32-bit words of an FD block equal
 * to 0xffffffff.  Being a plain XOR it does not depend on byte order.
 *
 * Directories are files of 64-byte entries: a NUL-terminated name of up
 * to 43 characters, and the FD block address at offset 60.  A deleted
 * entry has a zero first byte.  Every directory starts with ".." and
 * ".".  Link counts are Unix style: a directory has one link for its
 * entry, one for its "." and one for each subdirectory's "..".
 */

#ifndef _RBF9K_H
#define _RBF9K_H

#include "fs_api.h"
#include "cache.h"
#include "blockmap.h"

/* ID block (block 0) */
#define ID_SYNC         0       /* 4: R9K_IDB_SYNC */
#define ID_DISKID       4       /* 4: disk id */
#define ID_TOTBLKS      8       /* 4: blocks on the media */
#define ID_CYLS         12      /* 2: cylinders */
#define ID_CYL0SIZE     14      /* 2: blocks in cylinder 0 */
#define ID_CYLSIZE      16      /* 2: blocks per track */
#define ID_HEADS        18      /* 2: surfaces */
#define ID_BLKSIZE      20      /* 2: block size in bytes */
#define ID_FORMAT       22      /* 2: format word */
#define ID_FLAGS        24      /* 2: flags */
#define ID_BITMAP       28      /* 4: FD block of the bitmap file */
#define ID_FIRSTBOOT    32      /* 4: FD block of the low level booter */
#define ID_BOOTFILE     36      /* 4: FD block of the bootfile */
#define ID_ROOTDIR      40      /* 4: FD block of the root directory */
#define ID_GROUP        44      /* 2: owner group */
#define ID_OWNER        46      /* 2: owner user */
#define ID_CTIME        48      /* 4: creation time */
#define ID_MTIME        52      /* 4: time of last ID block update */
#define ID_NAME         56      /* 32: volume name, NUL terminated */
#define ID_ENDFLAG      88      /* 1: byte order, 'B' or 'L' */
#define ID_PARITY       92      /* 4: unused (zero) */
#define ID_SIZE         96

#define ID_NAME_LEN     32
#define R9K_IDB_SYNC    0xadb0b0adUL
#define R9K_PC_ID_OFFSET 512    /* x86: ID block after the boot sector */
#define R9K_BIG_END     'B'
#define R9K_LITTLE_END  'L'
#define IDF_EPOCH70     0x0001  /* Times count from 1970 (else 1980) */

#define IDFMT_DBLTRK0   0x0001
#define IDFMT_DBLBIT    0x0002
#define IDFMT_DBLTRK    0x0004
#define IDFMT_DBLSIDE   0x0008
#define IDFMT_HARDDISK  0x8000

/* File descriptor block */
#define FD_SYNC         0       /* 4: R9K_FD_SYNC */
#define FD_PARITY       4       /* 4: parity word */
#define FD_FLAG         8       /* 4: type flags and permissions */
#define FD_HOST         12      /* 2: host owner */
#define FD_GROUP        14      /* 2: owner group */
#define FD_OWNER        16      /* 2: owner user */
#define FD_LINKS        18      /* 2: link count */
#define FD_SIZE         20      /* 4: file size in bytes */
#define FD_CTIME        24      /* 4: created */
#define FD_ATIME        28      /* 4: last accessed */
#define FD_MTIME        32      /* 4: last modified */
#define FD_UTIME        36      /* 4: last changed (FD) */
#define FD_BTIME        40      /* 4: last backed up */
#define FD_REV          44      /* 2: revision of the writer */
#define FD_HDR          48      /* Segments or small file data follow */
#define FD_SEG_SIZE     8       /* 4: start block, 4: block count */
#define FD_LINK_SIZE    8       /* Last 8 bytes: previous, next FD block */

#define R9K_FD_SYNC     0xfdb0b0fdUL
#define R9K_FD_REV      1

/* FD_FLAG bits */
#define FDF_OWNER_R     0x0001
#define FDF_OWNER_W     0x0002
#define FDF_OWNER_X     0x0004
#define FDF_GROUP_R     0x0010
#define FDF_GROUP_W     0x0020
#define FDF_GROUP_X     0x0040
#define FDF_WORLD_R     0x0100
#define FDF_WORLD_W     0x0200
#define FDF_WORLD_X     0x0400
#define FDF_PERMS       0x0777
#define FDF_SYMLINK     0x2000
#define FDF_EXCL        0x4000  /* Non-sharable */
#define FDF_DIR         0x8000
#define FDF_SMALL       0x10000 /* Data is in the FD block */

/* Directory entries */
#define DIR_ENT_SIZE    64
#define DIR_NAME_LEN    43      /* Characters, plus a NUL */
#define DIR_FD          60      /* 4: FD block */

#define R9K_NAME_MAX    DIR_NAME_LEN
#define R9K_DEFAULT_SAS 8       /* Minimum blocks added to a file */
#define FREE_UNKNOWN    0xffffffffU

/* Seconds from 1970-01-01 and from 1980-01-01 to 1978-01-01 */
#define SECS_1970_1978  252460800UL
#define SECS_1978_1980  63072000UL

typedef struct r9k_seg {
    uint32_t start;
    uint32_t count;
} r9k_seg_t;

/*
 * In-memory file descriptor.  The first FD block is kept as raw disk
 * bytes (the header, and the data of a small file).  The segments of all
 * FD blocks are kept as one list in host order, together with the
 * addresses of the file's further FD blocks; node_write() lays them out
 * into the FD blocks again.  Every user of an FD shares one node.
 */
typedef struct r9k_node r9k_node_t;
struct r9k_node {
    r9k_node_t *next;
    uint32_t    lsn;            /* First FD block */
    unsigned    refs;
    uint8_t     dirty;
    uint8_t     deleted;        /* Space released; never write back */
    uint8_t    *fd;             /* First FD block, block_size bytes */
    r9k_seg_t  *segs;
    unsigned    nsegs;
    unsigned    segcap;
    uint32_t   *ext;            /* Further FD blocks, in ring order */
    unsigned    next_blks;
    unsigned    extcap;
    uint32_t    alloc;          /* Blocks in all segments */
};

typedef struct r9k_vol {
    fs_volume_t base;
    cache_t     cache;
    uint32_t    bsize;          /* Block size */
    uint8_t     bshift;
    uint8_t     le;             /* Little-endian numbers */
    uint8_t     epoch70;        /* Times count from 1970 */
    uint8_t     read_only;
    uint8_t     mapped;         /* I/O goes through map */
    uint32_t    total;          /* Blocks on the media */
    uint32_t    root;           /* Root directory FD */
    uint32_t    map_fd;         /* Bitmap file FD */
    uint32_t    map_lsn;        /* First bitmap block */
    uint32_t    map_blocks;
    uint32_t    sas;            /* Minimum allocation, blocks */
    uint32_t    rove;           /* Next block to try allocating */
    uint32_t    free_blocks;    /* Cached free count, ~0 if unknown */
    unsigned    slots;          /* Segment slots in an FD block */
    r9k_node_t *nodes;
    uint8_t    *zero;           /* A block of zeros */
    blockmap_t  map;
} r9k_vol_t;

typedef struct r9k_file {
    fs_file_t   base;
    r9k_node_t *node;
    uint8_t     written;        /* Data changed since open */
} r9k_file_t;

#define R9VOL(v) ((r9k_vol_t *) (v))

/* Numbers in the volume's byte order (rbf9k_vol.c) */
uint32_t r9_get16(const r9k_vol_t *v, const uint8_t *p);
uint32_t r9_get32(const r9k_vol_t *v, const uint8_t *p);
void     r9_put16(const r9k_vol_t *v, uint8_t *p, uint32_t x);
void     r9_put32(const r9k_vol_t *v, uint8_t *p, uint32_t x);

/* rbf9k_vol.c */
int  rbf9k_mount(blockdev_t *dev, unsigned cache_buffers, fs_volume_t **vp);
int  rbf9k_format(blockdev_t *dev, const fs_format_opts_t *opts);
int  r9k_sync(fs_volume_t *v);
int  r9k_alloc(r9k_vol_t *vol, uint32_t want, uint32_t hint,
               uint32_t *lsnp, uint32_t *countp);
int  r9k_free(r9k_vol_t *vol, uint32_t lsn, uint32_t count);
fs_time_t r9k_to_time(const r9k_vol_t *vol, uint32_t t);
uint32_t  r9k_from_time(const r9k_vol_t *vol, fs_time_t t);
void r9k_set_parity(uint8_t *blk, uint32_t size);
void r9k_decode_name(const uint8_t *src, int maxlen, char *dst);
extern const struct fs_ops rbf9k_ops;

/* rbf9k_node.c */
int  r9k_node_get(r9k_vol_t *vol, uint32_t lsn, r9k_node_t **np);
int  r9k_node_put(r9k_vol_t *vol, r9k_node_t *n);
int  r9k_node_write(r9k_vol_t *vol, r9k_node_t *n);
void r9k_node_free(r9k_vol_t *vol, r9k_node_t *n);
uint32_t r9k_node_flag(const r9k_vol_t *vol, const r9k_node_t *n);
void r9k_node_set_flag(r9k_vol_t *vol, r9k_node_t *n, uint32_t flag);
uint32_t r9k_node_size(const r9k_vol_t *vol, const r9k_node_t *n);
void r9k_node_set_size(r9k_vol_t *vol, r9k_node_t *n, uint32_t size);
void r9k_node_stamp(r9k_vol_t *vol, r9k_node_t *n, int field);
int  r9k_node_is_small(const r9k_vol_t *vol, const r9k_node_t *n);
uint32_t r9k_small_max(const r9k_vol_t *vol);
int  r9k_node_map(r9k_vol_t *vol, r9k_node_t *n, uint32_t index,
                  uint32_t *lsnp, uint32_t *runp);
int  r9k_node_grow(r9k_vol_t *vol, r9k_node_t *n, uint32_t blocks,
                   int zero_new);
int  r9k_node_trim(r9k_vol_t *vol, r9k_node_t *n, uint32_t blocks);
int  r9k_node_release(r9k_vol_t *vol, r9k_node_t *n);
int  r9k_node_unsmall(r9k_vol_t *vol, r9k_node_t *n);

/* rbf9k_file.c */
int  r9k_open(fs_volume_t *v, fs_node_t node, int mode, fs_file_t **fp);
int  r9k_close(fs_file_t *fp);
int  r9k_read(fs_file_t *fp, void *buf, uint32_t len, uint32_t *done);
int  r9k_write(fs_file_t *fp, const void *buf, uint32_t len,
               uint32_t *done);
int  r9k_truncate(fs_file_t *fp, uint32_t size);
uint32_t r9k_file_size(fs_file_t *fp);
int  r9k_read_bytes(r9k_vol_t *vol, r9k_node_t *n, uint32_t pos,
                    void *buf, uint32_t len);
int  r9k_write_bytes(r9k_vol_t *vol, r9k_node_t *n, uint32_t pos,
                     const void *buf, uint32_t len, uint32_t valid_end);
int  r9k_reserve(r9k_vol_t *vol, r9k_node_t *n, uint32_t end);

/* rbf9k_dir.c */
int  r9k_valid_name(fs_volume_t *v, const char *name);
int  r9k_lookup(fs_volume_t *v, fs_node_t dir, const char *name,
                fs_stat_t *st);
int  r9k_stat(fs_volume_t *v, fs_node_t dir, fs_node_t node, fs_stat_t *st);
int  r9k_parent(fs_volume_t *v, fs_node_t dir, fs_node_t *parent);
int  r9k_readdir(fs_volume_t *v, fs_node_t dir, uint32_t *cookie,
                 fs_stat_t *st);
int  r9k_create(fs_volume_t *v, fs_node_t dir, const char *name,
                uint16_t attr, int is_dir, fs_stat_t *st);
int  r9k_remove(fs_volume_t *v, fs_node_t dir, const char *name);
int  r9k_rename(fs_volume_t *v, fs_node_t sdir, const char *sname,
                fs_node_t ddir, const char *dname);
int  r9k_setattr(fs_volume_t *v, fs_node_t node, uint16_t attr);
int  r9k_setowner(fs_volume_t *v, fs_node_t node, uint16_t group,
                  uint16_t user);
int  r9k_setmtime(fs_volume_t *v, fs_node_t node, fs_time_t mtime);

#endif /* _RBF9K_H */
