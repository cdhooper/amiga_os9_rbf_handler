/*
 * Filesystem core: backend selection, error strings, time conversion
 */

#include <string.h>
#include "fs_api.h"
#include "blockmap.h"

/* Backends (core9/rbf68k_vol.c, core9k/rbf9k_vol.c) */
int rbf68k_mount(blockdev_t *dev, unsigned cache_buffers, fs_volume_t **vp);
int rbf68k_format(blockdev_t *dev, const fs_format_opts_t *opts);
int rbf9k_mount(blockdev_t *dev, unsigned cache_buffers, fs_volume_t **vp);
int rbf9k_format(blockdev_t *dev, const fs_format_opts_t *opts);

/*
 * OS-9000 is probed first: its ID block has a sync code, while OS-9/68k
 * and OS-9/6809 LSN 0 are recognized by their layout being plausible
 * (rbf68k_mount() tells those two apart).  MBR partitions are tried
 * only when nothing at block 0 is RBF.
 */
static int
mount_backends(blockdev_t *dev, unsigned cache_buffers, fs_volume_t **volp)
{
    int rc;

    rc = rbf9k_mount(dev, cache_buffers, volp);
    if (rc == FS_ENOTFS)
        rc = rbf68k_mount(dev, cache_buffers, volp);
    return (rc);
}

/* ------------------------------------------------------------------ */
/* MBR partitions                                                     */
/* ------------------------------------------------------------------ */

#define MBR_TABLE       446     /* Four 16-byte partition entries */
#define MBR_ENT_SIZE    16
#define MBR_BOOT        0       /* 1: 0x00, or 0x80 if bootable */
#define MBR_TYPE        4       /* 1: partition type, 0 if unused */
#define MBR_START       8       /* 4: first sector (little-endian) */
#define MBR_COUNT       12      /* 4: sectors */

static uint32_t
get_le32(const uint8_t *p)
{
    return (p[0] | (p[1] << 8) | ((uint32_t) p[2] << 16) |
            ((uint32_t) p[3] << 24));
}

/* Extended partitions (not followed) and the GPT protective entry */
static int
mbr_skip_type(uint8_t type)
{
    return ((type == 0x05) || (type == 0x0f) || (type == 0x85) ||
            (type == 0xee));
}

/*
 * Mounts the first primary MBR partition holding an RBF filesystem.
 * The table must be entirely plausible (boot flags 0x00 or 0x80, every
 * used entry inside the device), since PC floppy boot sectors carry the
 * 0x55aa signature too but hold code where the table would be.  Sector
 * numbers count 512-byte sectors, or device blocks if those are larger.
 * Returns FS_ENOTFS if there is no table or no partition is RBF.
 */
static int
mount_mbr(blockdev_t *dev, unsigned cache_buffers, fs_volume_t **volp)
{
    uint32_t    dss = dev->sector_size;
    uint32_t    unit = (dss > 512) ? dss : 512;
    uint64_t    units = ((uint64_t) dev->sector_count * dss) / unit;
    uint8_t     mbr[512];
    uint8_t    *tmp;
    blockmap_t *part;
    int         used = 0;
    int         rc = FS_ENOTFS;
    int         i;

    tmp = fsp_alloc(dss);
    if (tmp == NULL)
        return (FS_ENOMEM);
    i = blockdev_read_bytes(dev, 0, sizeof (mbr), mbr, tmp);
    fsp_free(tmp, dss);
    if (i != 0)
        return (FS_EIO);
    if ((mbr[510] != 0x55) || (mbr[511] != 0xaa))
        return (FS_ENOTFS);
    for (i = 0; i < 4; i++) {
        const uint8_t *e = mbr + MBR_TABLE + i * MBR_ENT_SIZE;
        uint32_t start = get_le32(e + MBR_START);
        uint32_t count = get_le32(e + MBR_COUNT);

        if ((e[MBR_BOOT] != 0x00) && (e[MBR_BOOT] != 0x80))
            return (FS_ENOTFS);
        if (e[MBR_TYPE] == 0)
            continue;
        if ((start == 0) || (count == 0) ||
            ((uint64_t) start + count > units))
            return (FS_ENOTFS);
        used++;
    }
    if (used == 0)
        return (FS_ENOTFS);

    for (i = 0; i < 4; i++) {
        const uint8_t *e = mbr + MBR_TABLE + i * MBR_ENT_SIZE;
        uint64_t start = (uint64_t) get_le32(e + MBR_START) * unit;
        uint64_t len   = (uint64_t) get_le32(e + MBR_COUNT) * unit;
        int      prc;

        if ((e[MBR_TYPE] == 0) || mbr_skip_type(e[MBR_TYPE]))
            continue;
        part = fsp_alloc(sizeof (*part));
        if (part == NULL)
            return (FS_ENOMEM);
        if (blockmap_init_offset(part, dev, dss, start, len) != 0) {
            fsp_free(part, sizeof (*part));
            return (FS_ENOMEM);
        }
        prc = mount_backends(&part->bd, cache_buffers, volp);
        if (prc == FS_OK) {
            (*volp)->part = part;
            fsp_log("rbf: MBR partition %d (type %02x) at sector %lu\n",
                    i + 1, e[MBR_TYPE],
                    (unsigned long) get_le32(e + MBR_START));
            return (FS_OK);
        }
        blockmap_destroy(part);
        fsp_free(part, sizeof (*part));
        if (rc == FS_ENOTFS)
            rc = prc;           /* Report the first real failure */
    }
    return (rc);
}

int
fs_mount(blockdev_t *dev, unsigned cache_buffers, fs_volume_t **volp)
{
    int rc;

    *volp = NULL;
    rc = mount_backends(dev, cache_buffers, volp);
    if (rc == FS_ENOTFS)
        rc = mount_mbr(dev, cache_buffers, volp);
    return (rc);
}

int
fs_unmount(fs_volume_t *vol, int discard)
{
    blockmap_t *part = vol->part;
    int         rc;

    rc = vol->ops->unmount(vol, discard);
    if (part != NULL) {
        blockmap_destroy(part);
        fsp_free(part, sizeof (*part));
    }
    return (rc);
}

int
fs_format(blockdev_t *dev, const fs_format_opts_t *opts)
{
    fs_format_opts_t o = *opts;
    blockdev_t      *vdev = dev;
    blockmap_t       map;
    uint32_t         dss = dev->sector_size;
    uint32_t         ss;
    int              mapped = 0;
    int              rc;

    switch (o.fs_type) {
        case FS_FMT_OS9:
        case FS_FMT_OS9_6809:
        case FS_FMT_OS9K_BE:
        case FS_FMT_OS9K_LE:
            break;
        default:
            return (FS_EINVAL);
    }
    ss = o.sector_size;
    if (ss == 0)
        ss = (o.fs_type == FS_FMT_OS9_6809) ? 256 : dss;
    if ((ss < 256) || (ss > 4096) || ((ss & (ss - 1)) != 0) ||
        ((o.fs_type == FS_FMT_OS9_6809) && (ss != 256)))
        return (FS_ESECTSIZE);
    if (o.half && ((o.fs_type != FS_FMT_OS9_6809) || (ss >= dss)))
        return (FS_EINVAL);

    if (o.half) {
        if (blockmap_init_sparse(&map, dev, ss) != 0)
            return (FS_ENOMEM);
        mapped = 1;
    } else if (ss != dss) {
        if (blockmap_init(&map, dev, ss) != 0)
            return (FS_ENOMEM);
        mapped = 1;
        /* Track size in filesystem sectors */
        if (ss < dss)
            o.sectors_per_track *= dss / ss;
        else
            o.sectors_per_track /= ss / dss;
    }
    if (mapped)
        vdev = &map.bd;
    o.sector_size = ss;

    if ((o.fs_type == FS_FMT_OS9) || (o.fs_type == FS_FMT_OS9_6809))
        rc = rbf68k_format(vdev, &o);
    else
        rc = rbf9k_format(vdev, &o);
    if (mapped)
        blockmap_destroy(&map);
    return (rc);
}

/* ------------------------------------------------------------------ */
/* Format options                                                     */
/* ------------------------------------------------------------------ */

static int
is_delim(char c)
{
    return ((c == ',') || (c == '|') || (c == '/'));
}

/* Case-insensitive compare of an option word with a lowercase keyword */
static int
word_is(const char *w, const char *kw)
{
    for (; *kw != '\0'; w++, kw++) {
        char c = *w;
        if ((c >= 'A') && (c <= 'Z'))
            c += 'a' - 'A';
        if (c != *kw)
            return (0);
    }
    return (*w == '\0');
}

/* Parses a number: decimal, or hex with 0x (always hex if hex is set) */
static int
parse_num(const char *s, int hex, uint32_t *vp)
{
    uint32_t v = 0;
    int      base = hex ? 16 : 10;
    int      digits = 0;

    if ((s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X'))) {
        base = 16;
        s += 2;
    } else if (s[0] == '$') {
        base = 16;
        s++;
    }
    for (; *s != '\0'; s++) {
        char     c = *s;
        uint32_t d;

        if ((c >= '0') && (c <= '9'))
            d = c - '0';
        else if ((c >= 'a') && (c <= 'f'))
            d = c - 'a' + 10;
        else if ((c >= 'A') && (c <= 'F'))
            d = c - 'A' + 10;
        else
            return (FS_EINVAL);
        if (d >= (uint32_t) base)
            return (FS_EINVAL);
        if (v > (0xffffffffUL - d) / base)
            return (FS_EINVAL);
        v = v * base + d;
        digits++;
    }
    if (digits == 0)
        return (FS_EINVAL);
    *vp = v;
    return (FS_OK);
}

static int
parse_option(char *opt, fs_format_opts_t *opts)
{
    char    *val = strchr(opt, '=');
    uint32_t v;
    uint32_t u;

    if (val != NULL)
        *val++ = '\0';
    if (val == NULL) {
        if (word_is(opt, "6809")) {
            opts->fs_type = FS_FMT_OS9_6809;
        } else if (word_is(opt, "68k")) {
            opts->fs_type = FS_FMT_OS9;
        } else if (word_is(opt, "9k") || word_is(opt, "os9k")) {
            if (opts->fs_type != FS_FMT_OS9K_LE)
                opts->fs_type = FS_FMT_OS9K_BE;
        } else if (word_is(opt, "be")) {
            opts->fs_type = FS_FMT_OS9K_BE;
        } else if (word_is(opt, "le")) {
            opts->fs_type = FS_FMT_OS9K_LE;
        } else if (word_is(opt, "half")) {
            opts->half = 1;
        } else {
            return (FS_EINVAL);
        }
        return (FS_OK);
    }

    if (word_is(opt, "ssize") || word_is(opt, "ss")) {
        if (parse_num(val, 0, &v) != FS_OK)
            return (FS_EINVAL);
        opts->sector_size = v;
    } else if (word_is(opt, "cluster") || word_is(opt, "cl")) {
        if ((parse_num(val, 0, &v) != FS_OK) || (v == 0))
            return (FS_EINVAL);
        opts->cluster = v;
    } else if (word_is(opt, "id")) {
        if (parse_num(val, 1, &v) != FS_OK)
            return (FS_EINVAL);
        opts->disk_id = v;
        opts->disk_id_set = 1;
    } else if (word_is(opt, "owner") || word_is(opt, "own")) {
        char *dot = strchr(val, '.');
        if (dot != NULL) {
            *dot++ = '\0';
            if ((parse_num(val, 0, &v) != FS_OK) ||
                (parse_num(dot, 0, &u) != FS_OK) ||
                (v > 0xffff) || (u > 0xffff))
                return (FS_EINVAL);
        } else {
            v = 0;
            if ((parse_num(val, 0, &u) != FS_OK) || (u > 0xffff))
                return (FS_EINVAL);
        }
        opts->owner_group = v;
        opts->owner_user  = u;
    } else {
        return (FS_EINVAL);
    }
    return (FS_OK);
}

int
fs_parse_format_opts(char *name, fs_format_opts_t *opts)
{
    char *p = name;
    int   rc = FS_OK;

    opts->name = name;
    while ((*p != '\0') && !is_delim(*p))
        p++;
    while (*p != '\0') {
        char *opt;

        *p++ = '\0';
        opt = p;
        while ((*p != '\0') && !is_delim(*p))
            p++;
        if (p != opt) {
            char save = *p;
            int  rc2;

            *p = '\0';
            rc2 = parse_option(opt, opts);
            *p = save;
            if (rc == FS_OK)
                rc = rc2;
        }
    }
    return (rc);
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
