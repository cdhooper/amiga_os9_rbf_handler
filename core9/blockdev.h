/*
 * Block device interface used by the filesystem core
 * --------------------------------------------------
 * A block device is an array of equal-sized sectors numbered from 0.
 * On AmigaOS it is a partition of a trackdisk-style exec device; on the
 * host it is a region of an image file.
 */

#ifndef _BLOCKDEV_H
#define _BLOCKDEV_H

#include <stdint.h>

typedef struct blockdev blockdev_t;

struct blockdev {
    uint32_t sector_size;       /* Bytes per sector (256..4096) */
    uint32_t sector_count;      /* Sectors on the device or partition */
    uint8_t  read_only;         /* Media is write-protected */

    /* Return 0 on success, nonzero on I/O error */
    int (*read)(blockdev_t *dev, uint32_t lsn, uint32_t count, void *buf);
    int (*write)(blockdev_t *dev, uint32_t lsn, uint32_t count,
                 const void *buf);
    int (*sync)(blockdev_t *dev);   /* Flush device-level write buffers */
};

/*
 * Platform services the core needs.  Each platform (Amiga handler,
 * host tools) provides these at link time.
 */
void     *fsp_alloc(uint32_t size);            /* Zeroed memory */
void      fsp_free(void *ptr, uint32_t size);
uint32_t  fsp_now(void);                       /* fs_time_t "now" */
void      fsp_log(const char *fmt, ...)
              __attribute__((format(__printf__, 1, 2)));

#endif /* _BLOCKDEV_H */
