/*
 * OS-9/68k and OS-9/6809 RBF (Random Block File manager) filesystem
 * -----------------------------------------------------------------
 * On-disk layout (all multi-byte values are big-endian, and most are
 * byte aligned for historical 6809 reasons):
 *
 *   LSN 0          Identification sector (struct sector0 in rbf.h)
 *   LSN 1..        Allocation bitmap, DD_MAP bytes.  Bit 7 of byte 0 is
 *                  cluster 0; a set bit means the cluster is in use.  A
 *                  cluster is DD_BIT sectors.
 *   elsewhere      File descriptors (one sector each) and file data.
 *
 * A file descriptor (FD) holds the attributes, owner, dates, link count
 * and size of a file, followed by a list of segments (3-byte start LSN,
 * 2-byte length in sectors) which fills the rest of the sector.  A
 * directory is a file of 32-byte entries: 28 bytes of name (the last
 * character has bit 7 set; unused bytes are zero; a zero first byte
 * marks a free entry) and a 4-byte FD LSN.  The first two entries of
 * every directory are ".." and ".".
 *
 * OS-9/6809 (and NitrOS-9) disks have the same layout with these
 * differences: sectors are always 256 bytes, LSN 0 ends at DD_RES2 (no
 * sync code, sector size or bitmap LSN fields), and a directory entry
 * has 29 bytes of name followed by a 3-byte FD LSN.
 */

#ifndef _RBF68K_H
#define _RBF68K_H

#include <string.h>
#include "fs_api.h"
#include "cache.h"
#include "blockmap.h"

/* Identification sector (LSN 0) */
#define DD_TOT          0x00    /* 3: total sectors */
#define DD_TKS          0x03    /* 1: track size in sectors */
#define DD_MAP          0x04    /* 2: bytes in allocation bitmap */
#define DD_BIT          0x06    /* 2: sectors per cluster */
#define DD_DIR          0x08    /* 3: root directory FD LSN */
#define DD_OWN          0x0b    /* 2: owner */
#define DD_ATT          0x0d    /* 1: attributes */
#define DD_DSK          0x0e    /* 2: disk id */
#define DD_FMT          0x10    /* 1: format (density/sides) */
#define DD_SPT          0x11    /* 2: sectors per track */
#define DD_RES          0x13    /* 2: reserved */
#define DD_BT           0x15    /* 3: bootstrap LSN */
#define DD_BSZ          0x18    /* 2: bootstrap size */
#define DD_DATE         0x1a    /* 5: creation date YY MM DD HH MM */
#define DD_NAME         0x1f    /* 32: volume name */
#define DD_OPT          0x3f    /* 32: path descriptor options */
#define DD_RES2         0x5f    /* 1: reserved */
#define DD_SYNC         0x60    /* 4: media integrity code */
#define DD_MAPLSN       0x64    /* 4: bitmap start LSN (0 = LSN 1) */
#define DD_LSNSIZE      0x68    /* 2: logical sector size (0 = 256) */
#define DD_VERSID       0x6a    /* 2: sector 0 version */

#define DD_NAME_LEN     32
#define DD_SYNC_CODE    0x4372757aUL    /* "Cruz" */

#define FMT_DS          0x01    /* Double sided */
#define FMT_DD          0x02    /* Double density */
#define FMT_DT          0x04    /* Double track density */

/* File descriptor */
#define FD_ATT          0x00    /* 1: attributes */
#define FD_OWN          0x01    /* 2: owner group, user */
#define FD_DAT          0x03    /* 5: modified YY MM DD HH MM */
#define FD_LNK          0x08    /* 1: link count */
#define FD_SIZ          0x09    /* 4: file size */
#define FD_DCR          0x0d    /* 3: created YY MM DD */
#define FD_SEG          0x10    /* segment list */
#define FD_SEG_SIZE     5       /* 3-byte LSN, 2-byte sector count */
#define FD_SEG_MAX_LEN  0xffff  /* Largest segment, in sectors */

/* Directory entries */
#define DIR_ENT_SIZE    32
#define DIR_NAME_LEN    28
#define DIR_FD          28      /* 4: FD LSN */
#define DIR_NAME_LEN_6809 29
#define DIR_FD_6809     29      /* 3: FD LSN */

#define RBF_MAX_LSN     0xffffff        /* LSNs are 24 bits */
#define RBF_MAX_MAP     0xffff          /* DD_MAP is 16 bits */

#define FREE_UNKNOWN    0xffffffffU     /* rbf_vol.free_sectors not counted */

/* Default segment allocation size (sectors), as PD_SAS in a descriptor */
#define RBF_DEFAULT_SAS 8

/* Big-endian field access */
static inline uint32_t get8(const uint8_t *p)  { return (p[0]); }
static inline uint32_t get16(const uint8_t *p) { return ((p[0] << 8) | p[1]); }
static inline uint32_t get24(const uint8_t *p)
    { return (((uint32_t) p[0] << 16) | (p[1] << 8) | p[2]); }
static inline uint32_t get32(const uint8_t *p)
    { return (((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
              (p[2] << 8) | p[3]); }
static inline void put16(uint8_t *p, uint32_t v)
    { p[0] = v >> 8; p[1] = v; }
static inline void put24(uint8_t *p, uint32_t v)
    { p[0] = v >> 16; p[1] = v >> 8; p[2] = v; }
static inline void put32(uint8_t *p, uint32_t v)
    { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/*
 * In-memory copy of a file descriptor.  Every user of an FD (open files,
 * directory operations) shares one node per FD sector, so sizes and
 * segment lists are always consistent.  A node is written back to the
 * cache by node_put() or rbf_sync() when dirty.
 */
typedef struct rbf_node rbf_node_t;
struct rbf_node {
    rbf_node_t *next;
    uint32_t    lsn;            /* FD sector */
    unsigned    refs;
    uint8_t     dirty;
    uint8_t     deleted;        /* Space released; never write back */
    uint8_t    *fd;             /* sector_size bytes */
};

typedef struct rbf_vol {
    fs_volume_t base;
    cache_t     cache;
    uint32_t    sector_size;
    uint8_t     sector_shift;
    uint32_t    total;          /* DD_TOT */
    uint32_t    cluster;        /* Sectors per cluster (DD_BIT) */
    uint8_t     cluster_shift;
    uint32_t    clusters;       /* Clusters covered by the bitmap */
    uint32_t    map_bytes;      /* DD_MAP */
    uint32_t    map_lsn;        /* First bitmap sector */
    uint32_t    map_sectors;
    uint32_t    root;           /* Root directory FD LSN */
    uint32_t    sas;            /* Minimum allocation, sectors */
    uint32_t    rove;           /* Next cluster to try allocating */
    uint32_t    free_sectors;   /* Cached free count, ~0 if unknown */
    unsigned    max_segs;       /* Segment slots in an FD */
    uint8_t     read_only;
    uint8_t     fs_type;        /* FS_FMT_OS9 or FS_FMT_OS9_6809 */
    uint8_t     name_len;       /* Name bytes in a directory entry */
    rbf_node_t *nodes;
    uint8_t    *zero;           /* A sector of zeros */
    blockmap_t  map;            /* Sector size differs from the device */
    uint8_t     mapped;
} rbf_vol_t;

typedef struct rbf_file {
    fs_file_t   base;
    rbf_node_t *node;
    uint8_t     written;        /* Data changed since open */
} rbf_file_t;

#define RVOL(v) ((rbf_vol_t *) (v))

/* FD LSN of a directory entry */
static inline uint32_t dir_ent_fd(const rbf_vol_t *vol, const uint8_t *ent)
{
    return ((vol->fs_type == FS_FMT_OS9_6809) ? get24(ent + DIR_FD_6809) :
            get32(ent + DIR_FD));
}

/* Sets the FD LSN of an entry, clearing the bytes after the name */
static inline void dir_ent_set_fd(const rbf_vol_t *vol, uint8_t *ent,
                                  uint32_t lsn)
{
    memset(ent + vol->name_len, 0, DIR_ENT_SIZE - vol->name_len);
    if (vol->fs_type == FS_FMT_OS9_6809)
        put24(ent + DIR_FD_6809, lsn);
    else
        put32(ent + DIR_FD, lsn);
}

/* rbf68k_vol.c */
int  rbf68k_mount(blockdev_t *dev, unsigned cache_buffers, fs_volume_t **vp);
int  rbf68k_format(blockdev_t *dev, const fs_format_opts_t *opts);
int  rbf_sync(fs_volume_t *v);
int  rbf_alloc(rbf_vol_t *vol, uint32_t want, uint32_t hint,
               uint32_t *lsnp, uint32_t *countp);
int  rbf_free(rbf_vol_t *vol, uint32_t lsn, uint32_t count);
int  rbf_valid_volname(const char *name);
fs_time_t rbf_date_to_time(const uint8_t *date, int len);
void rbf_time_to_date(fs_time_t t, uint8_t *date, int len);
extern const struct fs_ops rbf68k_ops;

/* rbf68k_node.c */
int  node_get(rbf_vol_t *vol, uint32_t lsn, rbf_node_t **np);
int  node_put(rbf_vol_t *vol, rbf_node_t *n);
int  node_write(rbf_vol_t *vol, rbf_node_t *n);
uint32_t node_size(const rbf_node_t *n);
void node_set_size(rbf_node_t *n, uint32_t size);
uint32_t node_alloc_sectors(rbf_vol_t *vol, const rbf_node_t *n);
int  node_map(rbf_vol_t *vol, rbf_node_t *n, uint32_t index,
              uint32_t *lsnp, uint32_t *runp);
int  node_grow(rbf_vol_t *vol, rbf_node_t *n, uint32_t sectors,
               int zero_new);
int  node_trim(rbf_vol_t *vol, rbf_node_t *n, uint32_t sectors);
int  node_release(rbf_vol_t *vol, rbf_node_t *n);
int  node_check(rbf_vol_t *vol, const uint8_t *fd);

/* rbf68k_file.c */
int  rbf_open(fs_volume_t *v, fs_node_t node, int mode, fs_file_t **fp);
int  rbf_close(fs_file_t *fp);
int  rbf_read(fs_file_t *fp, void *buf, uint32_t len, uint32_t *done);
int  rbf_write(fs_file_t *fp, const void *buf, uint32_t len,
               uint32_t *done);
int  rbf_truncate(fs_file_t *fp, uint32_t size);
uint32_t rbf_file_size(fs_file_t *fp);
int  node_read_bytes(rbf_vol_t *vol, rbf_node_t *n, uint32_t pos,
                     void *buf, uint32_t len);
int  node_write_bytes(rbf_vol_t *vol, rbf_node_t *n, uint32_t pos,
                      const void *buf, uint32_t len, uint32_t valid_end);

/* rbf68k_dir.c */
int  rbf_valid_name(fs_volume_t *v, const char *name);
int  rbf_lookup(fs_volume_t *v, fs_node_t dir, const char *name,
                fs_stat_t *st);
int  rbf_stat(fs_volume_t *v, fs_node_t dir, fs_node_t node, fs_stat_t *st);
int  rbf_parent(fs_volume_t *v, fs_node_t dir, fs_node_t *parent);
int  rbf_readdir(fs_volume_t *v, fs_node_t dir, uint32_t *cookie,
                 fs_stat_t *st);
int  rbf_create(fs_volume_t *v, fs_node_t dir, const char *name,
                uint16_t attr, int is_dir, fs_stat_t *st);
int  rbf_remove(fs_volume_t *v, fs_node_t dir, const char *name);
int  rbf_rename(fs_volume_t *v, fs_node_t sdir, const char *sname,
                fs_node_t ddir, const char *dname);
int  rbf_setattr(fs_volume_t *v, fs_node_t node, uint16_t attr);
int  rbf_setowner(fs_volume_t *v, fs_node_t node, uint16_t group,
                  uint16_t user);
int  rbf_setmtime(fs_volume_t *v, fs_node_t node, fs_time_t mtime);
void rbf_decode_name(const uint8_t *src, int maxlen, char *dst);
int  rbf_encode_name(const char *src, uint8_t *dst, int maxlen);

#endif /* _RBF68K_H */
