/*
 * OS-9/68k RBF: file data I/O
 *
 * Whole sectors move directly between the caller's buffer and the
 * device (several at a time when a segment is contiguous); partial
 * sectors go through the sector cache.
 */

#include <string.h>
#include "rbf68k.h"

/* Reads bytes [pos, pos + len) of the file's allocated space */
int
node_read_bytes(rbf_vol_t *vol, rbf_node_t *n, uint32_t pos, void *buf,
                uint32_t len)
{
    uint8_t *dst = buf;
    uint32_t mask = vol->sector_size - 1;
    int      rc;

    while (len > 0) {
        uint32_t lsn;
        uint32_t run;
        uint32_t off = pos & mask;

        rc = node_map(vol, n, pos >> vol->sector_shift, &lsn, &run);
        if (rc != FS_OK)
            return (rc);

        if ((off == 0) && (len >= vol->sector_size)) {
            uint32_t count = len >> vol->sector_shift;
            if (count > run)
                count = run;
            rc = cache_read_direct(&vol->cache, lsn, count, dst);
            if (rc != FS_OK)
                return (rc);
            count <<= vol->sector_shift;
            dst += count;
            pos += count;
            len -= count;
        } else {
            cache_buf_t *b;
            uint32_t     part = vol->sector_size - off;
            if (part > len)
                part = len;
            rc = cache_get(&vol->cache, lsn, 0, &b);
            if (rc != FS_OK)
                return (rc);
            memcpy(dst, b->data + off, part);
            cache_put(&vol->cache, b, 0);
            dst += part;
            pos += part;
            len -= part;
        }
    }
    return (FS_OK);
}

/*
 * Writes bytes [pos, pos + len) within the file's allocated space.
 * Sectors which start at or beyond valid_end hold no file data yet, so
 * they are not read first; their unwritten bytes become zero.
 */
int
node_write_bytes(rbf_vol_t *vol, rbf_node_t *n, uint32_t pos,
                 const void *buf, uint32_t len, uint32_t valid_end)
{
    const uint8_t *src = buf;
    uint32_t mask = vol->sector_size - 1;
    int      rc;

    while (len > 0) {
        uint32_t lsn;
        uint32_t run;
        uint32_t off = pos & mask;

        rc = node_map(vol, n, pos >> vol->sector_shift, &lsn, &run);
        if (rc != FS_OK)
            return (rc);

        if ((off == 0) && (len >= vol->sector_size)) {
            uint32_t count = len >> vol->sector_shift;
            if (count > run)
                count = run;
            rc = cache_write_direct(&vol->cache, lsn, count, src);
            if (rc != FS_OK)
                return (rc);
            count <<= vol->sector_shift;
            src += count;
            pos += count;
            len -= count;
        } else {
            cache_buf_t *b;
            uint32_t     part = vol->sector_size - off;
            int          fresh = ((pos - off) >= valid_end);

            if (part > len)
                part = len;
            rc = cache_get(&vol->cache, lsn, fresh ? CACHE_NOREAD : 0, &b);
            if (rc != FS_OK)
                return (rc);
            if (fresh)
                memset(b->data, 0, vol->sector_size);
            memcpy(b->data + off, src, part);
            cache_put(&vol->cache, b, 1);
            src += part;
            pos += part;
            len -= part;
        }
    }
    return (FS_OK);
}

/* Writes zeros over [from, to) of the file */
static int
node_zero_range(rbf_vol_t *vol, rbf_node_t *n, uint32_t from, uint32_t to,
                uint32_t valid_end)
{
    int rc;

    while (from < to) {
        uint32_t part = vol->sector_size - (from & (vol->sector_size - 1));
        if (part > to - from)
            part = to - from;
        rc = node_write_bytes(vol, n, from, vol->zero, part, valid_end);
        if (rc != FS_OK)
            return (rc);
        from += part;
    }
    return (FS_OK);
}

static uint32_t
bytes_to_sectors(rbf_vol_t *vol, uint32_t bytes)
{
    return ((bytes >> vol->sector_shift) +
            ((bytes & (vol->sector_size - 1)) != 0));
}

int
rbf_open(fs_volume_t *v, fs_node_t node, int mode, fs_file_t **fp)
{
    rbf_vol_t  *vol = RVOL(v);
    rbf_file_t *f;
    rbf_node_t *n;
    int         rc;

    *fp = NULL;
    if ((mode & FS_OPEN_WRITE) && vol->read_only)
        return (FS_EROFS);
    rc = node_get(vol, node, &n);
    if (rc != FS_OK)
        return (rc);
    if ((mode & FS_OPEN_WRITE) && (n->fd[FD_ATT] & FS_ATTR_DIR)) {
        node_put(vol, n);
        return (FS_EISDIR);
    }
    f = fsp_alloc(sizeof (*f));
    if (f == NULL) {
        node_put(vol, n);
        return (FS_ENOMEM);
    }
    f->base.vol  = v;
    f->base.node = node;
    f->base.pos  = 0;
    f->base.mode = mode;
    f->node      = n;
    *fp = &f->base;
    return (FS_OK);
}

int
rbf_close(fs_file_t *fp)
{
    rbf_file_t *f   = (rbf_file_t *) fp;
    rbf_vol_t  *vol = RVOL(fp->vol);
    rbf_node_t *n   = f->node;
    int         rc  = FS_OK;
    int         rc2;

    if (f->written && !n->deleted) {
        /* Stamp the modification time and give back preallocation */
        rbf_time_to_date(fsp_now(), n->fd + FD_DAT, 5);
        n->dirty = 1;
        rc = node_trim(vol, n, bytes_to_sectors(vol, node_size(n)));
    }
    rc2 = node_put(vol, n);
    if (rc == FS_OK)
        rc = rc2;
    fsp_free(f, sizeof (*f));
    return (rc);
}

int
rbf_read(fs_file_t *fp, void *buf, uint32_t len, uint32_t *done)
{
    rbf_file_t *f   = (rbf_file_t *) fp;
    rbf_vol_t  *vol = RVOL(fp->vol);
    uint32_t    size = node_size(f->node);
    int         rc;

    *done = 0;
    if (fp->pos >= size)
        return (FS_OK);
    if (len > size - fp->pos)
        len = size - fp->pos;
    rc = node_read_bytes(vol, f->node, fp->pos, buf, len);
    if (rc != FS_OK)
        return (rc);
    fp->pos += len;
    *done = len;
    return (FS_OK);
}

/*
 * Makes room for the file to hold end bytes.  If the disk fills up, the
 * file keeps whatever could be allocated.
 */
static int
file_reserve(rbf_vol_t *vol, rbf_node_t *n, uint32_t end)
{
    int is_dir = (n->fd[FD_ATT] & FS_ATTR_DIR) != 0;

    return (node_grow(vol, n, bytes_to_sectors(vol, end), is_dir));
}

int
rbf_write(fs_file_t *fp, const void *buf, uint32_t len, uint32_t *done)
{
    rbf_file_t *f   = (rbf_file_t *) fp;
    rbf_vol_t  *vol = RVOL(fp->vol);
    rbf_node_t *n   = f->node;
    uint32_t    size = node_size(n);
    uint32_t    pos  = fp->pos;
    uint32_t    room;
    int         rc;
    int         rc_grow;

    *done = 0;
    if ((fp->mode & FS_OPEN_WRITE) == 0)
        return (FS_EINVAL);
    if (vol->read_only)
        return (FS_EROFS);
    if (len == 0)
        return (FS_OK);
    if (pos + len < pos)
        return (FS_ERANGE);

    rc_grow = file_reserve(vol, n, pos + len);
    room = node_alloc_sectors(vol, n) << vol->sector_shift;
    if (rc_grow != FS_OK) {
        /* Write what fits */
        if (room <= pos)
            return (rc_grow);
        len = room - pos;
    }

    f->written = 1;
    if (pos > size) {
        /* Fill the gap left by seeking past the end */
        rc = node_zero_range(vol, n, size, pos, size);
        if (rc != FS_OK)
            return (rc);
        node_set_size(n, pos);
        size = pos;
    }
    rc = node_write_bytes(vol, n, pos, buf, len, size);
    if (rc != FS_OK)
        return (rc);
    if (pos + len > size)
        node_set_size(n, pos + len);
    fp->pos = pos + len;
    *done = len;
    return (rc_grow);
}

int
rbf_truncate(fs_file_t *fp, uint32_t size)
{
    rbf_file_t *f   = (rbf_file_t *) fp;
    rbf_vol_t  *vol = RVOL(fp->vol);
    rbf_node_t *n   = f->node;
    uint32_t    old = node_size(n);
    int         rc;

    if ((fp->mode & FS_OPEN_WRITE) == 0)
        return (FS_EINVAL);
    if (vol->read_only)
        return (FS_EROFS);
    f->written = 1;
    if (size < old) {
        node_set_size(n, size);
        return (node_trim(vol, n, bytes_to_sectors(vol, size)));
    }
    if (size > old) {
        rc = file_reserve(vol, n, size);
        if (rc != FS_OK)
            return (rc);
        rc = node_zero_range(vol, n, old, size, old);
        if (rc != FS_OK)
            return (rc);
        node_set_size(n, size);
    }
    return (FS_OK);
}

uint32_t
rbf_file_size(fs_file_t *fp)
{
    return (node_size(((rbf_file_t *) fp)->node));
}
