/*
 * Block size mapping (see blockmap.h)
 */

#include <string.h>
#include "blockmap.h"

static int
log2_of(uint32_t v, uint8_t *shift)
{
    uint8_t s = 0;

    if ((v == 0) || ((v & (v - 1)) != 0))
        return (0);
    while ((1UL << s) != v)
        s++;
    *shift = s;
    return (1);
}

int
blockdev_read_bytes(blockdev_t *dev, uint64_t off, uint32_t len, void *buf,
                    uint8_t *tmp)
{
    uint32_t ds = dev->sector_size;
    uint8_t *dst = buf;
    uint8_t  ds_shift;

    if (!log2_of(ds, &ds_shift))
        return (-1);
    while (len > 0) {
        uint32_t blk    = (uint32_t) (off >> ds_shift);
        uint32_t within = (uint32_t) off & (ds - 1);
        uint32_t part   = ds - within;

        if (part > len)
            part = len;
        if (dev->read(dev, blk, 1, tmp) != 0)
            return (-1);
        memcpy(dst, tmp + within, part);
        dst += part;
        off += part;
        len -= part;
    }
    return (0);
}

/*
 * Moves count logical sectors.  When logical sectors are at least as
 * large as device blocks, each is a whole number of blocks; otherwise
 * whole device blocks move directly and partial blocks go through buf.
 */
static int
bm_xfer(blockmap_t *bm, int write, uint32_t lsn, uint32_t count,
        uint8_t *data)
{
    blockdev_t *dev = bm->dev;
    uint32_t    ds  = dev->sector_size;
    uint64_t    off;
    uint64_t    end;

    if ((uint64_t) lsn + count > bm->bd.sector_count)
        return (-1);
    if (bm->shift >= bm->dev_shift) {
        uint8_t r = bm->shift - bm->dev_shift;
        return (write ? dev->write(dev, lsn << r, count << r, data) :
                        dev->read(dev, lsn << r, count << r, data));
    }

    off = (uint64_t) lsn << bm->shift;
    end = off + ((uint64_t) count << bm->shift);
    while (off < end) {
        uint32_t blk    = (uint32_t) (off >> bm->dev_shift);
        uint32_t within = (uint32_t) (off & (ds - 1));

        if ((within == 0) && (end - off >= ds)) {
            uint32_t n = (uint32_t) ((end - off) >> bm->dev_shift);
            int rc = write ? dev->write(dev, blk, n, data) :
                             dev->read(dev, blk, n, data);
            if (rc != 0)
                return (-1);
            data += n << bm->dev_shift;
            off  += (uint64_t) n << bm->dev_shift;
        } else {
            uint32_t part = ds - within;
            if (part > end - off)
                part = (uint32_t) (end - off);
            if (dev->read(dev, blk, 1, bm->buf) != 0)
                return (-1);
            if (write) {
                memcpy(bm->buf + within, data, part);
                if (dev->write(dev, blk, 1, bm->buf) != 0)
                    return (-1);
            } else {
                memcpy(data, bm->buf + within, part);
            }
            data += part;
            off  += part;
        }
    }
    return (0);
}

static int
bm_read(blockdev_t *bd, uint32_t lsn, uint32_t count, void *buf)
{
    return (bm_xfer((blockmap_t *) bd, 0, lsn, count, buf));
}

static int
bm_write(blockdev_t *bd, uint32_t lsn, uint32_t count, const void *buf)
{
    if (bd->read_only)
        return (-1);
    return (bm_xfer((blockmap_t *) bd, 1, lsn, count, (uint8_t *) buf));
}

static int
bm_sync(blockdev_t *bd)
{
    blockdev_t *dev = ((blockmap_t *) bd)->dev;

    return ((dev->sync != NULL) ? dev->sync(dev) : 0);
}

int
blockmap_init(blockmap_t *bm, blockdev_t *dev, uint32_t sector_size)
{
    memset(bm, 0, sizeof (*bm));
    if (!log2_of(sector_size, &bm->shift) ||
        !log2_of(dev->sector_size, &bm->dev_shift))
        return (-1);
    if (bm->shift < bm->dev_shift) {
        bm->buf = fsp_alloc(dev->sector_size);
        if (bm->buf == NULL)
            return (-1);
    }
    bm->dev = dev;
    bm->bd.sector_size  = sector_size;
    bm->bd.sector_count = (uint32_t)
        (((uint64_t) dev->sector_count << bm->dev_shift) >> bm->shift);
    bm->bd.read_only    = dev->read_only;
    bm->bd.read         = bm_read;
    bm->bd.write        = bm_write;
    bm->bd.sync         = bm_sync;
    return (0);
}

void
blockmap_destroy(blockmap_t *bm)
{
    if (bm->buf != NULL)
        fsp_free(bm->buf, bm->dev->sector_size);
    bm->buf = NULL;
}
