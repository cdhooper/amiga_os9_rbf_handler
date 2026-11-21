/*
 * Host platform for the filesystem core: an image file as a block
 * device, and the fsp_* services.
 */

#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "hostdev.h"
#include "fs_api.h"

static int
host_read(blockdev_t *dev, uint32_t lsn, uint32_t count, void *buf)
{
    hostdev_t *hd = (hostdev_t *) dev;
    off_t      off = hd->offset + (off_t) lsn * dev->sector_size;
    size_t     len = (size_t) count * dev->sector_size;

    if ((uint64_t) lsn + count > dev->sector_count)
        return (-1);
    hd->reads++;
    return ((pread(hd->fd, buf, len, off) == (ssize_t) len) ? 0 : -1);
}

static int
host_write(blockdev_t *dev, uint32_t lsn, uint32_t count, const void *buf)
{
    hostdev_t *hd = (hostdev_t *) dev;
    off_t      off = hd->offset + (off_t) lsn * dev->sector_size;
    size_t     len = (size_t) count * dev->sector_size;

    if (dev->read_only || ((uint64_t) lsn + count > dev->sector_count))
        return (-1);
    hd->writes++;
    return ((pwrite(hd->fd, buf, len, off) == (ssize_t) len) ? 0 : -1);
}

static int
host_sync(blockdev_t *dev)
{
    (void) dev;
    return (0);
}

int
hostdev_open(hostdev_t *hd, const char *path, uint32_t sector_size,
             uint64_t offset, uint32_t sectors, int read_only)
{
    struct stat st;

    memset(hd, 0, sizeof (*hd));
    hd->fd = open(path, read_only ? O_RDONLY : O_RDWR);
    if (hd->fd < 0)
        return (-1);
    if (fstat(hd->fd, &st) != 0) {
        close(hd->fd);
        return (-1);
    }
    if (sectors == 0) {
        if ((uint64_t) st.st_size <= offset) {
            close(hd->fd);
            return (-1);
        }
        sectors = (st.st_size - offset) / sector_size;
    }
    hd->offset = offset;
    hd->dev.sector_size  = sector_size;
    hd->dev.sector_count = sectors;
    hd->dev.read_only    = read_only;
    hd->dev.read  = host_read;
    hd->dev.write = host_write;
    hd->dev.sync  = host_sync;
    return (0);
}

void
hostdev_close(hostdev_t *hd)
{
    if (hd->fd >= 0)
        close(hd->fd);
    hd->fd = -1;
}

/* ------------------------------------------------------------------ */
/* Platform services                                                  */
/* ------------------------------------------------------------------ */

int        hostdev_verbose = 0;
fs_time_t  hostdev_fixed_time = 0;   /* Nonzero: use as "now" */

void *
fsp_alloc(uint32_t size)
{
    return (calloc(1, size));
}

void
fsp_free(void *ptr, uint32_t size)
{
    (void) size;
    free(ptr);
}

uint32_t
fsp_now(void)
{
    time_t     now;
    struct tm  lt;
    fs_tm_t    tm;

    if (hostdev_fixed_time != 0)
        return (hostdev_fixed_time);
    now = time(NULL);
    localtime_r(&now, &lt);
    tm.year  = lt.tm_year + 1900;
    tm.month = lt.tm_mon + 1;
    tm.day   = lt.tm_mday;
    tm.hour  = lt.tm_hour;
    tm.min   = lt.tm_min;
    tm.sec   = lt.tm_sec;
    return (fs_mktime(&tm));
}

void
fsp_log(const char *fmt, ...)
{
    va_list ap;

    if (!hostdev_verbose)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
