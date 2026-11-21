/*
 * Filesystem core API
 * -------------------
 * Interface between a filesystem handler (AmigaOS packet handler, or a
 * host test tool) and an on-disk filesystem implementation.  The core is
 * written in portable C and knows nothing about AmigaDOS; the handler
 * deals with packets, locks, path syntax and protection bit mapping.
 *
 * Objects are identified by an fs_node_t, which for OS-9 RBF is the
 * sector number of the object's file descriptor.  A node id stays valid
 * for as long as the object exists.  Directory entries also have a
 * position (byte offset within their directory), which is what
 * fs_readdir() iterates over.
 *
 * Each backend (OS-9/68k RBF in core9/, OS-9000 RBF in core9k/)
 * provides a struct fs_ops.  fs_mount() probes the backends and returns
 * a volume whose ops pointer is used by the inline wrappers below.
 * fs_format() writes the filesystem named by fs_format_opts.fs_type.
 *
 * All functions return FS_OK (0) or one of the FS_E* error codes.
 */

#ifndef _FS_API_H
#define _FS_API_H

#include <stdint.h>
#include <stddef.h>
#include "blockdev.h"

typedef uint32_t fs_node_t;     /* Object id (RBF: file descriptor LSN) */
typedef uint32_t fs_time_t;     /* Local time, seconds since 1978-01-01 */

#define FS_NODE_NONE    0       /* No object (LSN 0 is never a file) */

#define FS_NAME_MAX     255     /* Largest name any backend supports */
#define FS_VOLNAME_MAX  32

enum fs_error {
    FS_OK = 0,
    FS_ENOENT,          /* Object not found */
    FS_EEXIST,          /* Object already exists */
    FS_ENOTDIR,         /* Not a directory */
    FS_EISDIR,          /* Is a directory */
    FS_ENOTEMPTY,       /* Directory not empty */
    FS_ENOSPC,          /* No free space */
    FS_EFBIG,           /* File cannot grow (segment list full) */
    FS_EIO,             /* Device I/O error */
    FS_EROFS,           /* Volume is read-only */
    FS_EBADNAME,        /* Name not valid for this filesystem */
    FS_ENOMEM,          /* Out of memory */
    FS_ECORRUPT,        /* Inconsistent on-disk structure */
    FS_ENOTFS,          /* Not a recognized filesystem */
    FS_EBUSY,           /* Object in use */
    FS_EINVAL,          /* Invalid argument */
    FS_ELOOP,           /* Directory would become its own descendant */
    FS_ESECTSIZE,       /* Filesystem sector size differs from device */
    FS_ERANGE,          /* Position or size out of range */
};

/* Filesystem types (fs_volinfo.fs_type, fs_format_opts.fs_type) */
#define FS_FMT_OS9      0       /* OS-9/68k RBF (big-endian) */
#define FS_FMT_OS9K_BE  1       /* OS-9000 RBF, big-endian */
#define FS_FMT_OS9K_LE  2       /* OS-9000 RBF, little-endian */

/* Object types */
#define FS_TYPE_FILE    1
#define FS_TYPE_DIR     2

/*
 * Permission and type attributes.  These are the OS-9/68k attribute
 * bits; the OS-9000 backend maps its permission word onto them (owner
 * bits to owner, world bits to public).
 */
#define FS_ATTR_READ     0x01   /* Owner may read */
#define FS_ATTR_WRITE    0x02   /* Owner may write */
#define FS_ATTR_EXEC     0x04   /* Owner may execute (search a directory) */
#define FS_ATTR_PREAD    0x08   /* Public (others) may read */
#define FS_ATTR_PWRITE   0x10   /* Public may write */
#define FS_ATTR_PEXEC    0x20   /* Public may execute */
#define FS_ATTR_NONSHARE 0x40   /* Single user (non-sharable) */
#define FS_ATTR_DIR      0x80   /* Object is a directory */
#define FS_ATTR_PERMS    0x7f   /* Bits a caller may change */

/* Open modes */
#define FS_OPEN_READ    0x01
#define FS_OPEN_WRITE   0x02

typedef struct fs_stat {
    fs_node_t node;             /* Object id */
    fs_node_t dir;              /* Directory holding the entry (0=unknown) */
    uint32_t  dirpos;           /* Byte offset of the entry in that dir */
    uint32_t  size;             /* Size in bytes */
    uint32_t  blocks;           /* Sectors in use, including the FD */
    fs_time_t mtime;            /* Last modified */
    fs_time_t ctime;            /* Created (date only on OS-9/68k) */
    uint16_t  owner_group;
    uint16_t  owner_user;
    uint16_t  attr;             /* FS_ATTR_* */
    uint8_t   type;             /* FS_TYPE_* */
    uint16_t  links;            /* Link count */
    char      name[FS_NAME_MAX + 1];
} fs_stat_t;

typedef struct fs_volinfo {
    char      name[FS_VOLNAME_MAX + 1];
    uint32_t  block_size;       /* Bytes per sector */
    uint32_t  total_blocks;     /* Sectors in the filesystem */
    uint32_t  free_blocks;      /* Sectors free */
    uint32_t  disk_id;          /* Media identifier set at format */
    fs_time_t created;          /* Volume creation date */
    uint8_t   read_only;
    uint8_t   name_max;         /* Longest file name accepted */
    uint8_t   fs_type;          /* FS_FMT_* */
} fs_volinfo_t;

typedef struct fs_format_opts {
    const char *name;           /* Volume name */
    uint32_t    cluster;        /* Sectors per bitmap bit, 0 = automatic */
    uint32_t    sectors_per_track;  /* Recorded in LSN 0 (informational) */
    uint32_t    heads;              /* Recorded in LSN 0 format byte */
    uint32_t    cylinders;          /* Recorded in LSN 0 format byte */
    uint32_t    disk_id;        /* 0 = derive from the current time */
    uint8_t     fs_type;        /* FS_FMT_*: which filesystem to write */
} fs_format_opts_t;

typedef struct fs_volume fs_volume_t;
typedef struct fs_file   fs_file_t;

struct fs_ops {
    const char *name;
    int  (*unmount)(fs_volume_t *vol, int discard);
    int  (*sync)(fs_volume_t *vol);
    int  (*volinfo)(fs_volume_t *vol, fs_volinfo_t *info);
    int  (*relabel)(fs_volume_t *vol, const char *name);
    int  (*set_disk_id)(fs_volume_t *vol, uint32_t disk_id);
    fs_node_t (*root)(fs_volume_t *vol);
    int  (*valid_name)(fs_volume_t *vol, const char *name);
    int  (*lookup)(fs_volume_t *vol, fs_node_t dir, const char *name,
                   fs_stat_t *st);
    int  (*stat)(fs_volume_t *vol, fs_node_t dir, fs_node_t node,
                 fs_stat_t *st);
    int  (*parent)(fs_volume_t *vol, fs_node_t dir, fs_node_t *parent);
    int  (*readdir)(fs_volume_t *vol, fs_node_t dir, uint32_t *cookie,
                    fs_stat_t *st);
    int  (*create)(fs_volume_t *vol, fs_node_t dir, const char *name,
                   uint16_t attr, int is_dir, fs_stat_t *st);
    int  (*remove)(fs_volume_t *vol, fs_node_t dir, const char *name);
    int  (*rename)(fs_volume_t *vol, fs_node_t sdir, const char *sname,
                   fs_node_t ddir, const char *dname);
    int  (*setattr)(fs_volume_t *vol, fs_node_t node, uint16_t attr);
    int  (*setowner)(fs_volume_t *vol, fs_node_t node, uint16_t group,
                     uint16_t user);
    int  (*setmtime)(fs_volume_t *vol, fs_node_t node, fs_time_t mtime);
    int  (*open)(fs_volume_t *vol, fs_node_t node, int mode, fs_file_t **fp);
    int  (*close)(fs_file_t *fp);
    int  (*read)(fs_file_t *fp, void *buf, uint32_t len, uint32_t *done);
    int  (*write)(fs_file_t *fp, const void *buf, uint32_t len,
                  uint32_t *done);
    int  (*truncate)(fs_file_t *fp, uint32_t size);
    uint32_t (*file_size)(fs_file_t *fp);
    int  (*set_cache)(fs_volume_t *vol, unsigned buffers);
};

/* Common head of every backend's volume structure */
struct fs_volume {
    const struct fs_ops *ops;
    blockdev_t          *dev;
};

/* Common head of every backend's open file structure */
struct fs_file {
    fs_volume_t *vol;
    fs_node_t    node;
    uint32_t     pos;           /* Current position; callers may set it */
    uint8_t      mode;          /* FS_OPEN_* */
};

/*
 * fs_mount() tries each backend on the device.  cache_buffers is the
 * number of sector buffers in the volume's cache.  Returns FS_ENOTFS if
 * no backend recognizes the media.
 */
int fs_mount(blockdev_t *dev, unsigned cache_buffers, fs_volume_t **volp);

/* Writes an empty filesystem of type opts->fs_type over the whole device */
int fs_format(blockdev_t *dev, const fs_format_opts_t *opts);

const char *fs_strerror(int err);

/* Calendar conversions for fs_time_t */
typedef struct fs_tm {
    uint16_t year;              /* 1978.. */
    uint8_t  month;             /* 1..12 */
    uint8_t  day;               /* 1..31 */
    uint8_t  hour;
    uint8_t  min;
    uint8_t  sec;
} fs_tm_t;

fs_time_t fs_mktime(const fs_tm_t *tm);
void      fs_gmtime(fs_time_t t, fs_tm_t *tm);

/* Inline wrappers which dispatch through the volume's ops */
static inline int fs_unmount(fs_volume_t *v, int discard)
    { return (v->ops->unmount(v, discard)); }
static inline int fs_sync(fs_volume_t *v)
    { return (v->ops->sync(v)); }
static inline int fs_volinfo(fs_volume_t *v, fs_volinfo_t *i)
    { return (v->ops->volinfo(v, i)); }
static inline int fs_relabel(fs_volume_t *v, const char *n)
    { return (v->ops->relabel(v, n)); }
static inline int fs_set_disk_id(fs_volume_t *v, uint32_t id)
    { return (v->ops->set_disk_id(v, id)); }
static inline fs_node_t fs_root(fs_volume_t *v)
    { return (v->ops->root(v)); }
static inline int fs_valid_name(fs_volume_t *v, const char *n)
    { return (v->ops->valid_name(v, n)); }
static inline int fs_lookup(fs_volume_t *v, fs_node_t d, const char *n,
                            fs_stat_t *st)
    { return (v->ops->lookup(v, d, n, st)); }
static inline int fs_stat(fs_volume_t *v, fs_node_t d, fs_node_t n,
                          fs_stat_t *st)
    { return (v->ops->stat(v, d, n, st)); }
static inline int fs_parent(fs_volume_t *v, fs_node_t d, fs_node_t *p)
    { return (v->ops->parent(v, d, p)); }
static inline int fs_readdir(fs_volume_t *v, fs_node_t d, uint32_t *c,
                             fs_stat_t *st)
    { return (v->ops->readdir(v, d, c, st)); }
static inline int fs_create(fs_volume_t *v, fs_node_t d, const char *n,
                            uint16_t attr, fs_stat_t *st)
    { return (v->ops->create(v, d, n, attr, 0, st)); }
static inline int fs_mkdir(fs_volume_t *v, fs_node_t d, const char *n,
                           uint16_t attr, fs_stat_t *st)
    { return (v->ops->create(v, d, n, attr, 1, st)); }
static inline int fs_remove(fs_volume_t *v, fs_node_t d, const char *n)
    { return (v->ops->remove(v, d, n)); }
static inline int fs_rename(fs_volume_t *v, fs_node_t sd, const char *sn,
                            fs_node_t dd, const char *dn)
    { return (v->ops->rename(v, sd, sn, dd, dn)); }
static inline int fs_setattr(fs_volume_t *v, fs_node_t n, uint16_t a)
    { return (v->ops->setattr(v, n, a)); }
static inline int fs_setowner(fs_volume_t *v, fs_node_t n, uint16_t g,
                              uint16_t u)
    { return (v->ops->setowner(v, n, g, u)); }
static inline int fs_setmtime(fs_volume_t *v, fs_node_t n, fs_time_t t)
    { return (v->ops->setmtime(v, n, t)); }
static inline int fs_open(fs_volume_t *v, fs_node_t n, int mode,
                          fs_file_t **fp)
    { return (v->ops->open(v, n, mode, fp)); }
static inline int fs_close(fs_file_t *f)
    { return (f->vol->ops->close(f)); }
static inline int fs_read(fs_file_t *f, void *b, uint32_t l, uint32_t *d)
    { return (f->vol->ops->read(f, b, l, d)); }
static inline int fs_write(fs_file_t *f, const void *b, uint32_t l,
                           uint32_t *d)
    { return (f->vol->ops->write(f, b, l, d)); }
static inline int fs_truncate(fs_file_t *f, uint32_t size)
    { return (f->vol->ops->truncate(f, size)); }
static inline uint32_t fs_file_size(fs_file_t *f)
    { return (f->vol->ops->file_size(f)); }
static inline int fs_set_cache(fs_volume_t *v, unsigned buffers)
    { return (v->ops->set_cache(v, buffers)); }

#endif /* _FS_API_H */
