/*
 * OS-9000 RBF: file descriptors and segment lists
 *
 * A node keeps the segments of all of a file's FD blocks as one list.
 * The FD blocks hold slots segments each; when the list outgrows them a
 * further FD block is allocated and linked into the file's ring, and
 * trimming gives back FD blocks that are no longer needed.  A small
 * file has no segments: its data follows the header of its one FD
 * block.
 */

#include <string.h>
#include "rbf9k.h"

uint32_t
r9k_node_flag(const r9k_vol_t *vol, const r9k_node_t *n)
{
    return (r9_get32(vol, n->fd + FD_FLAG));
}

void
r9k_node_set_flag(r9k_vol_t *vol, r9k_node_t *n, uint32_t flag)
{
    r9_put32(vol, n->fd + FD_FLAG, flag);
    n->dirty = 1;
}

uint32_t
r9k_node_size(const r9k_vol_t *vol, const r9k_node_t *n)
{
    return (r9_get32(vol, n->fd + FD_SIZE));
}

void
r9k_node_set_size(r9k_vol_t *vol, r9k_node_t *n, uint32_t size)
{
    r9_put32(vol, n->fd + FD_SIZE, size);
    n->dirty = 1;
}

/* Sets a timestamp field of the FD to now */
void
r9k_node_stamp(r9k_vol_t *vol, r9k_node_t *n, int field)
{
    r9_put32(vol, n->fd + field, r9k_from_time(vol, fsp_now()));
    n->dirty = 1;
}

int
r9k_node_is_small(const r9k_vol_t *vol, const r9k_node_t *n)
{
    return ((r9k_node_flag(vol, n) & FDF_SMALL) != 0);
}

uint32_t
r9k_small_max(const r9k_vol_t *vol)
{
    return (vol->bsize - FD_HDR);
}

/* ------------------------------------------------------------------ */
/* Node memory                                                        */
/* ------------------------------------------------------------------ */

static int
grow_array(void **arr, unsigned *cap, unsigned want, uint32_t elem)
{
    unsigned ncap;
    void    *na;

    if (want <= *cap)
        return (FS_OK);
    ncap = (*cap != 0) ? *cap * 2 : 8;
    while (ncap < want)
        ncap *= 2;
    na = fsp_alloc(ncap * elem);
    if (na == NULL)
        return (FS_ENOMEM);
    if (*arr != NULL) {
        memcpy(na, *arr, *cap * elem);
        fsp_free(*arr, *cap * elem);
    }
    *arr = na;
    *cap = ncap;
    return (FS_OK);
}

static int
seg_reserve(r9k_node_t *n, unsigned want)
{
    return (grow_array((void **) &n->segs, &n->segcap, want,
                       sizeof (r9k_seg_t)));
}

static int
ext_reserve(r9k_node_t *n, unsigned want)
{
    return (grow_array((void **) &n->ext, &n->extcap, want,
                       sizeof (uint32_t)));
}

void
r9k_node_free(r9k_vol_t *vol, r9k_node_t *n)
{
    if (n->segs != NULL)
        fsp_free(n->segs, n->segcap * sizeof (r9k_seg_t));
    if (n->ext != NULL)
        fsp_free(n->ext, n->extcap * sizeof (uint32_t));
    if (n->fd != NULL)
        fsp_free(n->fd, vol->bsize);
    fsp_free(n, sizeof (*n));
}

/* ------------------------------------------------------------------ */
/* Reading and writing FD blocks                                      */
/* ------------------------------------------------------------------ */

static int
is_fd_block(const uint8_t *b)
{
    return ((b[0] == 0xfd) && (b[1] == 0xb0) && (b[2] == 0xb0) &&
            (b[3] == 0xfd));
}

/*
 * Appends the segments of one FD block.  A zero count ends the list of
 * that block.  Segments must lie within the media and outside block 0,
 * so that a corrupt FD cannot direct writes or frees anywhere harmful.
 */
static int
parse_segs(r9k_vol_t *vol, r9k_node_t *n, const uint8_t *blk)
{
    unsigned i;
    int      rc;

    for (i = 0; i < vol->slots; i++) {
        const uint8_t *p = blk + FD_HDR + i * FD_SEG_SIZE;
        uint32_t start = r9_get32(vol, p);
        uint32_t count = r9_get32(vol, p + 4);

        if (count == 0)
            break;
        if ((start == 0) || (count > vol->total) ||
            (start > vol->total - count))
            return (FS_ECORRUPT);
        rc = seg_reserve(n, n->nsegs + 1);
        if (rc != FS_OK)
            return (rc);
        n->segs[n->nsegs].start = start;
        n->segs[n->nsegs].count = count;
        n->nsegs++;
        n->alloc += count;
    }
    return (FS_OK);
}

/* Reads the rest of the file's FD ring, following the next links */
static int
read_ring(r9k_vol_t *vol, r9k_node_t *n)
{
    uint32_t bs = vol->bsize;
    uint32_t next = r9_get32(vol, n->fd + bs - 4);
    int      rc;

    while ((next != n->lsn) && (next != 0)) {
        cache_buf_t *b;
        uint32_t     after;

        if ((next >= vol->total) || (n->next_blks >= vol->total))
            return (FS_ECORRUPT);
        rc = cache_get(&vol->cache, next, 0, &b);
        if (rc != FS_OK)
            return (rc);
        if (!is_fd_block(b->data)) {
            cache_put(&vol->cache, b, 0);
            return (FS_ECORRUPT);
        }
        rc = parse_segs(vol, n, b->data);
        after = r9_get32(vol, b->data + bs - 4);
        cache_put(&vol->cache, b, 0);
        if (rc == FS_OK)
            rc = ext_reserve(n, n->next_blks + 1);
        if (rc != FS_OK)
            return (rc);
        n->ext[n->next_blks++] = next;
        next = after;
    }
    return (FS_OK);
}

int
r9k_node_get(r9k_vol_t *vol, uint32_t lsn, r9k_node_t **np)
{
    r9k_node_t  *n;
    cache_buf_t *b;
    int          rc;

    if ((lsn == 0) || (lsn >= vol->total))
        return (FS_ECORRUPT);

    for (n = vol->nodes; n != NULL; n = n->next) {
        if ((n->lsn == lsn) && !n->deleted) {
            n->refs++;
            *np = n;
            return (FS_OK);
        }
    }

    n = fsp_alloc(sizeof (*n));
    if (n == NULL)
        return (FS_ENOMEM);
    n->lsn = lsn;
    n->fd  = fsp_alloc(vol->bsize);
    if (n->fd == NULL) {
        r9k_node_free(vol, n);
        return (FS_ENOMEM);
    }
    rc = cache_get(&vol->cache, lsn, 0, &b);
    if (rc == FS_OK) {
        memcpy(n->fd, b->data, vol->bsize);
        cache_put(&vol->cache, b, 0);
        if (!is_fd_block(n->fd))
            rc = FS_ECORRUPT;
    }
    if (rc == FS_OK) {
        if (r9k_node_is_small(vol, n)) {
            if (r9k_node_size(vol, n) > r9k_small_max(vol))
                rc = FS_ECORRUPT;
        } else {
            rc = parse_segs(vol, n, n->fd);
            if (rc == FS_OK)
                rc = read_ring(vol, n);
        }
    }
    if (rc != FS_OK) {
        r9k_node_free(vol, n);
        return (rc);
    }
    n->refs = 1;
    n->next = vol->nodes;
    vol->nodes = n;
    *np = n;
    return (FS_OK);
}

/*
 * Writes the node to its FD blocks: the header (and small file data)
 * of the first block as kept, the segment list spread over the blocks
 * in ring order, the ring links, the revision and the parity.
 */
int
r9k_node_write(r9k_vol_t *vol, r9k_node_t *n)
{
    uint32_t bs = vol->bsize;
    unsigned nblk = 1 + n->next_blks;
    int      small = r9k_node_is_small(vol, n);
    unsigned i;
    int      rc;

    if (vol->read_only)
        return (FS_EROFS);
    for (i = 0; i < nblk; i++) {
        cache_buf_t *b;
        uint32_t     lsn = (i == 0) ? n->lsn : n->ext[i - 1];
        uint8_t     *buf;

        rc = cache_get(&vol->cache, lsn, CACHE_NOREAD, &b);
        if (rc != FS_OK)
            return (rc);
        if (i == 0) {
            buf = n->fd;
        } else {
            buf = b->data;
            memset(buf, 0, bs);
            r9_put32(vol, buf + FD_SYNC, R9K_FD_SYNC);
        }
        if (!small) {
            unsigned first = i * vol->slots;
            unsigned j;
            uint32_t prev = (i == 0) ? ((nblk > 1) ? n->ext[nblk - 2] :
                                        n->lsn) :
                            ((i == 1) ? n->lsn : n->ext[i - 2]);
            uint32_t next = (i + 1 < nblk) ? n->ext[i] : n->lsn;

            memset(buf + FD_HDR, 0, bs - FD_HDR);
            for (j = 0; (j < vol->slots) && (first + j < n->nsegs); j++) {
                uint8_t *p = buf + FD_HDR + j * FD_SEG_SIZE;
                r9_put32(vol, p, n->segs[first + j].start);
                r9_put32(vol, p + 4, n->segs[first + j].count);
            }
            r9_put32(vol, buf + bs - 8, prev);
            r9_put32(vol, buf + bs - 4, next);
        }
        r9_put16(vol, buf + FD_REV, R9K_FD_REV);
        r9k_set_parity(buf, bs);
        if (i == 0)
            memcpy(b->data, buf, bs);
        cache_put(&vol->cache, b, 1);
    }
    n->dirty = 0;
    return (FS_OK);
}

int
r9k_node_put(r9k_vol_t *vol, r9k_node_t *n)
{
    r9k_node_t **pp;
    int          rc = FS_OK;

    if (n->dirty && !n->deleted)
        rc = r9k_node_write(vol, n);
    if (--n->refs > 0)
        return (rc);

    for (pp = &vol->nodes; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == n) {
            *pp = n->next;
            break;
        }
    }
    r9k_node_free(vol, n);
    return (rc);
}

/* ------------------------------------------------------------------ */
/* Segments                                                           */
/* ------------------------------------------------------------------ */

/*
 * Finds block index of the file.  Returns its LSN and the number of
 * blocks from there to the end of its segment.
 */
int
r9k_node_map(r9k_vol_t *vol, r9k_node_t *n, uint32_t index, uint32_t *lsnp,
             uint32_t *runp)
{
    uint32_t base = 0;
    unsigned i;

    (void) vol;
    for (i = 0; i < n->nsegs; i++) {
        uint32_t count = n->segs[i].count;

        if (index < base + count) {
            *lsnp = n->segs[i].start + (index - base);
            *runp = count - (index - base);
            return (FS_OK);
        }
        base += count;
    }
    return (FS_ERANGE);
}

static int
zero_blocks(r9k_vol_t *vol, uint32_t lsn, uint32_t count)
{
    int rc;

    for (; count > 0; lsn++, count--) {
        rc = cache_write_direct(&vol->cache, lsn, 1, vol->zero);
        if (rc != FS_OK)
            return (rc);
    }
    return (FS_OK);
}

/*
 * Grows the file's allocation to at least the given number of blocks.
 * At least vol->sas blocks are added each time, and new space joins the
 * last segment when it is adjacent.  zero_new clears the new blocks
 * (directories rely on unused entries being zero).
 */
int
r9k_node_grow(r9k_vol_t *vol, r9k_node_t *n, uint32_t blocks, int zero_new)
{
    int rc;

    if (r9k_node_is_small(vol, n)) {
        rc = r9k_node_unsmall(vol, n);
        if (rc != FS_OK)
            return (rc);
    }
    while (n->alloc < blocks) {
        uint32_t   need = blocks - n->alloc;
        uint32_t   want = (need > vol->sas) ? need : vol->sas;
        r9k_seg_t *last = (n->nsegs > 0) ? &n->segs[n->nsegs - 1] : NULL;
        uint32_t   hint = (last != NULL) ? last->start + last->count :
                          n->lsn + 1;
        uint32_t   lsn;
        uint32_t   count;

        /* Memory first, so that a failure leaves the disk alone */
        rc = seg_reserve(n, n->nsegs + 1);
        if (rc == FS_OK)
            rc = ext_reserve(n, n->next_blks + 1);
        if (rc != FS_OK)
            return (rc);

        rc = r9k_alloc(vol, want, hint, &lsn, &count);
        if (rc != FS_OK)
            return (rc);
        if (zero_new && ((rc = zero_blocks(vol, lsn, count)) != FS_OK)) {
            r9k_free(vol, lsn, count);
            return (rc);
        }

        if ((last != NULL) && (last->start + last->count == lsn)) {
            last->count += count;
        } else {
            if (n->nsegs + 1 > (1 + n->next_blks) * vol->slots) {
                /* All FD blocks are full: add one to the ring */
                uint32_t fdb;
                uint32_t one;

                rc = r9k_alloc(vol, 1, 0, &fdb, &one);
                if (rc != FS_OK) {
                    r9k_free(vol, lsn, count);
                    return (rc);
                }
                n->ext[n->next_blks++] = fdb;
            }
            n->segs[n->nsegs].start = lsn;
            n->segs[n->nsegs].count = count;
            n->nsegs++;
        }
        n->alloc += count;
        n->dirty = 1;
    }
    return (FS_OK);
}

/*
 * Releases allocation beyond the given number of blocks, and the FD
 * blocks which the shorter segment list no longer needs.
 */
int
r9k_node_trim(r9k_vol_t *vol, r9k_node_t *n, uint32_t blocks)
{
    uint32_t base = 0;
    unsigned i;
    unsigned j;
    unsigned need_blks;
    int      rc = FS_OK;

    if (r9k_node_is_small(vol, n))
        return (FS_OK);
    for (i = 0; i < n->nsegs; i++) {
        r9k_seg_t *s = &n->segs[i];
        uint32_t   keep;

        if (base + s->count <= blocks) {
            base += s->count;
            continue;
        }
        keep = (blocks > base) ? blocks - base : 0;
        rc = r9k_free(vol, s->start + keep, s->count - keep);
        if (rc != FS_OK)
            break;
        n->alloc -= s->count - keep;
        s->count = keep;
        base += keep;
        n->dirty = 1;
    }

    /* Drop empty segments */
    for (i = 0, j = 0; i < n->nsegs; i++) {
        if (n->segs[i].count == 0)
            continue;
        n->segs[j++] = n->segs[i];
    }
    n->nsegs = j;

    /* Give back FD blocks beyond those the segments need */
    need_blks = (n->nsegs + vol->slots - 1) / vol->slots;
    if (need_blks < 1)
        need_blks = 1;
    while (1 + n->next_blks > need_blks) {
        int rc2 = r9k_free(vol, n->ext[n->next_blks - 1], 1);
        if (rc2 != FS_OK) {
            if (rc == FS_OK)
                rc = rc2;
            break;
        }
        n->next_blks--;
        n->dirty = 1;
    }
    return (rc);
}

/*
 * Frees all space of a file, including its FD blocks.  The node is
 * marked deleted so it is never written back.
 */
int
r9k_node_release(r9k_vol_t *vol, r9k_node_t *n)
{
    int rc;

    rc = r9k_node_trim(vol, n, 0);
    if (rc != FS_OK)
        return (rc);
    rc = r9k_free(vol, n->lsn, 1);
    n->deleted = 1;
    n->dirty   = 0;
    return (rc);
}

/*
 * Turns a small file into an ordinary one: its data moves from the FD
 * block into allocated blocks.  On failure the file stays small.
 */
int
r9k_node_unsmall(r9k_vol_t *vol, r9k_node_t *n)
{
    uint32_t max  = r9k_small_max(vol);
    uint32_t size = r9k_node_size(vol, n);
    uint32_t flag = r9k_node_flag(vol, n);
    uint8_t *data;
    int      rc;

    if ((flag & FDF_SMALL) == 0)
        return (FS_OK);
    data = fsp_alloc(max);
    if (data == NULL)
        return (FS_ENOMEM);
    memcpy(data, n->fd + FD_HDR, max);

    memset(n->fd + FD_HDR, 0, max);
    r9k_node_set_flag(vol, n, flag & ~FDF_SMALL);
    rc = FS_OK;
    if (size > 0) {
        rc = r9k_node_grow(vol, n, (size + vol->bsize - 1) >> vol->bshift,
                           0);
        if (rc == FS_OK)
            rc = r9k_write_bytes(vol, n, 0, data, size, 0);
        if (rc != FS_OK) {
            r9k_node_trim(vol, n, 0);
            memcpy(n->fd + FD_HDR, data, max);
            r9k_node_set_flag(vol, n, flag);
        }
    }
    fsp_free(data, max);
    return (rc);
}
