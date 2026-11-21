/*
 * Filesystem core: backend selection, error strings, time conversion
 */

#include "fs_api.h"

/* Backends (core9/rbf68k_vol.c, core9k/rbf9k_vol.c) */
int rbf68k_mount(blockdev_t *dev, unsigned cache_buffers, fs_volume_t **vp);
int rbf68k_format(blockdev_t *dev, const fs_format_opts_t *opts);
int rbf9k_mount(blockdev_t *dev, unsigned cache_buffers, fs_volume_t **vp);
int rbf9k_format(blockdev_t *dev, const fs_format_opts_t *opts);

/*
 * OS-9000 is probed first: its ID block has a sync code, while OS-9/68k
 * LSN 0 has none and is recognized only by its layout being plausible.
 */
int
fs_mount(blockdev_t *dev, unsigned cache_buffers, fs_volume_t **volp)
{
    int rc;

    *volp = NULL;
    rc = rbf9k_mount(dev, cache_buffers, volp);
    if (rc == FS_ENOTFS)
        rc = rbf68k_mount(dev, cache_buffers, volp);
    return (rc);
}

int
fs_format(blockdev_t *dev, const fs_format_opts_t *opts)
{
    switch (opts->fs_type) {
        case FS_FMT_OS9:
            return (rbf68k_format(dev, opts));
        case FS_FMT_OS9K_BE:
        case FS_FMT_OS9K_LE:
            return (rbf9k_format(dev, opts));
        default:
            return (FS_EINVAL);
    }
}

const char *
fs_strerror(int err)
{
    switch (err) {
        case FS_OK:         return ("no error");
        case FS_ENOENT:     return ("object not found");
        case FS_EEXIST:     return ("object exists");
        case FS_ENOTDIR:    return ("not a directory");
        case FS_EISDIR:     return ("is a directory");
        case FS_ENOTEMPTY:  return ("directory not empty");
        case FS_ENOSPC:     return ("disk full");
        case FS_EFBIG:      return ("segment list full");
        case FS_EIO:        return ("I/O error");
        case FS_EROFS:      return ("read-only volume");
        case FS_EBADNAME:   return ("invalid name");
        case FS_ENOMEM:     return ("out of memory");
        case FS_ECORRUPT:   return ("corrupt filesystem structure");
        case FS_ENOTFS:     return ("not an RBF filesystem");
        case FS_EBUSY:      return ("object in use");
        case FS_EINVAL:     return ("invalid argument");
        case FS_ELOOP:      return ("directory loop");
        case FS_ESECTSIZE:  return ("sector size mismatch");
        case FS_ERANGE:     return ("out of range");
        default:            return ("unknown error");
    }
}

/*
 * Days from 1978-01-01 to the given date.  Uses the "days from civil"
 * method (proleptic Gregorian calendar), shifted so that the year
 * starts in March.
 */
static uint32_t
days_from_civil(unsigned y, unsigned m, unsigned d)
{
    uint32_t era_days;
    unsigned doy;

    if (m <= 2)
        y--;
    doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    era_days = (uint32_t) y * 365 + y / 4 - y / 100 + y / 400 + doy;
    /* 1978-01-01 is day 722390 of this count */
    return (era_days - 722390);
}

fs_time_t
fs_mktime(const fs_tm_t *tm)
{
    uint32_t days;

    if (tm->year < 1978)
        return (0);
    days = days_from_civil(tm->year, tm->month ? tm->month : 1,
                           tm->day ? tm->day : 1);
    return (days * 86400 + tm->hour * 3600 + tm->min * 60 + tm->sec);
}

void
fs_gmtime(fs_time_t t, fs_tm_t *tm)
{
    uint32_t days = t / 86400 + 722390;  /* Days since 0000-03-01 */
    uint32_t secs = t % 86400;
    uint32_t y;
    uint32_t doy;
    unsigned mp;

    /* Inverse of days_from_civil(); day 0 is 0000-03-01 */
    y = (days * 400 + 399) / 146097;      /* Estimate, at most one high */
    while ((y * 365 + y / 4 - y / 100 + y / 400) > days)
        y--;
    doy = days - (y * 365 + y / 4 - y / 100 + y / 400);
    mp  = (5 * doy + 2) / 153;
    tm->day   = doy - (153 * mp + 2) / 5 + 1;
    tm->month = (mp < 10) ? mp + 3 : mp - 9;
    tm->year  = y + (tm->month <= 2);
    tm->hour  = secs / 3600;
    tm->min   = (secs / 60) % 60;
    tm->sec   = secs % 60;
}
