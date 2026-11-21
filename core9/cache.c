/*
 * Sector cache (see cache.h)
 */

#include <string.h>
#include "cache.h"
#include "fs_api.h"

#define HASH(lsn)   ((lsn) % CACHE_HASH_SIZE)

static void
lru_unlink(cache_buf_t *b)
{
    b->lru_prev->lru_next = b->lru_next;
    b->lru_next->lru_prev = b->lru_prev;
}

static void
lru_insert_head(cache_t *c, cache_buf_t *b)
{
    b->lru_next = c->lru.lru_next;
    b->lru_prev = &c->lru;
    c->lru.lru_next->lru_prev = b;
    c->lru.lru_next = b;
}

static void
lru_insert_tail(cache_t *c, cache_buf_t *b)
{
    b->lru_prev = c->lru.lru_prev;
    b->lru_next = &c->lru;
    c->lru.lru_prev->lru_next = b;
    c->lru.lru_prev = b;
}

static void
hash_remove(cache_t *c, cache_buf_t *b)
{
    cache_buf_t **pp = &c->hash[HASH(b->lsn)];

    while (*pp != NULL) {
        if (*pp == b) {
            *pp = b->hash_next;
            break;
        }
        pp = &(*pp)->hash_next;
    }
    b->hash_next = NULL;
}

static cache_buf_t *
hash_find(cache_t *c, uint32_t lsn)
{
    cache_buf_t *b;

    for (b = c->hash[HASH(lsn)]; b != NULL; b = b->hash_next)
        if (b->lsn == lsn)
            return (b);
    return (NULL);
}

static int
buf_write(cache_t *c, cache_buf_t *b)
{
    if (c->dev->write(c->dev, b->lsn, 1, b->data) != 0)
        return (FS_EIO);
    b->dirty = 0;
    c->ndirty--;
    return (FS_OK);
}

int
cache_init(cache_t *c, blockdev_t *dev, unsigned nbufs)
{
    unsigned i;

    if (nbufs < CACHE_MIN_BUFFERS)
        nbufs = CACHE_MIN_BUFFERS;
    memset(c, 0, sizeof (*c));
    c->dev = dev;
    c->sector_size = dev->sector_size;
    c->bufs = fsp_alloc(nbufs * sizeof (cache_buf_t));
    c->mem  = fsp_alloc(nbufs * dev->sector_size);
    if ((c->bufs == NULL) || (c->mem == NULL)) {
        cache_destroy(c);
        return (FS_ENOMEM);
    }
    c->nbufs = nbufs;
    c->lru.lru_next = &c->lru;
    c->lru.lru_prev = &c->lru;
    for (i = 0; i < nbufs; i++) {
        c->bufs[i].data = c->mem + i * dev->sector_size;
        lru_insert_tail(c, &c->bufs[i]);
    }
    return (FS_OK);
}

void
cache_destroy(cache_t *c)
{
    if (c->bufs != NULL)
        fsp_free(c->bufs, c->nbufs * sizeof (cache_buf_t));
    if (c->mem != NULL)
        fsp_free(c->mem, c->nbufs * c->sector_size);
    c->bufs  = NULL;
    c->mem   = NULL;
    c->nbufs = 0;
}

/*
 * Returns a buffer which is not pinned, writing it out first if dirty.
 * The buffer is removed from the hash and marked invalid.
 */
static int
cache_evict(cache_t *c, cache_buf_t **bp)
{
    cache_buf_t *b;
    int          rc;

    for (b = c->lru.lru_prev; b != &c->lru; b = b->lru_prev) {
        if (b->pins == 0)
            break;
    }
    if (b == &c->lru)
        return (FS_ENOMEM);  /* Every buffer is pinned */

    if (b->valid) {
        if (b->dirty && ((rc = buf_write(c, b)) != FS_OK))
            return (rc);
        hash_remove(c, b);
        b->valid = 0;
    }
    *bp = b;
    return (FS_OK);
}

int
cache_get(cache_t *c, uint32_t lsn, int flags, cache_buf_t **bp)
{
    cache_buf_t *b = hash_find(c, lsn);
    int          rc;

    if (b != NULL) {
        c->hits++;
    } else {
        c->misses++;
        rc = cache_evict(c, &b);
        if (rc != FS_OK)
            return (rc);
        if ((flags & CACHE_NOREAD) == 0) {
            if (c->dev->read(c->dev, lsn, 1, b->data) != 0) {
                /* Leave the buffer invalid at the LRU end */
                lru_unlink(b);
                lru_insert_tail(c, b);
                return (FS_EIO);
            }
        }
        b->lsn       = lsn;
        b->valid     = 1;
        b->dirty     = 0;
        b->hash_next = c->hash[HASH(lsn)];
        c->hash[HASH(lsn)] = b;
    }
    b->pins++;
    lru_unlink(b);
    lru_insert_head(c, b);
    *bp = b;
    return (FS_OK);
}

void
cache_put(cache_t *c, cache_buf_t *b, int dirty)
{
    if (dirty && !b->dirty) {
        b->dirty = 1;
        c->ndirty++;
    }
    if (b->pins > 0)
        b->pins--;
}

int
cache_flush(cache_t *c)
{
    int rc = FS_OK;

    /* Write dirty buffers lowest sector first */
    while (c->ndirty > 0) {
        cache_buf_t *low = NULL;
        unsigned     i;
        int          rc2;

        for (i = 0; i < c->nbufs; i++) {
            cache_buf_t *b = &c->bufs[i];
            if (b->valid && b->dirty && ((low == NULL) || (b->lsn < low->lsn)))
                low = b;
        }
        if (low == NULL) {
            c->ndirty = 0;  /* Count was wrong; should not happen */
            break;
        }
        rc2 = buf_write(c, low);
        if (rc2 != FS_OK) {
            /* Drop the sector so the flush can make progress */
            low->dirty = 0;
            c->ndirty--;
            rc = rc2;
        }
    }
    return (rc);
}

void
cache_invalidate(cache_t *c)
{
    unsigned i;

    for (i = 0; i < c->nbufs; i++) {
        c->bufs[i].valid = 0;
        c->bufs[i].dirty = 0;
        c->bufs[i].pins  = 0;
        c->bufs[i].hash_next = NULL;
    }
    memset(c->hash, 0, sizeof (c->hash));
    c->ndirty = 0;
}

int
cache_resize(cache_t *c, unsigned nbufs)
{
    cache_t new_cache;
    int     rc;

    rc = cache_flush(c);
    if (rc != FS_OK)
        return (rc);
    rc = cache_init(&new_cache, c->dev, nbufs);
    if (rc != FS_OK)
        return (rc);
    cache_destroy(c);
    *c = new_cache;
    /* The list head moved with the structure copy; relink its neighbors */
    c->lru.lru_next->lru_prev = &c->lru;
    c->lru.lru_prev->lru_next = &c->lru;
    return (FS_OK);
}

int
cache_read_direct(cache_t *c, uint32_t lsn, uint32_t count, void *buf)
{
    uint32_t i;

    if (c->dev->read(c->dev, lsn, count, buf) != 0)
        return (FS_EIO);

    /* A cached sector may be newer than the device's copy */
    if (c->ndirty > 0) {
        for (i = 0; i < c->nbufs; i++) {
            cache_buf_t *b = &c->bufs[i];
            if (b->valid && b->dirty && (b->lsn >= lsn) &&
                (b->lsn < lsn + count)) {
                memcpy((uint8_t *) buf + (b->lsn - lsn) * c->sector_size,
                       b->data, c->sector_size);
            }
        }
    }
    return (FS_OK);
}

int
cache_write_direct(cache_t *c, uint32_t lsn, uint32_t count,
                   const void *buf)
{
    uint32_t i;

    if (c->dev->write(c->dev, lsn, count, buf) != 0)
        return (FS_EIO);

    /* Refresh cached copies so they match what was written */
    for (i = 0; i < c->nbufs; i++) {
        cache_buf_t *b = &c->bufs[i];
        if (b->valid && (b->lsn >= lsn) && (b->lsn < lsn + count)) {
            memcpy(b->data,
                   (const uint8_t *) buf + (b->lsn - lsn) * c->sector_size,
                   c->sector_size);
            if (b->dirty) {
                b->dirty = 0;
                c->ndirty--;
            }
        }
    }
    return (FS_OK);
}
