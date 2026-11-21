/*
 * OS-9/68k RBF: file descriptors and segment lists
 *
 * A file's data lives in up to max_segs segments.  Logical sector i of a
 * file is found by walking the segment list.  Allocation is in whole
 * clusters; the only segment which may start inside a cluster is a
 * first segment placed directly after the file's FD, in the FD's own
 * cluster (this is how RBF's Create uses the rest of that cluster).
 */

#include <string.h>
#include "rbf68k.h"

#define SEG(n, i)   ((n)->fd + FD_SEG + (i) * FD_SEG_SIZE)

static uint32_t seg_start(const uint8_t *e) { return (get24(e)); }
static uint32_t seg_len(const uint8_t *e)   { return (get16(e + 3)); }

/* Number of segments in use */
static unsigned
node_nsegs(rbf_vol_t *vol, const rbf_node_t *n)
{
    unsigned i;

    for (i = 0; i < vol->max_segs; i++)
        if (seg_len(SEG(n, i)) == 0)
            break;
    return (i);
}

/*
 * Validates an FD read from disk so that a corrupt FD cannot direct
 * writes or frees outside the data area.
 */
int
node_check(rbf_vol_t *vol, const uint8_t *fd)
{
    uint32_t sys_end = vol->map_lsn + vol->map_sectors;
    unsigned i;

    for (i = 0; i < vol->max_segs; i++) {
        const uint8_t *e = fd + FD_SEG + i * FD_SEG_SIZE;
        uint32_t start = seg_start(e);
        uint32_t len   = seg_len(e);

        if (len == 0)
            break;
        if ((start < sys_end) || (start + len > vol->total))
            return (FS_ECORRUPT);
    }
    return (FS_OK);
}

int
node_get(rbf_vol_t *vol, uint32_t lsn, rbf_node_t **np)
{
    rbf_node_t  *n;
    cache_buf_t *b;
    int          rc;

    if ((lsn < vol->map_lsn + vol->map_sectors) || (lsn >= vol->total))
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
    n->fd = fsp_alloc(vol->sector_size);
    if (n->fd == NULL) {
        fsp_free(n, sizeof (*n));
        return (FS_ENOMEM);
    }
    rc = cache_get(&vol->cache, lsn, 0, &b);
    if (rc == FS_OK) {
        memcpy(n->fd, b->data, vol->sector_size);
        cache_put(&vol->cache, b, 0);
        rc = node_check(vol, n->fd);
    }
    if (rc != FS_OK) {
        fsp_free(n->fd, vol->sector_size);
        fsp_free(n, sizeof (*n));
        return (rc);
    }
    n->lsn  = lsn;
    n->refs = 1;
    n->next = vol->nodes;
    vol->nodes = n;
    *np = n;
    return (FS_OK);
}

int
node_write(rbf_vol_t *vol, rbf_node_t *n)
{
    cache_buf_t *b;
    int          rc;

    if (vol->read_only)
        return (FS_EROFS);
    rc = cache_get(&vol->cache, n->lsn, CACHE_NOREAD, &b);
    if (rc != FS_OK)
        return (rc);
    memcpy(b->data, n->fd, vol->sector_size);
    cache_put(&vol->cache, b, 1);
    n->dirty = 0;
    return (FS_OK);
}

int
node_put(rbf_vol_t *vol, rbf_node_t *n)
{
    rbf_node_t **pp;
    int          rc = FS_OK;

    if (n->dirty && !n->deleted)
        rc = node_write(vol, n);
    if (--n->refs > 0)
        return (rc);

    for (pp = &vol->nodes; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == n) {
            *pp = n->next;
            break;
        }
    }
    fsp_free(n->fd, vol->sector_size);
    fsp_free(n, sizeof (*n));
    return (rc);
}

uint32_t
node_size(const rbf_node_t *n)
{
    return (get32(n->fd + FD_SIZ));
}

void
node_set_size(rbf_node_t *n, uint32_t size)
{
    put32(n->fd + FD_SIZ, size);
    n->dirty = 1;
}

uint32_t
node_alloc_sectors(rbf_vol_t *vol, const rbf_node_t *n)
{
    uint32_t total = 0;
    unsigned i;

    for (i = 0; i < vol->max_segs; i++) {
        uint32_t len = seg_len(SEG(n, i));
        if (len == 0)
            break;
        total += len;
    }
    return (total);
}

/*
 * Finds logical sector index of the file.  Returns its LSN and the number
 * of sectors from there to the end of its segment.
 */
int
node_map(rbf_vol_t *vol, rbf_node_t *n, uint32_t index, uint32_t *lsnp,
         uint32_t *runp)
{
    uint32_t base = 0;
    unsigned i;

    for (i = 0; i < vol->max_segs; i++) {
        uint8_t *e   = SEG(n, i);
        uint32_t len = seg_len(e);

        if (len == 0)
            break;
        if (index < base + len) {
            *lsnp = seg_start(e) + (index - base);
            *runp = len - (index - base);
            return (FS_OK);
        }
        base += len;
    }
    return (FS_ERANGE);
}

static int
zero_sectors(rbf_vol_t *vol, uint32_t lsn, uint32_t count)
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
 * Grows the file's allocation to at least the given number of sectors.
 * At least vol->sas sectors are added each time, and new space joins
 * the last segment when it is adjacent.  zero_new clears the new
 * sectors (directories rely on unused entries being zero).
 */
int
node_grow(rbf_vol_t *vol, rbf_node_t *n, uint32_t sectors, int zero_new)
{
    uint32_t have = node_alloc_sectors(vol, n);
    int      rc;

    while (have < sectors) {
        uint32_t need = sectors - have;
        uint32_t want = (need > vol->sas) ? need : vol->sas;
        unsigned nsegs = node_nsegs(vol, n);
        uint8_t *last = (nsegs > 0) ? SEG(n, nsegs - 1) : NULL;
        uint32_t hint;
        uint32_t lsn;
        uint32_t count;

        hint = (last != NULL) ? seg_start(last) + seg_len(last) : n->lsn + 1;
        rc = rbf_alloc(vol, want, hint, &lsn, &count);
        if (rc != FS_OK)
            return (rc);

        if (zero_new && ((rc = zero_sectors(vol, lsn, count)) != FS_OK)) {
            rbf_free(vol, lsn, count);
            return (rc);
        }

        if ((last != NULL) && (seg_start(last) + seg_len(last) == lsn) &&
            (seg_len(last) + count <= FD_SEG_MAX_LEN)) {
            put16(last + 3, seg_len(last) + count);
        } else if (nsegs < vol->max_segs) {
            uint8_t *e = SEG(n, nsegs);
            put24(e, lsn);
            put16(e + 3, count);
        } else {
            rbf_free(vol, lsn, count);
            return (FS_EFBIG);
        }
        n->dirty = 1;
        have += count;
    }
    return (FS_OK);
}

/*
 * Releases allocation beyond the given number of sectors.  Segments are
 * cut at a cluster boundary, so up to a cluster of slack may remain.
 */
int
node_trim(rbf_vol_t *vol, rbf_node_t *n, uint32_t sectors)
{
    uint32_t base = 0;
    unsigned i;
    unsigned j;
    int      rc = FS_OK;

    for (i = 0; i < vol->max_segs; i++) {
        uint8_t *e     = SEG(n, i);
        uint32_t start = seg_start(e);
        uint32_t len   = seg_len(e);
        uint32_t keep;
        uint32_t newend;

        if (len == 0)
            break;
        if (base + len <= sectors) {
            base += len;
            continue;
        }
        keep   = (sectors > base) ? sectors - base : 0;
        newend = ((start + keep + vol->cluster - 1) >> vol->cluster_shift) <<
                 vol->cluster_shift;
        if (newend > start + len)
            newend = start + len;
        if (newend < start + len) {
            rc = rbf_free(vol, newend, start + len - newend);
            if (rc != FS_OK)
                break;
            put16(e + 3, newend - start);
            n->dirty = 1;
        }
        base += newend - start;
    }

    /* Remove empty segments, keeping the list contiguous */
    for (i = 0, j = 0; i < vol->max_segs; i++) {
        uint8_t *e = SEG(n, i);
        if (seg_len(e) == 0) {
            if (get24(e) != 0) {
                memset(e, 0, FD_SEG_SIZE);
                n->dirty = 1;
            }
            continue;
        }
        if (i != j) {
            memcpy(SEG(n, j), e, FD_SEG_SIZE);
            memset(e, 0, FD_SEG_SIZE);
            n->dirty = 1;
        }
        j++;
    }
    return (rc);
}

/*
 * Frees all space of a file, including its FD's cluster.  The node is
 * marked deleted so it is never written back.
 */
int
node_release(rbf_vol_t *vol, rbf_node_t *n)
{
    uint32_t fd_cluster = (n->lsn >> vol->cluster_shift) <<
                          vol->cluster_shift;
    unsigned i;
    int      rc;

    rc = node_trim(vol, n, 0);
    if (rc != FS_OK)
        return (rc);

    /* What remains lies in the FD's cluster (see node_trim) */
    for (i = 0; i < vol->max_segs; i++) {
        uint8_t *e = SEG(n, i);
        uint32_t len = seg_len(e);
        if (len == 0)
            break;
        if ((seg_start(e) < fd_cluster) ||
            (seg_start(e) + len > fd_cluster + vol->cluster))
            fsp_log("rbf: FD %lu keeps sectors %lu+%lu\n",
                    (unsigned long) n->lsn, (unsigned long) seg_start(e),
                    (unsigned long) len);
    }
    rc = rbf_free(vol, fd_cluster, vol->cluster);
    n->deleted = 1;
    n->dirty   = 0;
    return (rc);
}
