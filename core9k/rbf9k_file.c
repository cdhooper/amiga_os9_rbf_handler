/*
 * OS-9000 RBF: file data I/O
 *
 * Whole blocks move directly between the caller's buffer and the device
 * (several at a time when a segment is contiguous); partial blocks go
 * through the block cache.  A small file's data is read and written in
 * its FD block.
 */

#include <string.h>
#include "rbf9k.h"

/* Reads bytes [pos, pos + len) of the file's allocated space */
int
r9k_read_bytes(r9k_vol_t *vol, r9k_node_t *n, uint32_t pos, void *buf,
               uint32_t len)
{
    uint8_t *dst = buf;
    uint32_t mask = vol->bsize - 1;
    int      rc;

    if (r9k_node_is_small(vol, n)) {
        if ((pos > r9k_small_max(vol)) || (len > r9k_small_max(vol) - pos))
            return (FS_ERANGE);
        memcpy(dst, n->fd + FD_HDR + pos, len);
        return (FS_OK);
    }
    while (len > 0) {
        uint32_t lsn;
        uint32_t run;
        uint32_t off = pos & mask;

        rc = r9k_node_map(vol, n, pos >> vol->bshift, &lsn, &run);
        if (rc != FS_OK)
            return (rc);

        if ((off == 0) && (len >= vol->bsize)) {
            uint32_t count = len >> vol->bshift;
            if (count > run)
                count = run;
            rc = cache_read_direct(&vol->cache, lsn, count, dst);
            if (rc != FS_OK)
                return (rc);
            count <<= vol->bshift;
            dst += count;
            pos += count;
            len -= count;
        } else {
            cache_buf_t *b;
            uint32_t     part = vol->bsize - off;
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
 * Blocks which start at or beyond valid_end hold no file data yet, so
 * they are not read first; their unwritten bytes become zero.
 */
int
r9k_write_bytes(r9k_vol_t *vol, r9k_node_t *n, uint32_t pos,
                const void *buf, uint32_t len, uint32_t valid_end)
{
    const uint8_t *src = buf;
    uint32_t mask = vol->bsize - 1;
    int      rc;

    if (r9k_node_is_small(vol, n)) {
        if ((pos > r9k_small_max(vol)) || (len > r9k_small_max(vol) - pos))
            return (FS_ERANGE);
        memcpy(n->fd + FD_HDR + pos, src, len);
        n->dirty = 1;
        return (FS_OK);
    }
    while (len > 0) {
        uint32_t lsn;
        uint32_t run;
        uint32_t off = pos & mask;

        rc = r9k_node_map(vol, n, pos >> vol->bshift, &lsn, &run);
        if (rc != FS_OK)
            return (rc);

        if ((off == 0) && (len >= vol->bsize)) {
            uint32_t count = len >> vol->bshift;
            if (count > run)
                count = run;
            rc = cache_write_direct(&vol->cache, lsn, count, src);
            if (rc != FS_OK)
                return (rc);
            count <<= vol->bshift;
            src += count;
            pos += count;
            len -= count;
        } else {
            cache_buf_t *b;
            uint32_t     part = vol->bsize - off;
            int          fresh = ((pos - off) >= valid_end);

            if (part > len)
                part = len;
            rc = cache_get(&vol->cache, lsn, fresh ? CACHE_NOREAD : 0, &b);
            if (rc != FS_OK)
                return (rc);
            if (fresh)
                memset(b->data, 0, vol->bsize);
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
zero_range(r9k_vol_t *vol, r9k_node_t *n, uint32_t from, uint32_t to,
           uint32_t valid_end)
{
    int rc;

    while (from < to) {
        uint32_t part = vol->bsize - (from & (vol->bsize - 1));
        if (part > to - from)
            part = to - from;
        rc = r9k_write_bytes(vol, n, from, vol->zero, part, valid_end);
        if (rc != FS_OK)
            return (rc);
        from += part;
    }
    return (FS_OK);
}

static uint32_t
bytes_to_blocks(r9k_vol_t *vol, uint32_t bytes)
{
    return ((bytes >> vol->bshift) + ((bytes & (vol->bsize - 1)) != 0));
}

/* Bytes the file can hold without allocating */
static uint32_t
capacity(r9k_vol_t *vol, r9k_node_t *n)
{
    if (r9k_node_is_small(vol, n))
        return (r9k_small_max(vol));
    return (n->alloc << vol->bshift);
}

/*
 * Makes room for the file to hold end bytes, turning a small file into
 * an ordinary one when it outgrows its FD block.  If the disk fills up,
 * the file keeps whatever could be allocated.
 */
int
r9k_reserve(r9k_vol_t *vol, r9k_node_t *n, uint32_t end)
{
    int is_dir = (r9k_node_flag(vol, n) & FDF_DIR) != 0;

    if (end <= capacity(vol, n))
        return (FS_OK);
    return (r9k_node_grow(vol, n, bytes_to_blocks(vol, end), is_dir));
}

int
r9k_open(fs_volume_t *v, fs_node_t node, int mode, fs_file_t **fp)
{
    r9k_vol_t  *vol = R9VOL(v);
    r9k_file_t *f;
    r9k_node_t *n;
    int         rc;

    *fp = NULL;
    if ((mode & FS_OPEN_WRITE) && vol->read_only)
        return (FS_EROFS);
    rc = r9k_node_get(vol, node, &n);
    if (rc != FS_OK)
        return (rc);
    if ((mode & FS_OPEN_WRITE) && (r9k_node_flag(vol, n) & FDF_DIR)) {
        r9k_node_put(vol, n);
        return (FS_EISDIR);
    }
    f = fsp_alloc(sizeof (*f));
    if (f == NULL) {
        r9k_node_put(vol, n);
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
r9k_close(fs_file_t *fp)
{
    r9k_file_t *f   = (r9k_file_t *) fp;
    r9k_vol_t  *vol = R9VOL(fp->vol);
    r9k_node_t *n   = f->node;
    int         rc  = FS_OK;
    int         rc2;

    if (f->written && !n->deleted) {
        /* Stamp the modification time and give back preallocation */
        r9k_node_stamp(vol, n, FD_MTIME);
        r9k_node_stamp(vol, n, FD_UTIME);
        rc = r9k_node_trim(vol, n,
                           bytes_to_blocks(vol, r9k_node_size(vol, n)));
    }
    rc2 = r9k_node_put(vol, n);
    if (rc == FS_OK)
        rc = rc2;
    fsp_free(f, sizeof (*f));
    return (rc);
}

int
r9k_read(fs_file_t *fp, void *buf, uint32_t len, uint32_t *done)
{
    r9k_file_t *f   = (r9k_file_t *) fp;
    r9k_vol_t  *vol = R9VOL(fp->vol);
    uint32_t    size = r9k_node_size(vol, f->node);
    int         rc;

    *done = 0;
    if (fp->pos >= size)
        return (FS_OK);
    if (len > size - fp->pos)
        len = size - fp->pos;
    rc = r9k_read_bytes(vol, f->node, fp->pos, buf, len);
    if (rc != FS_OK)
        return (rc);
    fp->pos += len;
    *done = len;
    return (FS_OK);
}

int
r9k_write(fs_file_t *fp, const void *buf, uint32_t len, uint32_t *done)
{
    r9k_file_t *f   = (r9k_file_t *) fp;
    r9k_vol_t  *vol = R9VOL(fp->vol);
    r9k_node_t *n   = f->node;
    uint32_t    size = r9k_node_size(vol, n);
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

    rc_grow = r9k_reserve(vol, n, pos + len);
    room = capacity(vol, n);
    if (rc_grow != FS_OK) {
        /* Write what fits */
        if (room <= pos)
            return (rc_grow);
        len = room - pos;
    }

    f->written = 1;
    if (pos > size) {
        /* Fill the gap left by seeking past the end */
        rc = zero_range(vol, n, size, pos, size);
        if (rc != FS_OK)
            return (rc);
        r9k_node_set_size(vol, n, pos);
        size = pos;
    }
    rc = r9k_write_bytes(vol, n, pos, buf, len, size);
    if (rc != FS_OK)
        return (rc);
    if (pos + len > size)
        r9k_node_set_size(vol, n, pos + len);
    fp->pos = pos + len;
    *done = len;
    return (rc_grow);
}

int
r9k_truncate(fs_file_t *fp, uint32_t size)
{
    r9k_file_t *f   = (r9k_file_t *) fp;
    r9k_vol_t  *vol = R9VOL(fp->vol);
    r9k_node_t *n   = f->node;
    uint32_t    old = r9k_node_size(vol, n);
    int         rc;

    if ((fp->mode & FS_OPEN_WRITE) == 0)
        return (FS_EINVAL);
    if (vol->read_only)
        return (FS_EROFS);
    f->written = 1;
    if (size < old) {
        r9k_node_set_size(vol, n, size);
        if (r9k_node_is_small(vol, n)) {
            memset(n->fd + FD_HDR + size, 0, old - size);
            return (FS_OK);
        }
        return (r9k_node_trim(vol, n, bytes_to_blocks(vol, size)));
    }
    if (size > old) {
        rc = r9k_reserve(vol, n, size);
        if (rc != FS_OK)
            return (rc);
        rc = zero_range(vol, n, old, size, old);
        if (rc != FS_OK)
            return (rc);
        r9k_node_set_size(vol, n, size);
    }
    return (FS_OK);
}

uint32_t
r9k_file_size(fs_file_t *fp)
{
    return (r9k_node_size(R9VOL(fp->vol), ((r9k_file_t *) fp)->node));
}
