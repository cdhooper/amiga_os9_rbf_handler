/*
 * OS-9/68k RBF: volume mount, format, allocation bitmap, dates
 */

#include <string.h>
#include "rbf68k.h"

static int
log2_exact(uint32_t v, uint8_t *shift)
{
    uint8_t s = 0;

    if ((v == 0) || ((v & (v - 1)) != 0))
        return (0);
    while ((1UL << s) != v)
        s++;
    *shift = s;
    return (1);
}

/* ------------------------------------------------------------------ */
/* Dates                                                              */
/* ------------------------------------------------------------------ */

/*
 * OS-9 dates are YY MM DD [HH MM] with YY = year - 1900.  Dates before
 * 1978 (the start of fs_time_t) become 1978-01-01.
 */
fs_time_t
rbf_date_to_time(const uint8_t *date, int len)
{
    fs_tm_t tm;

    tm.year  = 1900 + date[0];
    tm.month = date[1];
    tm.day   = date[2];
    tm.hour  = (len > 3) ? date[3] : 0;
    tm.min   = (len > 4) ? date[4] : 0;
    tm.sec   = 0;
    if ((tm.month < 1) || (tm.month > 12))
        tm.month = 1;
    if ((tm.day < 1) || (tm.day > 31))
        tm.day = 1;
    if (tm.hour > 23)
        tm.hour = 0;
    if (tm.min > 59)
        tm.min = 0;
    return (fs_mktime(&tm));
}

void
rbf_time_to_date(fs_time_t t, uint8_t *date, int len)
{
    fs_tm_t tm;
    unsigned year;

    fs_gmtime(t, &tm);
    year = tm.year - 1900;
    if (year > 255)
        year = 255;
    date[0] = year;
    date[1] = tm.month;
    date[2] = tm.day;
    if (len > 3)
        date[3] = tm.hour;
    if (len > 4)
        date[4] = tm.min;
}

/* ------------------------------------------------------------------ */
/* Allocation bitmap                                                  */
/* ------------------------------------------------------------------ */

/*
 * Bitmap cursor: keeps the bitmap sector holding the current cluster
 * pinned in the cache while scanning.
 */
typedef struct {
    rbf_vol_t   *vol;
    cache_buf_t *buf;
    uint32_t     sector;        /* Bitmap sector index held in buf */
    int          dirty;
} map_cursor_t;

static void
map_release(map_cursor_t *mc)
{
    if (mc->buf != NULL) {
        cache_put(&mc->vol->cache, mc->buf, mc->dirty);
        mc->buf = NULL;
    }
    mc->dirty = 0;
}

/* Returns a pointer to the byte holding the bit for the cluster */
static int
map_byte(map_cursor_t *mc, uint32_t cl, uint8_t **bytep)
{
    rbf_vol_t *vol = mc->vol;
    uint32_t   sector = cl >> (vol->sector_shift + 3);
    int        rc;

    if ((mc->buf == NULL) || (mc->sector != sector)) {
        map_release(mc);
        rc = cache_get(&vol->cache, vol->map_lsn + sector, 0, &mc->buf);
        if (rc != FS_OK)
            return (rc);
        mc->sector = sector;
    }
    *bytep = mc->buf->data + ((cl >> 3) & (vol->sector_size - 1));
    return (FS_OK);
}

/*
 * Scans clusters [from, to) for a free run of at least want clusters.
 * Returns the first such run, or else the longest run found.
 */
static int
map_find_run(rbf_vol_t *vol, uint32_t from, uint32_t to, uint32_t want,
             uint32_t *startp, uint32_t *lenp)
{
    map_cursor_t mc = { vol, NULL, 0, 0 };
    uint32_t best_start = 0;
    uint32_t best_len = 0;
    uint32_t run_start = 0;
    uint32_t run_len = 0;
    uint32_t cl = from;
    uint8_t *bytep;
    int      rc = FS_OK;

    while (cl < to) {
        uint8_t byte;
        rc = map_byte(&mc, cl, &bytep);
        if (rc != FS_OK)
            break;
        byte = *bytep;
        if (((cl & 7) == 0) && (cl + 8 <= to) &&
            ((byte == 0xff) || (byte == 0x00))) {
            /* Whole byte used or free */
            if (byte == 0xff) {
                run_len = 0;
            } else {
                if (run_len == 0)
                    run_start = cl;
                run_len += 8;
            }
            cl += 8;
        } else {
            if (byte & (0x80 >> (cl & 7))) {
                run_len = 0;
            } else {
                if (run_len == 0)
                    run_start = cl;
                run_len++;
            }
            cl++;
        }
        if (run_len > best_len) {
            best_len   = run_len;
            best_start = run_start;
            if (best_len >= want)
                break;
        }
    }
    map_release(&mc);
    if (best_len > want)
        best_len = want;
    *startp = best_start;
    *lenp   = best_len;
    return (rc);
}

/* Counts free clusters starting at cl, up to max */
static int
map_free_at(rbf_vol_t *vol, uint32_t cl, uint32_t max, uint32_t *lenp)
{
    uint32_t start;
    uint32_t len;
    uint32_t end = cl + max;
    int      rc;

    if (end > vol->clusters)
        end = vol->clusters;
    if (cl >= end) {
        *lenp = 0;
        return (FS_OK);
    }
    rc = map_find_run(vol, cl, end, max, &start, &len);
    *lenp = ((rc == FS_OK) && (start == cl)) ? len : 0;
    return (rc);
}

static int
map_mark(rbf_vol_t *vol, uint32_t cl, uint32_t count, int used)
{
    map_cursor_t mc = { vol, NULL, 0, 0 };
    uint8_t     *bytep;
    int          rc = FS_OK;

    for (; count > 0; cl++, count--) {
        uint8_t mask = 0x80 >> (cl & 7);
        rc = map_byte(&mc, cl, &bytep);
        if (rc != FS_OK)
            break;
        if (used) {
            if (*bytep & mask)
                fsp_log("rbf: cluster %lu already allocated\n",
                        (unsigned long) cl);
            *bytep |= mask;
        } else {
            if ((*bytep & mask) == 0)
                fsp_log("rbf: cluster %lu already free\n",
                        (unsigned long) cl);
            *bytep &= ~mask;
        }
        mc.dirty = 1;
    }
    map_release(&mc);
    return (rc);
}

static int
map_count_free(rbf_vol_t *vol, uint32_t *freep)
{
    map_cursor_t mc = { vol, NULL, 0, 0 };
    uint32_t     cl;
    uint32_t     nfree = 0;
    uint8_t     *bytep;
    int          rc = FS_OK;

    for (cl = 0; cl < vol->clusters; ) {
        rc = map_byte(&mc, cl, &bytep);
        if (rc != FS_OK)
            break;
        if (((cl & 7) == 0) && (cl + 8 <= vol->clusters)) {
            uint8_t byte = *bytep;
            while (byte != 0xff) {      /* Count zero bits */
                nfree++;
                byte |= byte + 1;
            }
            cl += 8;
        } else {
            if ((*bytep & (0x80 >> (cl & 7))) == 0)
                nfree++;
            cl++;
        }
    }
    map_release(&mc);
    *freep = nfree << vol->cluster_shift;
    return (rc);
}

/*
 * Allocates up to want sectors (rounded up to whole clusters) as one
 * contiguous run.  If hint is a cluster-aligned LSN which is free, the
 * run starts there so that a file can grow in place.  Otherwise the
 * first free run large enough is used, searching from the roving
 * pointer, or the largest run if none is large enough.
 */
int
rbf_alloc(rbf_vol_t *vol, uint32_t want, uint32_t hint, uint32_t *lsnp,
          uint32_t *countp)
{
    uint32_t want_cl;
    uint32_t max_cl = FD_SEG_MAX_LEN >> vol->cluster_shift;
    uint32_t start = 0;
    uint32_t len = 0;
    int      rc;

    if (vol->read_only)
        return (FS_EROFS);
    if (want == 0)
        want = 1;
    want_cl = (want + vol->cluster - 1) >> vol->cluster_shift;
    if (max_cl == 0)
        max_cl = 1;
    if (want_cl > max_cl)
        want_cl = max_cl;

    if ((hint != 0) && ((hint & (vol->cluster - 1)) == 0)) {
        start = hint >> vol->cluster_shift;
        rc = map_free_at(vol, start, want_cl, &len);
        if (rc != FS_OK)
            return (rc);
    }
    if (len == 0) {
        uint32_t s2;
        uint32_t l2;

        if (vol->rove >= vol->clusters)
            vol->rove = 0;
        rc = map_find_run(vol, vol->rove, vol->clusters, want_cl, &start,
                          &len);
        if ((rc == FS_OK) && (len < want_cl) && (vol->rove > 0)) {
            rc = map_find_run(vol, 0, vol->rove, want_cl, &s2, &l2);
            if (l2 > len) {
                start = s2;
                len   = l2;
            }
        }
        if (rc != FS_OK)
            return (rc);
    }
    if (len == 0)
        return (FS_ENOSPC);

    rc = map_mark(vol, start, len, 1);
    if (rc != FS_OK)
        return (rc);
    vol->rove = start + len;
    if (vol->free_sectors != FREE_UNKNOWN)
        vol->free_sectors -= len << vol->cluster_shift;
    *lsnp   = start << vol->cluster_shift;
    *countp = len << vol->cluster_shift;
    return (FS_OK);
}

/*
 * Frees the clusters lying entirely within sectors [lsn, lsn + count).
 * A partial cluster at either end stays allocated.
 */
int
rbf_free(rbf_vol_t *vol, uint32_t lsn, uint32_t count)
{
    uint32_t first = (lsn + vol->cluster - 1) >> vol->cluster_shift;
    uint32_t end   = (lsn + count) >> vol->cluster_shift;
    uint32_t sys_end = vol->map_lsn + vol->map_sectors;
    int      rc;

    if (end <= first)
        return (FS_OK);
    if ((lsn < sys_end) || (end > vol->clusters)) {
        fsp_log("rbf: refusing to free sectors %lu+%lu\n",
                (unsigned long) lsn, (unsigned long) count);
        return (FS_ECORRUPT);
    }
    rc = map_mark(vol, first, end - first, 0);
    if (rc == FS_OK) {
        if (vol->free_sectors != FREE_UNKNOWN)
            vol->free_sectors += (end - first) << vol->cluster_shift;
        if (first < vol->rove)
            vol->rove = first;
    }
    return (rc);
}

/* ------------------------------------------------------------------ */
/* Volume operations                                                  */
/* ------------------------------------------------------------------ */

int
rbf_valid_volname(const char *name)
{
    int len = strlen(name);
    int i;

    if ((len == 0) || (len > DD_NAME_LEN) || (name[0] == ' '))
        return (FS_EBADNAME);
    for (i = 0; i < len; i++) {
        unsigned char c = name[i];
        if ((c < 0x20) || (c > 0x7e) || (c == ':') || (c == '/'))
            return (FS_EBADNAME);
    }
    return (FS_OK);
}

static int
read_lsn0(rbf_vol_t *vol, cache_buf_t **bp)
{
    return (cache_get(&vol->cache, 0, 0, bp));
}

int
rbf_sync(fs_volume_t *v)
{
    rbf_vol_t  *vol = RVOL(v);
    rbf_node_t *n;
    int         rc = FS_OK;
    int         rc2;

    for (n = vol->nodes; n != NULL; n = n->next) {
        if (n->dirty && !n->deleted) {
            rc2 = node_write(vol, n);
            if (rc == FS_OK)
                rc = rc2;
        }
    }
    rc2 = cache_flush(&vol->cache);
    if (rc == FS_OK)
        rc = rc2;
    if ((vol->base.dev->sync != NULL) && !vol->read_only &&
        (vol->base.dev->sync(vol->base.dev) != 0) && (rc == FS_OK))
        rc = FS_EIO;
    return (rc);
}

static int
rbf_unmount(fs_volume_t *v, int discard)
{
    rbf_vol_t  *vol = RVOL(v);
    rbf_node_t *n;
    int         rc = FS_OK;

    if (!discard)
        rc = rbf_sync(v);
    while ((n = vol->nodes) != NULL) {
        vol->nodes = n->next;
        fsp_free(n->fd, vol->sector_size);
        fsp_free(n, sizeof (*n));
    }
    cache_destroy(&vol->cache);
    if (vol->zero != NULL)
        fsp_free(vol->zero, vol->sector_size);
    if (vol->mapped)
        blockmap_destroy(&vol->map);
    fsp_free(vol, sizeof (*vol));
    return (rc);
}

static int
rbf_volinfo(fs_volume_t *v, fs_volinfo_t *info)
{
    rbf_vol_t   *vol = RVOL(v);
    cache_buf_t *b;
    int          rc;

    memset(info, 0, sizeof (*info));
    rc = read_lsn0(vol, &b);
    if (rc != FS_OK)
        return (rc);
    rbf_decode_name(b->data + DD_NAME, DD_NAME_LEN, info->name);
    info->disk_id = get16(b->data + DD_DSK);
    info->created = rbf_date_to_time(b->data + DD_DATE, 5);
    cache_put(&vol->cache, b, 0);

    if (vol->free_sectors == FREE_UNKNOWN) {
        rc = map_count_free(vol, &vol->free_sectors);
        if (rc != FS_OK) {
            vol->free_sectors = FREE_UNKNOWN;
            return (rc);
        }
    }
    info->block_size   = vol->sector_size;
    info->total_blocks = vol->total;
    info->free_blocks  = vol->free_sectors;
    info->read_only    = vol->read_only;
    info->name_max     = RBF_NAME_MAX;
    info->fs_type      = FS_FMT_OS9;
    return (FS_OK);
}

static int
rbf_relabel(fs_volume_t *v, const char *name)
{
    rbf_vol_t   *vol = RVOL(v);
    cache_buf_t *b;
    int          rc;

    if (vol->read_only)
        return (FS_EROFS);
    rc = rbf_valid_volname(name);
    if (rc != FS_OK)
        return (rc);
    rc = read_lsn0(vol, &b);
    if (rc != FS_OK)
        return (rc);
    rbf_encode_name(name, b->data + DD_NAME, DD_NAME_LEN);
    cache_put(&vol->cache, b, 1);
    return (FS_OK);
}

static int
rbf_set_disk_id(fs_volume_t *v, uint32_t disk_id)
{
    rbf_vol_t   *vol = RVOL(v);
    cache_buf_t *b;
    int          rc;

    if (vol->read_only)
        return (FS_EROFS);
    rc = read_lsn0(vol, &b);
    if (rc != FS_OK)
        return (rc);
    put16(b->data + DD_DSK, disk_id);
    cache_put(&vol->cache, b, 1);
    return (FS_OK);
}

static fs_node_t
rbf_root(fs_volume_t *v)
{
    return (RVOL(v)->root);
}

static int
rbf_set_cache(fs_volume_t *v, unsigned buffers)
{
    return (cache_resize(&RVOL(v)->cache, buffers));
}

const struct fs_ops rbf68k_ops = {
    .name        = "OS-9/68k RBF",
    .unmount     = rbf_unmount,
    .sync        = rbf_sync,
    .volinfo     = rbf_volinfo,
    .relabel     = rbf_relabel,
    .set_disk_id = rbf_set_disk_id,
    .root        = rbf_root,
    .valid_name  = rbf_valid_name,
    .lookup      = rbf_lookup,
    .stat        = rbf_stat,
    .parent      = rbf_parent,
    .readdir     = rbf_readdir,
    .create      = rbf_create,
    .remove      = rbf_remove,
    .rename      = rbf_rename,
    .setattr     = rbf_setattr,
    .setowner    = rbf_setowner,
    .setmtime    = rbf_setmtime,
    .open        = rbf_open,
    .close       = rbf_close,
    .read        = rbf_read,
    .write       = rbf_write,
    .truncate    = rbf_truncate,
    .file_size   = rbf_file_size,
    .set_cache   = rbf_set_cache,
};

/*
 * Checks that LSN 0 describes an RBF filesystem with sector size ss
 * which fits on the device.  RBF has no magic number, so this validates
 * the layout fields and the root directory's FD.  With strict, the
 * root directory's data must also begin with ".." and "." entries, and
 * "." must point back at the root FD.  That is used to find the sector
 * size of disks whose LSN 0 does not record it.  tmp holds one device
 * block; buf holds 4096 bytes.
 */
static int
rbf_layout_ok(blockdev_t *dev, const uint8_t *lsn0, uint32_t ss, int strict,
              uint8_t *buf, uint8_t *tmp)
{
    uint64_t dev_bytes = (uint64_t) dev->sector_count * dev->sector_size;
    uint32_t tot    = get24(lsn0 + DD_TOT);
    uint32_t bit    = get16(lsn0 + DD_BIT);
    uint32_t map    = get16(lsn0 + DD_MAP);
    uint32_t dir    = get24(lsn0 + DD_DIR);
    uint32_t maplsn = get32(lsn0 + DD_MAPLSN);
    uint32_t map_sectors;
    uint32_t seg;
    uint32_t seglen;
    uint8_t  shift;

    if (bit == 0)
        bit = 1;                /* As RBF does */
    if ((tot < 4) || ((uint64_t) tot * ss > dev_bytes) ||
        !log2_exact(bit, &shift))
        return (0);
    if (((uint32_t) map << 3) < (tot + bit - 1) / bit)
        return (0);             /* Bitmap too small for the media */
    if ((maplsn != 0) && (maplsn != 1))
        return (0);             /* Only OS-9000 moves the bitmap */
    map_sectors = (map + ss - 1) / ss;
    if ((dir < 1 + map_sectors) || (dir >= tot))
        return (0);

    /* The root directory FD must describe a directory */
    if (blockdev_read_bytes(dev, (uint64_t) dir * ss, ss, buf, tmp) != 0)
        return (-1);
    if ((buf[FD_ATT] & FS_ATTR_DIR) == 0)
        return (0);
    if (!strict)
        return (1);

    seg    = get24(buf + FD_SEG);
    seglen = get16(buf + FD_SEG + 3);
    if ((get32(buf + FD_SIZ) < 2 * DIR_ENT_SIZE) || (seglen == 0) ||
        (seg < 1 + map_sectors) || (seg + seglen > tot))
        return (0);
    if (blockdev_read_bytes(dev, (uint64_t) seg * ss, 2 * DIR_ENT_SIZE,
                            buf, tmp) != 0)
        return (-1);
    if ((buf[0] != '.') || (buf[1] != ('.' | 0x80)) ||
        (buf[DIR_ENT_SIZE] != ('.' | 0x80)) ||
        (get32(buf + DIR_ENT_SIZE + DIR_FD) != dir))
        return (0);
    return (1);
}

/*
 * Mounts an OS-9/68k RBF filesystem.  The sector size is DD_LSNSIZE
 * when LSN 0 records it.  Older disks leave it 0; then each size from
 * 256 to 4096 bytes is tried (the device block size first) and the
 * first whose layout and root directory check out is used.  If it
 * differs from the device block size, I/O goes through a blockmap.
 */
int
rbf68k_mount(blockdev_t *dev, unsigned cache_buffers, fs_volume_t **vp)
{
    uint32_t   dss = dev->sector_size;
    uint32_t   ss = 0;
    uint8_t    ss_shift;
    uint8_t    cl_shift = 0;
    uint8_t   *tmp;
    uint8_t    lsn0[256];
    uint8_t   *buf;
    rbf_vol_t *vol = NULL;
    blockdev_t *vdev = dev;
    uint32_t   tot, bit, map, dir, lsnsize;
    int        rc;
    int        ok = 0;

    *vp = NULL;
    if (!log2_exact(dss, &ss_shift) || (dss < 256) || (dss > 4096))
        return (FS_ESECTSIZE);

    tmp = fsp_alloc(dss);
    buf = fsp_alloc(4096);
    if ((tmp == NULL) || (buf == NULL)) {
        rc = FS_ENOMEM;
        goto fail;
    }

    rc = FS_EIO;
    if (blockdev_read_bytes(dev, 0, sizeof (lsn0), lsn0, tmp) != 0)
        goto fail;

    rc = FS_ENOTFS;
    lsnsize = get16(lsn0 + DD_LSNSIZE);
    if (lsnsize != 0) {
        if (!log2_exact(lsnsize, &ss_shift) || (lsnsize < 256) ||
            (lsnsize > 4096)) {
            rc = FS_ESECTSIZE;
            goto fail;
        }
        ss = lsnsize;
        ok = rbf_layout_ok(dev, lsn0, ss, 0, buf, tmp);
    } else {
        uint32_t try;

        ok = rbf_layout_ok(dev, lsn0, dss, 1, buf, tmp);
        if (ok > 0)
            ss = dss;
        for (try = 256; (ok == 0) && (try <= 4096); try <<= 1) {
            if (try == dss)
                continue;
            ok = rbf_layout_ok(dev, lsn0, try, 1, buf, tmp);
            if (ok > 0)
                ss = try;
        }
    }
    if (ok < 0)
        rc = FS_EIO;
    if (ok <= 0)
        goto fail;
    log2_exact(ss, &ss_shift);

    tot = get24(lsn0 + DD_TOT);
    bit = get16(lsn0 + DD_BIT);
    map = get16(lsn0 + DD_MAP);
    dir = get24(lsn0 + DD_DIR);
    if (bit == 0)
        bit = 1;
    log2_exact(bit, &cl_shift);

    rc = FS_ENOMEM;
    vol = fsp_alloc(sizeof (*vol));
    if (vol == NULL)
        goto fail;
    if (ss != dss) {
        if (blockmap_init(&vol->map, dev, ss) != 0)
            goto fail;
        vol->mapped = 1;
        vdev = &vol->map.bd;
        fsp_log("rbf: %lu-byte sectors on a %lu-byte device\n",
                (unsigned long) ss, (unsigned long) dss);
    }
    vol->zero = fsp_alloc(ss);
    if (vol->zero == NULL)
        goto fail;
    rc = cache_init(&vol->cache, vdev, cache_buffers);
    if (rc != FS_OK)
        goto fail;

    vol->base.ops      = &rbf68k_ops;
    vol->base.dev      = vdev;
    vol->sector_size   = ss;
    vol->sector_shift  = ss_shift;
    vol->total         = tot;
    vol->cluster       = bit;
    vol->cluster_shift = cl_shift;
    vol->clusters      = tot >> cl_shift;  /* Only whole clusters */
    vol->map_bytes     = map;
    vol->map_lsn       = 1;
    vol->map_sectors   = (map + ss - 1) >> ss_shift;
    vol->root          = dir;
    vol->sas           = (bit > RBF_DEFAULT_SAS) ? bit : RBF_DEFAULT_SAS;
    vol->rove          = 0;
    vol->free_sectors  = FREE_UNKNOWN;
    vol->max_segs      = (ss - FD_SEG) / FD_SEG_SIZE;
    vol->read_only     = dev->read_only;
    fsp_free(buf, 4096);
    fsp_free(tmp, dss);
    *vp = &vol->base;
    return (FS_OK);

fail:
    if (vol != NULL) {
        if (vol->zero != NULL)
            fsp_free(vol->zero, ss);
        if (vol->mapped)
            blockmap_destroy(&vol->map);
        fsp_free(vol, sizeof (*vol));
    }
    if (buf != NULL)
        fsp_free(buf, 4096);
    if (tmp != NULL)
        fsp_free(tmp, dss);
    return (rc);
}

/*
 * Writes an empty filesystem: LSN 0, the bitmap from LSN 1, and a root
 * directory in the first cluster after the bitmap.  The layout matches
 * what OS-9 format produces.  LSN 0 is written last, so an interrupted
 * format leaves media which does not mount.
 */
int
rbf68k_format(blockdev_t *dev, const fs_format_opts_t *opts)
{
    uint32_t ss = dev->sector_size;
    uint8_t  ss_shift;
    uint8_t  cl_shift;
    uint32_t total = dev->sector_count;
    uint32_t cluster = (opts->cluster != 0) ? opts->cluster : 1;
    uint32_t nclusters, map_bytes, map_sectors;
    uint32_t root_fd, dir_lsn, dir_len, used_end;
    uint32_t i;
    uint8_t *buf;
    uint8_t  date[5];
    uint32_t disk_id;
    const char *name = (opts->name != NULL) ? opts->name : "OS-9";
    int      rc;

    if (dev->read_only)
        return (FS_EROFS);
    if (!log2_exact(ss, &ss_shift) || (ss < 256) || (ss > 4096))
        return (FS_ESECTSIZE);
    if (!log2_exact(cluster, &cl_shift) || (cluster > 0x8000))
        return (FS_EINVAL);
    if (rbf_valid_volname(name) != FS_OK)
        return (FS_EBADNAME);
    if (total > RBF_MAX_LSN)
        total = RBF_MAX_LSN;

    /* DD_MAP is 16 bits: grow the cluster until the bitmap fits */
    while (((total >> cl_shift) + 7) / 8 > RBF_MAX_MAP) {
        cl_shift++;
        cluster <<= 1;
    }
    total     = (total >> cl_shift) << cl_shift;    /* Whole clusters */
    nclusters = total >> cl_shift;
    map_bytes = (nclusters + 7) / 8;
    map_sectors = (map_bytes + ss - 1) >> ss_shift;

    /* Root directory FD on the first cluster after the bitmap */
    root_fd = ((1 + map_sectors + cluster - 1) >> cl_shift) << cl_shift;
    dir_lsn = root_fd + 1;
    dir_len = (cluster > 1) ? cluster - 1 : 1;
    used_end = dir_lsn + dir_len;           /* Cluster aligned */
    if (used_end + cluster > total)
        return (FS_ENOSPC);

    buf = fsp_alloc(ss);
    if (buf == NULL)
        return (FS_ENOMEM);

    rbf_time_to_date(fsp_now(), date, 5);
    disk_id = opts->disk_id;
    if (disk_id == 0)
        disk_id = ((fsp_now() * 2654435761UL) >> 16) | 1;

    /* Bitmap: system area and root directory used, beyond media used */
    rc = FS_EIO;
    for (i = 0; i < map_sectors; i++) {
        uint32_t bitbase = i << (ss_shift + 3);
        uint32_t b;

        memset(buf, 0, ss);
        for (b = 0; b < (ss << 3); b++) {
            uint32_t cl = bitbase + b;
            if ((cl < (used_end >> cl_shift)) || (cl >= nclusters))
                buf[b >> 3] |= 0x80 >> (b & 7);
        }
        if (dev->write(dev, 1 + i, 1, buf) != 0)
            goto done;
    }

    /* Root directory data: ".." and "." both refer to the root */
    for (i = 0; i < dir_len; i++) {
        memset(buf, 0, ss);
        if (i == 0) {
            rbf_encode_name("..", buf, DIR_NAME_LEN);
            put32(buf + DIR_FD, root_fd);
            rbf_encode_name(".", buf + DIR_ENT_SIZE, DIR_NAME_LEN);
            put32(buf + DIR_ENT_SIZE + DIR_FD, root_fd);
        }
        if (dev->write(dev, dir_lsn + i, 1, buf) != 0)
            goto done;
    }

    /* Root directory FD */
    memset(buf, 0, ss);
    buf[FD_ATT] = FS_ATTR_DIR | FS_ATTR_PEXEC | FS_ATTR_PWRITE |
                  FS_ATTR_PREAD | FS_ATTR_EXEC | FS_ATTR_WRITE |
                  FS_ATTR_READ;
    memcpy(buf + FD_DAT, date, 5);
    buf[FD_LNK] = 1;
    put32(buf + FD_SIZ, 2 * DIR_ENT_SIZE);
    memcpy(buf + FD_DCR, date, 3);
    put24(buf + FD_SEG, dir_lsn);
    put16(buf + FD_SEG + 3, dir_len);
    if (dev->write(dev, root_fd, 1, buf) != 0)
        goto done;

    /* Identification sector */
    memset(buf, 0, ss);
    put24(buf + DD_TOT, total);
    buf[DD_TKS] = opts->sectors_per_track;
    put16(buf + DD_MAP, map_bytes);
    put16(buf + DD_BIT, cluster);
    put24(buf + DD_DIR, root_fd);
    buf[DD_ATT] = FS_ATTR_DIR | (FS_ATTR_PERMS & ~FS_ATTR_NONSHARE);
    put16(buf + DD_DSK, disk_id);
    buf[DD_FMT] = FMT_DD | ((opts->heads > 1) ? FMT_DS : 0) |
                  ((opts->cylinders > 45) ? FMT_DT : 0);
    put16(buf + DD_SPT, opts->sectors_per_track);
    memcpy(buf + DD_DATE, date, 5);
    rbf_encode_name(name, buf + DD_NAME, DD_NAME_LEN);
    buf[DD_OPT] = 1;                        /* PD_DTP: DT_RBF */
    put32(buf + DD_SYNC, DD_SYNC_CODE);
    put32(buf + DD_MAPLSN, 1);
    put16(buf + DD_LSNSIZE, ss);
    put16(buf + DD_VERSID, 1);
    if (dev->write(dev, 0, 1, buf) != 0)
        goto done;
    if ((dev->sync != NULL) && (dev->sync(dev) != 0))
        goto done;
    rc = FS_OK;
done:
    fsp_free(buf, ss);
    return (rc);
}
