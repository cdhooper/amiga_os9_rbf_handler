/*
 * Sector cache
 * ------------
 * A write-back cache of whole sectors with LRU replacement.  Callers
 * obtain a sector with cache_get(), which pins it so it cannot be
 * evicted, and release it with cache_put().  Modified sectors are
 * written by cache_flush() in ascending sector order, or when they are
 * evicted.
 *
 * Bulk file data bypasses the cache with cache_read_direct() and
 * cache_write_direct(); these keep any cached copies coherent.
 */

#ifndef _CACHE_H
#define _CACHE_H

#include <stdint.h>
#include "blockdev.h"

#define CACHE_MIN_BUFFERS   8
#define CACHE_HASH_SIZE     64

typedef struct cache_buf cache_buf_t;
struct cache_buf {
    cache_buf_t *lru_next;      /* Toward least recently used */
    cache_buf_t *lru_prev;      /* Toward most recently used */
    cache_buf_t *hash_next;
    uint32_t     lsn;
    uint16_t     pins;          /* Users holding the buffer */
    uint8_t      valid;         /* lsn and data are meaningful */
    uint8_t      dirty;         /* data differs from the device */
    uint8_t     *data;
};

typedef struct cache {
    blockdev_t  *dev;
    uint32_t     sector_size;
    unsigned     nbufs;
    unsigned     ndirty;
    cache_buf_t *bufs;          /* Array of nbufs headers */
    uint8_t     *mem;           /* nbufs * sector_size bytes of data */
    cache_buf_t  lru;           /* List head: lru.lru_next is the MRU */
    cache_buf_t *hash[CACHE_HASH_SIZE];
    uint32_t     hits;
    uint32_t     misses;
} cache_t;

/* cache_get() flags */
#define CACHE_NOREAD    0x01    /* Caller will overwrite the whole sector */

int  cache_init(cache_t *c, blockdev_t *dev, unsigned nbufs);
void cache_destroy(cache_t *c);

int  cache_get(cache_t *c, uint32_t lsn, int flags, cache_buf_t **bp);
void cache_put(cache_t *c, cache_buf_t *b, int dirty);

int  cache_flush(cache_t *c);
void cache_invalidate(cache_t *c);      /* Drops everything, dirty or not */
int  cache_resize(cache_t *c, unsigned nbufs);

int  cache_read_direct(cache_t *c, uint32_t lsn, uint32_t count, void *buf);
int  cache_write_direct(cache_t *c, uint32_t lsn, uint32_t count,
                        const void *buf);

#endif /* _CACHE_H */
