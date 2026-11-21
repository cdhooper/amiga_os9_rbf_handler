/*
 * OS-9000 RBF: volume mount, format, allocation bitmap, times
 */

#include <string.h>
#include "rbf9k.h"

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
/* Byte order                                                         */
/* ------------------------------------------------------------------ */

uint32_t
r9_get16(const r9k_vol_t *v, const uint8_t *p)
{
    return (v->le ? (p[0] | (p[1] << 8)) : ((p[0] << 8) | p[1]));
}

uint32_t
r9_get32(const r9k_vol_t *v, const uint8_t *p)
{
    if (v->le)
        return (p[0] | (p[1] << 8) | ((uint32_t) p[2] << 16) |
                ((uint32_t) p[3] << 24));
    return (((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
            (p[2] << 8) | p[3]);
}

void
r9_put16(const r9k_vol_t *v, uint8_t *p, uint32_t x)
{
    if (v->le) {
        p[0] = x;
        p[1] = x >> 8;
    } else {
        p[0] = x >> 8;
        p[1] = x;
    }
}

void
r9_put32(const r9k_vol_t *v, uint8_t *p, uint32_t x)
{
    if (v->le) {
        p[0] = x;
        p[1] = x >> 8;
        p[2] = x >> 16;
        p[3] = x >> 24;
    } else {
        p[0] = x >> 24;
        p[1] = x >> 16;
        p[2] = x >> 8;
        p[3] = x;
    }
}

/* ------------------------------------------------------------------ */
/* Times, parity, names                                               */
/* ------------------------------------------------------------------ */

/*
 * OS-9000 times are seconds from 1970-01-01 (or 1980-01-01 on volumes
 * without IDF_EPOCH70).  Zero means "not set" and stays zero.
 */
fs_time_t
r9k_to_time(const r9k_vol_t *vol, uint32_t t)
{
    if (t == 0)
        return (0);
    if (vol->epoch70)
        return ((t > SECS_1970_1978) ? t - SECS_1970_1978 : 0);
    return ((t < 0xffffffffUL - SECS_1978_1980) ? t + SECS_1978_1980 :
            0xffffffffUL);
}

uint32_t
r9k_from_time(const r9k_vol_t *vol, fs_time_t t)
{
    if (vol->epoch70)
        return ((t < 0xffffffffUL - SECS_1970_1978) ? t + SECS_1970_1978 :
                0xffffffffUL);
    return ((t > SECS_1978_1980) ? t - SECS_1978_1980 : 1);
}

/*
 * Sets the parity word of an FD block: with the parity field all ones,
 * the XOR of the block's 32-bit words is stored there.  Working byte by
 * byte gives the same bytes whatever the volume's byte order.
 */
void
r9k_set_parity(uint8_t *blk, uint32_t size)
{
    uint8_t  p[4] = { 0, 0, 0, 0 };
    uint32_t i;

    memset(blk + FD_PARITY, 0xff, 4);
    for (i = 0; i < size; i += 4) {
        p[0] ^= blk[i];
        p[1] ^= blk[i + 1];
        p[2] ^= blk[i + 2];
        p[3] ^= blk[i + 3];
    }
    memcpy(blk + FD_PARITY, p, 4);
}

/* Copies a NUL-terminated (or maxlen long) on-disk name */
void
r9k_decode_name(const uint8_t *src, int maxlen, char *dst)
{
    int i;

    for (i = 0; (i < maxlen) && (src[i] != 0); i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static int
valid_volname(const char *name)
{
    int len = strlen(name);
    int i;

    if ((len == 0) || (len >= ID_NAME_LEN) || (name[0] == ' '))
        return (FS_EBADNAME);
    for (i = 0; i < len; i++) {
        unsigned char c = name[i];
        if ((c < 0x20) || (c > 0x7e) || (c == ':') || (c == '/'))
            return (FS_EBADNAME);
    }
    return (FS_OK);
}

/* ------------------------------------------------------------------ */
/* Allocation bitmap                                                  */
/* ------------------------------------------------------------------ */

/* Keeps the bitmap block of the current bit pinned while scanning */
typedef struct {
    r9k_vol_t   *vol;
    cache_buf_t *buf;
    uint32_t     index;         /* Bitmap block held in buf */
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

/* Returns a pointer to the bitmap byte holding the bit of block blk */
static int
map_byte(map_cursor_t *mc, uint32_t blk, uint8_t **bytep)
{
    r9k_vol_t *vol = mc->vol;
    uint32_t   index = blk >> (vol->bshift + 3);
    int        rc;

    if ((mc->buf == NULL) || (mc->index != index)) {
        map_release(mc);
        rc = cache_get(&vol->cache, vol->map_lsn + index, 0, &mc->buf);
        if (rc != FS_OK)
            return (rc);
        mc->index = index;
    }
    *bytep = mc->buf->data + ((blk >> 3) & (vol->bsize - 1));
    return (FS_OK);
}

/*
 * Scans blocks [from, to) for a free run of at least want blocks.
 * Returns the first such run, or else the longest run found.
 */
static int
map_find_run(r9k_vol_t *vol, uint32_t from, uint32_t to, uint32_t want,
             uint32_t *startp, uint32_t *lenp)
{
    map_cursor_t mc = { vol, NULL, 0, 0 };
    uint32_t best_start = 0;
    uint32_t best_len = 0;
    uint32_t run_start = 0;
    uint32_t run_len = 0;
    uint32_t b = from;
    uint8_t *bytep;
    int      rc = FS_OK;

    while (b < to) {
        uint8_t byte;

        rc = map_byte(&mc, b, &bytep);
        if (rc != FS_OK)
            break;
        byte = *bytep;
        if (((b & 7) == 0) && (b + 8 <= to) &&
            ((byte == 0xff) || (byte == 0x00))) {
            if (byte == 0xff) {
                run_len = 0;
            } else {
                if (run_len == 0)
                    run_start = b;
                run_len += 8;
            }
            b += 8;
        } else {
            if (byte & (0x80 >> (b & 7))) {
                run_len = 0;
            } else {
                if (run_len == 0)
                    run_start = b;
                run_len++;
            }
            b++;
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

static int
map_mark(r9k_vol_t *vol, uint32_t blk, uint32_t count, int used)
{
    map_cursor_t mc = { vol, NULL, 0, 0 };
    uint8_t     *bytep;
    int          rc = FS_OK;

    for (; count > 0; blk++, count--) {
        uint8_t mask = 0x80 >> (blk & 7);

        rc = map_byte(&mc, blk, &bytep);
        if (rc != FS_OK)
            break;
        if (used) {
            if (*bytep & mask)
                fsp_log("rbf9k: block %lu already allocated\n",
                        (unsigned long) blk);
            *bytep |= mask;
        } else {
            if ((*bytep & mask) == 0)
                fsp_log("rbf9k: block %lu already free\n",
                        (unsigned long) blk);
            *bytep &= ~mask;
        }
        mc.dirty = 1;
    }
    map_release(&mc);
    return (rc);
}

static int
map_count_free(r9k_vol_t *vol, uint32_t *freep)
{
    map_cursor_t mc = { vol, NULL, 0, 0 };
    uint32_t     b;
    uint32_t     nfree = 0;
    uint8_t     *bytep;
    int          rc = FS_OK;

    for (b = 0; b < vol->total; ) {
        rc = map_byte(&mc, b, &bytep);
        if (rc != FS_OK)
            break;
        if (((b & 7) == 0) && (b + 8 <= vol->total)) {
            uint8_t byte = *bytep;
            while (byte != 0xff) {      /* Count zero bits */
                nfree++;
                byte |= byte + 1;
            }
            b += 8;
        } else {
            if ((*bytep & (0x80 >> (b & 7))) == 0)
                nfree++;
            b++;
        }
    }
    map_release(&mc);
    *freep = nfree;
    return (rc);
}

/*
 * Allocates up to want blocks as one contiguous run.  If the block at
 * hint is free the run starts there, so that a file can grow in place.
 * Otherwise the first free run large enough is used, searching from the
 * roving pointer, or the largest run if none is large enough.
 */
int
r9k_alloc(r9k_vol_t *vol, uint32_t want, uint32_t hint, uint32_t *lsnp,
          uint32_t *countp)
{
    uint32_t start = 0;
    uint32_t len = 0;
    int      rc;

    if (vol->read_only)
        return (FS_EROFS);
    if (want == 0)
        want = 1;

    if ((hint != 0) && (hint < vol->total)) {
        uint32_t end = (want < vol->total - hint) ? hint + want : vol->total;

        rc = map_find_run(vol, hint, end, want, &start, &len);
        if (rc != FS_OK)
            return (rc);
        if (start != hint)
            len = 0;
    }
    if (len == 0) {
        uint32_t s2;
        uint32_t l2;

        if (vol->rove >= vol->total)
            vol->rove = 0;
        rc = map_find_run(vol, vol->rove, vol->total, want, &start, &len);
        if ((rc == FS_OK) && (len < want) && (vol->rove > 0)) {
            rc = map_find_run(vol, 0, vol->rove, want, &s2, &l2);
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
    if (vol->free_blocks != FREE_UNKNOWN)
        vol->free_blocks -= len;
    *lsnp   = start;
    *countp = len;
    return (FS_OK);
}

/* Frees blocks [lsn, lsn + count), refusing the volume's own structures */
int
r9k_free(r9k_vol_t *vol, uint32_t lsn, uint32_t count)
{
    uint32_t end = lsn + count;
    int      rc;

    if (count == 0)
        return (FS_OK);
    if ((lsn == 0) || (end > vol->total) || (end < lsn) ||
        ((lsn < vol->map_lsn + vol->map_blocks) && (end > vol->map_lsn)) ||
        ((lsn <= vol->map_fd) && (end > vol->map_fd))) {
        fsp_log("rbf9k: refusing to free blocks %lu+%lu\n",
                (unsigned long) lsn, (unsigned long) count);
        return (FS_ECORRUPT);
    }
    rc = map_mark(vol, lsn, count, 0);
    if (rc == FS_OK) {
        if (vol->free_blocks != FREE_UNKNOWN)
            vol->free_blocks += count;
        if (lsn < vol->rove)
            vol->rove = lsn;
    }
    return (rc);
}

/* ------------------------------------------------------------------ */
/* Volume operations                                                  */
/* ------------------------------------------------------------------ */

int
r9k_sync(fs_volume_t *v)
{
    r9k_vol_t  *vol = R9VOL(v);
    r9k_node_t *n;
    int         rc = FS_OK;
    int         rc2;

    for (n = vol->nodes; n != NULL; n = n->next) {
        if (n->dirty && !n->deleted) {
            rc2 = r9k_node_write(vol, n);
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

static void
vol_free(r9k_vol_t *vol)
{
    r9k_node_t *n;

    while ((n = vol->nodes) != NULL) {
        vol->nodes = n->next;
        r9k_node_free(vol, n);
    }
    cache_destroy(&vol->cache);
    if (vol->zero != NULL)
        fsp_free(vol->zero, vol->bsize);
    if (vol->mapped)
        blockmap_destroy(&vol->map);
    fsp_free(vol, sizeof (*vol));
}

static int
r9k_unmount(fs_volume_t *v, int discard)
{
    int rc = FS_OK;

    if (!discard)
        rc = r9k_sync(v);
    vol_free(R9VOL(v));
    return (rc);
}

static int
r9k_volinfo(fs_volume_t *v, fs_volinfo_t *info)
{
    r9k_vol_t   *vol = R9VOL(v);
    cache_buf_t *b;
    int          rc;

    memset(info, 0, sizeof (*info));
    rc = cache_get(&vol->cache, 0, 0, &b);
    if (rc != FS_OK)
        return (rc);
    r9k_decode_name(b->data + ID_NAME, ID_NAME_LEN - 1, info->name);
    info->disk_id = r9_get32(vol, b->data + ID_DISKID);
    info->created = r9k_to_time(vol, r9_get32(vol, b->data + ID_CTIME));
    cache_put(&vol->cache, b, 0);

    if (vol->free_blocks == FREE_UNKNOWN) {
        rc = map_count_free(vol, &vol->free_blocks);
        if (rc != FS_OK) {
            vol->free_blocks = FREE_UNKNOWN;
            return (rc);
        }
    }
    info->block_size   = vol->bsize;
    info->total_blocks = vol->total;
    info->free_blocks  = vol->free_blocks;
    info->read_only    = vol->read_only;
    info->name_max     = R9K_NAME_MAX;
    info->fs_type      = vol->le ? FS_FMT_OS9K_LE : FS_FMT_OS9K_BE;
    return (FS_OK);
}

static int
r9k_relabel(fs_volume_t *v, const char *name)
{
    r9k_vol_t   *vol = R9VOL(v);
    cache_buf_t *b;
    int          rc;

    if (vol->read_only)
        return (FS_EROFS);
    rc = valid_volname(name);
    if (rc != FS_OK)
        return (rc);
    rc = cache_get(&vol->cache, 0, 0, &b);
    if (rc != FS_OK)
        return (rc);
    memset(b->data + ID_NAME, 0, ID_NAME_LEN);
    memcpy(b->data + ID_NAME, name, strlen(name));
    r9_put32(vol, b->data + ID_MTIME, r9k_from_time(vol, fsp_now()));
    cache_put(&vol->cache, b, 1);
    return (FS_OK);
}

static int
r9k_set_disk_id(fs_volume_t *v, uint32_t disk_id)
{
    r9k_vol_t   *vol = R9VOL(v);
    cache_buf_t *b;
    int          rc;

    if (vol->read_only)
        return (FS_EROFS);
    rc = cache_get(&vol->cache, 0, 0, &b);
    if (rc != FS_OK)
        return (rc);
    r9_put32(vol, b->data + ID_DISKID, disk_id);
    cache_put(&vol->cache, b, 1);
    return (FS_OK);
}

static fs_node_t
r9k_root(fs_volume_t *v)
{
    return (R9VOL(v)->root);
}

static int
r9k_set_cache(fs_volume_t *v, unsigned buffers)
{
    return (cache_resize(&R9VOL(v)->cache, buffers));
}

const struct fs_ops rbf9k_ops = {
    .name        = "OS-9000 RBF",
    .unmount     = r9k_unmount,
    .sync        = r9k_sync,
    .volinfo     = r9k_volinfo,
    .relabel     = r9k_relabel,
    .set_disk_id = r9k_set_disk_id,
    .root        = r9k_root,
    .valid_name  = r9k_valid_name,
    .lookup      = r9k_lookup,
    .stat        = r9k_stat,
    .parent      = r9k_parent,
    .readdir     = r9k_readdir,
    .create      = r9k_create,
    .remove      = r9k_remove,
    .rename      = r9k_rename,
    .setattr     = r9k_setattr,
    .setowner    = r9k_setowner,
    .setmtime    = r9k_setmtime,
    .open        = r9k_open,
    .close       = r9k_close,
    .read        = r9k_read,
    .write       = r9k_write,
    .truncate    = r9k_truncate,
    .file_size   = r9k_file_size,
    .set_cache   = r9k_set_cache,
};

/* ------------------------------------------------------------------ */
/* Mount                                                              */
/* ------------------------------------------------------------------ */

static int
is_fd_block(const uint8_t *b)
{
    return ((b[0] == 0xfd) && (b[1] == 0xb0) && (b[2] == 0xb0) &&
            (b[3] == 0xfd));
}

/*
 * Mounts an OS-9000 RBF filesystem.  The ID block must carry the sync
 * code; anything else is FS_ENOTFS so that other backends can be tried.
 * The byte order is the ID block's end flag; volumes without one are
 * judged by which order gives a sensible block size.
 */
int
rbf9k_mount(blockdev_t *dev, unsigned cache_buffers, fs_volume_t **vp)
{
    uint64_t   dev_bytes = (uint64_t) dev->sector_count * dev->sector_size;
    uint32_t   dss = dev->sector_size;
    uint8_t    idb[ID_SIZE];
    uint8_t    dshift;
    uint8_t   *tmp = NULL;
    uint8_t   *fd = NULL;
    uint32_t   bs = 0;
    uint32_t   seg_count;
    r9k_vol_t *vol = NULL;
    blockdev_t *vdev = dev;
    int        rc;

    *vp = NULL;
    if (!log2_exact(dss, &dshift) || (dss < 256) || (dss > 4096))
        return (FS_ESECTSIZE);
    tmp = fsp_alloc(dss);
    vol = fsp_alloc(sizeof (*vol));
    rc = FS_ENOMEM;
    if ((tmp == NULL) || (vol == NULL))
        goto fail;

    rc = FS_EIO;
    if (blockdev_read_bytes(dev, 0, ID_SIZE, idb, tmp) != 0)
        goto fail;
    rc = FS_ENOTFS;
    if ((idb[0] != 0xad) || (idb[1] != 0xb0) || (idb[2] != 0xb0) ||
        (idb[3] != 0xad))
        goto fail;

    if (idb[ID_ENDFLAG] == R9K_LITTLE_END) {
        vol->le = 1;
    } else if (idb[ID_ENDFLAG] != R9K_BIG_END) {
        uint32_t be = (idb[ID_BLKSIZE] << 8) | idb[ID_BLKSIZE + 1];
        vol->le = !((be >= 256) && (be <= 32768) && ((be & (be - 1)) == 0));
    }
    vol->epoch70 = (r9_get16(vol, idb + ID_FLAGS) & IDF_EPOCH70) != 0;
    bs = r9_get16(vol, idb + ID_BLKSIZE);
    if (!log2_exact(bs, &vol->bshift) || (bs < 256) || (bs > 4096)) {
        bs = 0;
        rc = FS_ESECTSIZE;
        goto fail;
    }
    vol->bsize  = bs;
    vol->total  = r9_get32(vol, idb + ID_TOTBLKS);
    vol->root   = r9_get32(vol, idb + ID_ROOTDIR);
    vol->map_fd = r9_get32(vol, idb + ID_BITMAP);
    if ((vol->total < 8) || ((uint64_t) vol->total * bs > dev_bytes) ||
        (vol->root == 0) || (vol->root >= vol->total) ||
        (vol->map_fd == 0) || (vol->map_fd >= vol->total))
        goto fail;

    rc = FS_ENOMEM;
    fd = fsp_alloc(bs);
    if (fd == NULL)
        goto fail;

    /* The bitmap file: one segment, large enough for every block */
    rc = FS_EIO;
    if (blockdev_read_bytes(dev, (uint64_t) vol->map_fd * bs, bs, fd,
                            tmp) != 0)
        goto fail;
    rc = FS_ENOTFS;
    vol->map_lsn = r9_get32(vol, fd + FD_HDR);
    seg_count    = r9_get32(vol, fd + FD_HDR + 4);
    if (!is_fd_block(fd) || (r9_get32(vol, fd + FD_FLAG) & FDF_SMALL) ||
        (vol->map_lsn == 0) || (seg_count > vol->total) ||
        (vol->map_lsn > vol->total - seg_count) ||
        (((uint64_t) seg_count * bs * 8) < vol->total))
        goto fail;
    vol->map_blocks = seg_count;

    /* The root directory */
    rc = FS_EIO;
    if (blockdev_read_bytes(dev, (uint64_t) vol->root * bs, bs, fd,
                            tmp) != 0)
        goto fail;
    rc = FS_ENOTFS;
    if (!is_fd_block(fd) || !(r9_get32(vol, fd + FD_FLAG) & FDF_DIR))
        goto fail;

    rc = FS_ENOMEM;
    if (bs != dss) {
        if (blockmap_init(&vol->map, dev, bs) != 0)
            goto fail;
        vol->mapped = 1;
        vdev = &vol->map.bd;
        fsp_log("rbf9k: %lu-byte blocks on a %lu-byte device\n",
                (unsigned long) bs, (unsigned long) dss);
    }
    vol->zero = fsp_alloc(bs);
    if (vol->zero == NULL)
        goto fail;
    rc = cache_init(&vol->cache, vdev, cache_buffers);
    if (rc != FS_OK)
        goto fail;

    vol->base.ops    = &rbf9k_ops;
    vol->base.dev    = vdev;
    vol->sas         = R9K_DEFAULT_SAS;
    vol->free_blocks = FREE_UNKNOWN;
    vol->slots       = (bs - FD_HDR - FD_LINK_SIZE) / FD_SEG_SIZE;
    vol->read_only   = dev->read_only;
    fsp_free(fd, bs);
    fsp_free(tmp, dss);
    *vp = &vol->base;
    return (FS_OK);

fail:
    if (vol != NULL) {
        if (vol->zero != NULL)
            fsp_free(vol->zero, vol->bsize);
        if (vol->mapped)
            blockmap_destroy(&vol->map);
        fsp_free(vol, sizeof (*vol));
    }
    if (fd != NULL)
        fsp_free(fd, bs);
    if (tmp != NULL)
        fsp_free(tmp, dss);
    return (rc);
}

/* ------------------------------------------------------------------ */
/* Format                                                             */
/* ------------------------------------------------------------------ */

/* Fills in an FD block for the format: one segment, links to itself */
static void
format_fd(r9k_vol_t *ev, uint8_t *buf, uint32_t lsn, uint32_t flag,
          uint16_t links, uint32_t size, uint32_t start, uint32_t count,
          uint32_t now)
{
    uint32_t bs = ev->bsize;

    memset(buf, 0, bs);
    r9_put32(ev, buf + FD_SYNC, R9K_FD_SYNC);
    r9_put32(ev, buf + FD_FLAG, flag);
    r9_put16(ev, buf + FD_LINKS, links);
    r9_put32(ev, buf + FD_SIZE, size);
    r9_put32(ev, buf + FD_CTIME, now);
    r9_put32(ev, buf + FD_ATIME, now);
    r9_put32(ev, buf + FD_MTIME, now);
    r9_put32(ev, buf + FD_UTIME, now);
    r9_put16(ev, buf + FD_REV, R9K_FD_REV);
    r9_put32(ev, buf + FD_HDR, start);
    r9_put32(ev, buf + FD_HDR + 4, count);
    r9_put32(ev, buf + bs - 8, lsn);
    r9_put32(ev, buf + bs - 4, lsn);
    r9k_set_parity(buf, bs);
}

/*
 * Writes an empty filesystem in the byte order of opts->fs_type:
 *
 *   0  ID block       1  bitmap FD       2  root directory FD
 *   3  bitmap blocks, then one block of root directory data
 *
 * The ID block is written last, so an interrupted format leaves media
 * which does not mount.
 */
int
rbf9k_format(blockdev_t *dev, const fs_format_opts_t *opts)
{
    uint32_t   bs = dev->sector_size;
    uint32_t   total = dev->sector_count;
    uint32_t   map_bytes, map_blocks, map_lsn, dir_lsn, used_end;
    uint32_t   now, disk_id, i;
    uint16_t   fmt;
    uint8_t   *buf;
    r9k_vol_t *ev;
    const char *name = (opts->name != NULL) ? opts->name : "OS-9000";
    int        rc;

    if (dev->read_only)
        return (FS_EROFS);
    if ((bs < 256) || (bs > 4096) || ((bs & (bs - 1)) != 0))
        return (FS_ESECTSIZE);
    if (valid_volname(name) != FS_OK)
        return (FS_EBADNAME);

    map_bytes  = (total + 7) / 8;
    map_blocks = (map_bytes + bs - 1) / bs;
    map_lsn    = 3;
    dir_lsn    = map_lsn + map_blocks;
    used_end   = dir_lsn + 1;
    if (used_end + 8 > total)
        return (FS_ENOSPC);

    ev  = fsp_alloc(sizeof (*ev));
    buf = fsp_alloc(bs);
    rc  = FS_ENOMEM;
    if ((ev == NULL) || (buf == NULL))
        goto done;
    ev->le      = (opts->fs_type == FS_FMT_OS9K_LE);
    ev->epoch70 = 1;
    ev->bsize   = bs;
    now = r9k_from_time(ev, fsp_now());
    disk_id = opts->disk_id;
    if (disk_id == 0)
        disk_id = (fsp_now() * 2654435761UL) | 1;

    /* Bitmap: system area and root directory used, beyond media used */
    rc = FS_EIO;
    for (i = 0; i < map_blocks; i++) {
        uint32_t base = i * bs * 8;
        uint32_t b;

        memset(buf, 0, bs);
        for (b = 0; b < bs * 8; b++) {
            uint32_t blk = base + b;
            if ((blk < used_end) || (blk >= total))
                buf[b >> 3] |= 0x80 >> (b & 7);
        }
        if (dev->write(dev, map_lsn + i, 1, buf) != 0)
            goto done;
    }

    /* Root directory data: ".." and "." both refer to the root */
    memset(buf, 0, bs);
    buf[0] = '.';
    buf[1] = '.';
    r9_put32(ev, buf + DIR_FD, 2);
    buf[DIR_ENT_SIZE] = '.';
    r9_put32(ev, buf + DIR_ENT_SIZE + DIR_FD, 2);
    if (dev->write(dev, dir_lsn, 1, buf) != 0)
        goto done;

    /* Bitmap file FD; its size covers its whole blocks */
    format_fd(ev, buf, 1, FDF_OWNER_R | FDF_OWNER_W, 1, map_blocks * bs,
              map_lsn, map_blocks, now);
    if (dev->write(dev, 1, 1, buf) != 0)
        goto done;

    /* Root directory FD: links are its ".", its "..", and the volume */
    format_fd(ev, buf, 2, FDF_DIR | FDF_PERMS, 3, 2 * DIR_ENT_SIZE,
              dir_lsn, 1, now);
    if (dev->write(dev, 2, 1, buf) != 0)
        goto done;

    /* ID block */
    fmt = 0;
    if (opts->heads > 1)
        fmt |= IDFMT_DBLSIDE;
    if ((opts->cylinders > 100) || (total > 5760))
        fmt |= IDFMT_HARDDISK;
    else
        fmt |= IDFMT_DBLBIT | ((opts->cylinders > 45) ? IDFMT_DBLTRK : 0);
    memset(buf, 0, bs);
    r9_put32(ev, buf + ID_SYNC, R9K_IDB_SYNC);
    r9_put32(ev, buf + ID_DISKID, disk_id);
    r9_put32(ev, buf + ID_TOTBLKS, total);
    r9_put16(ev, buf + ID_CYLS, opts->cylinders);
    r9_put16(ev, buf + ID_CYL0SIZE, opts->sectors_per_track);
    r9_put16(ev, buf + ID_CYLSIZE, opts->sectors_per_track);
    r9_put16(ev, buf + ID_HEADS, opts->heads);
    r9_put16(ev, buf + ID_BLKSIZE, bs);
    r9_put16(ev, buf + ID_FORMAT, fmt);
    r9_put16(ev, buf + ID_FLAGS, IDF_EPOCH70);
    r9_put32(ev, buf + ID_BITMAP, 1);
    r9_put32(ev, buf + ID_ROOTDIR, 2);
    r9_put32(ev, buf + ID_CTIME, now);
    r9_put32(ev, buf + ID_MTIME, now);
    memcpy(buf + ID_NAME, name, strlen(name));
    buf[ID_ENDFLAG] = ev->le ? R9K_LITTLE_END : R9K_BIG_END;
    if (dev->write(dev, 0, 1, buf) != 0)
        goto done;
    if ((dev->sync != NULL) && (dev->sync(dev) != 0))
        goto done;
    rc = FS_OK;
done:
    if (buf != NULL)
        fsp_free(buf, bs);
    if (ev != NULL)
        fsp_free(ev, sizeof (*ev));
    return (rc);
}
