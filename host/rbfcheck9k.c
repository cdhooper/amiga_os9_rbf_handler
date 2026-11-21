/*
 * rbfcheck for OS-9000 RBF images (big- or little-endian)
 *
 * Like the OS-9/68k checker, this does not use the filesystem core.
 *
 * Checks:
 *  - ID block fields, bitmap file
 *  - every FD block has the sync code, and correct parity if its revision
 *    field is 1 (OS-9000 RBF checks parity only then; os9format leaves
 *    the revision 0 and the parity meaningless); the FD blocks
 *    of a file form a ring with consistent links
 *  - every directory entry refers to a valid FD; "." and ".." are right;
 *    names are valid; directory sizes are multiples of 64
 *  - segments lie on the media; no block belongs to two objects
 *  - every block in use is set in the bitmap, and every set bit belongs
 *    to some object (or lies past the end of the media)
 *  - file sizes fit in the allocation, small files fit in their FD
 *  - link counts: files have one per entry; directories one per entry,
 *    one for ".", and one per subdirectory (the root one more)
 */

#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define OWNER_FREE      0
#define OWNER_SYSTEM    0xffffffffU

#define FDF_DIR         0x8000
#define FDF_SMALL       0x10000

static int       img;
static uint64_t  base;
static int       le;
static uint32_t  bs, total, root;
static uint32_t *owner;             /* Per block: FD of owner */
static uint16_t *refs;              /* Per block: directory references */
static uint16_t *subdirs;           /* Per directory FD: subdirectories */
static uint8_t  *seen;              /* Per block: FD queued */
static uint8_t  *isdir;             /* Per block: FD is a directory */
static int       errors;
static int       quiet;
static uint32_t  objects;

#define ERR(...) do { errors++; if (!quiet) printf(__VA_ARGS__); } while (0)

static uint32_t
g16(const uint8_t *p)
{
    return (le ? (p[0] | (p[1] << 8)) : ((p[0] << 8) | p[1]));
}

static uint32_t
g32(const uint8_t *p)
{
    return (le ? (p[0] | (p[1] << 8) | ((uint32_t) p[2] << 16) |
                  ((uint32_t) p[3] << 24)) :
                 (((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
                  (p[2] << 8) | p[3]));
}

static int
rd(uint32_t blk, uint8_t *buf)
{
    off_t off = base + (off_t) blk * bs;
    return (pread(img, buf, bs, off) == (ssize_t) bs) ? 0 : -1;
}

static int
is_fd(const uint8_t *b)
{
    return ((b[0] == 0xfd) && (b[1] == 0xb0) && (b[2] == 0xb0) &&
            (b[3] == 0xfd));
}

/* The XOR of all words of an FD block is all ones */
static int
parity_ok(const uint8_t *b)
{
    uint8_t  x[4] = { 0, 0, 0, 0 };
    uint32_t i;

    for (i = 0; i < bs; i++)
        x[i & 3] ^= b[i];
    return ((x[0] == 0xff) && (x[1] == 0xff) && (x[2] == 0xff) &&
            (x[3] == 0xff));
}

static void
claim(uint32_t start, uint32_t count, uint32_t who, const char *what)
{
    uint32_t i;

    if ((start >= total) || (count > total - start)) {
        ERR("%s: blocks %u+%u off the media\n", what, start, count);
        return;
    }
    for (i = start; i < start + count; i++) {
        if (owner[i] != OWNER_FREE)
            ERR("%s: block %u already belongs to %u\n", what, i, owner[i]);
        else
            owner[i] = who;
    }
}

/* Reads an FD block and checks its sync code, and parity if revision 1 */
static int
read_fd(uint32_t lsn, uint8_t *buf, const char *path)
{
    if ((lsn == 0) || (lsn >= total)) {
        ERR("%s: FD %u off the media\n", path, lsn);
        return (-1);
    }
    if (rd(lsn, buf) != 0) {
        ERR("%s: cannot read FD %u\n", path, lsn);
        return (-1);
    }
    if (!is_fd(buf)) {
        ERR("%s: block %u is not an FD\n", path, lsn);
        return (-1);
    }
    if ((g16(buf + 44) == 1) && !parity_ok(buf))
        ERR("%s: FD block %u parity wrong\n", path, lsn);
    return (0);
}

typedef struct {
    uint32_t start;
    uint32_t count;
} seg_t;

/*
 * Collects the segments of a file from its FD ring and claims the FD
 * blocks and the segments.  Returns the segment count, or -1.
 */
static int
file_segs(uint32_t lsn, const uint8_t *fd, const char *path, seg_t **segp)
{
    uint8_t  *buf = malloc(bs);
    seg_t    *segs = NULL;
    int       nsegs = 0;
    uint32_t  slots = (bs - 56) / 8;
    uint32_t  cur = lsn;
    uint32_t  prev = g32(fd + bs - 8);
    uint32_t  nblk = 0;

    memcpy(buf, fd, bs);
    for (;;) {
        uint32_t i;
        uint32_t next;

        if (cur != lsn)
            claim(cur, 1, lsn, path);
        for (i = 0; i < slots; i++) {
            const uint8_t *p = buf + 48 + i * 8;
            uint32_t count = g32(p + 4);
            if (count == 0)
                break;
            segs = realloc(segs, (nsegs + 1) * sizeof (*segs));
            segs[nsegs].start = g32(p);
            segs[nsegs].count = count;
            claim(g32(p), count, lsn, path);
            nsegs++;
        }
        next = g32(buf + bs - 4);
        if (next == lsn) {
            if (g32(fd + bs - 8) != cur)
                ERR("%s: first FD's previous link %u, last FD is %u\n",
                    path, g32(fd + bs - 8), cur);
            break;
        }
        if (++nblk > total) {
            ERR("%s: FD ring does not close\n", path);
            break;
        }
        prev = cur;
        cur = next;
        if (read_fd(cur, buf, path) != 0)
            break;
        if (g32(buf + bs - 8) != prev)
            ERR("%s: FD %u previous link %u, expected %u\n", path, cur,
                g32(buf + bs - 8), prev);
    }
    free(buf);
    *segp = segs;
    return (nsegs);
}

static int
valid_name(const uint8_t *n)
{
    int i;
    int alnum = 0;

    for (i = 0; (i < 44) && (n[i] != 0); i++) {
        int c = n[i];
        if (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
            ((c >= '0') && (c <= '9')))
            alnum = 1;
        else if ((c != '_') && (c != '.') && (c != '$'))
            return (0);
    }
    return ((i > 0) && (i <= 43) && alnum);
}

typedef struct {
    uint32_t fd;
    uint32_t parent;
    char     path[512];
} work_t;

static work_t  *queue;
static uint32_t qhead, qtail, qcap;

static void
enqueue(uint32_t fd, uint32_t parent, const char *path)
{
    if (qtail == qcap) {
        qcap = qcap ? qcap * 2 : 256;
        queue = realloc(queue, qcap * sizeof (*queue));
    }
    queue[qtail].fd = fd;
    queue[qtail].parent = parent;
    snprintf(queue[qtail].path, sizeof (queue[qtail].path), "%s", path);
    qtail++;
}

static void
check_fd(uint32_t lsn, uint32_t parent, const char *path)
{
    uint8_t *fd = malloc(bs);
    uint8_t *data = NULL;
    seg_t   *segs = NULL;
    uint32_t flag, size, alloc = 0, pos;
    int      nsegs, i;

    objects++;
    claim(lsn, 1, lsn, path);
    if (read_fd(lsn, fd, path) != 0)
        goto done;
    flag = g32(fd + 8);
    size = g32(fd + 20);
    if (flag & FDF_SMALL) {
        if (size > bs - 48)
            ERR("%s: small file size %u does not fit\n", path, size);
        if (!(flag & FDF_DIR))
            goto done;
        data = malloc(bs);
        memcpy(data, fd + 48, bs - 48);
    } else {
        nsegs = file_segs(lsn, fd, path, &segs);
        for (i = 0; i < nsegs; i++)
            alloc += segs[i].count;
        if ((uint64_t) alloc * bs < size)
            ERR("%s: size %u but %u blocks allocated\n", path, size, alloc);
        if (!(flag & FDF_DIR))
            goto done;
        data = malloc((size_t) alloc * bs + 1);
        for (pos = 0, i = 0; i < nsegs; i++) {
            uint32_t j;
            for (j = 0; j < segs[i].count; j++, pos++)
                if (rd(segs[i].start + j, data + (size_t) pos * bs) != 0)
                    memset(data + (size_t) pos * bs, 0, bs);
        }
    }

    /* Directory entries */
    if (size % 64)
        ERR("%s: directory size %u not a multiple of 64\n", path, size);
    if ((size < 128) || strcmp((char *) data, "..") ||
        strcmp((char *) data + 64, ".")) {
        ERR("%s: directory does not start with .. and .\n", path);
    } else {
        if (g32(data + 60) != parent)
            ERR("%s: .. is %u, expected %u\n", path, g32(data + 60), parent);
        if (g32(data + 124) != lsn)
            ERR("%s: . is %u, expected %u\n", path, g32(data + 124), lsn);
    }
    for (pos = 128; pos + 64 <= size; pos += 64) {
        const uint8_t *e = data + pos;
        uint32_t       child = g32(e + 60);
        char           cpath[600];
        uint8_t       *cfd;

        if (e[0] == 0)
            continue;
        if (!valid_name(e)) {
            ERR("%s: bad name at offset %u\n", path, pos);
            continue;
        }
        snprintf(cpath, sizeof (cpath), "%s/%.43s", path, (const char *) e);
        if ((child == 0) || (child >= total)) {
            ERR("%s: FD %u off the media\n", cpath, child);
            continue;
        }
        refs[child]++;
        if (seen[child])
            continue;
        seen[child] = 1;
        cfd = malloc(bs);
        if ((rd(child, cfd) == 0) && is_fd(cfd) &&
            (g32(cfd + 8) & FDF_DIR)) {
            isdir[child] = 1;
            subdirs[lsn]++;
        }
        free(cfd);
        enqueue(child, lsn, cpath);
    }
done:
    free(segs);
    free(data);
    free(fd);
}

/* Checks an OS-9000 image whose ID block is at byte offset off */
int
check9k(int fd_img, uint64_t off, int q)
{
    uint8_t  idb[96];
    uint8_t *buf;
    uint8_t *bitmap;
    uint32_t mapfd, maplsn, mapblocks, i;
    uint32_t used = 0, missing = 0, unowned = 0;
    uint32_t bootfiles[2];

    img = fd_img;
    base = off;
    quiet = q;
    if (pread(img, idb, sizeof (idb), off) != sizeof (idb))
        return (2);
    le = (idb[88] == 'L');
    if ((idb[88] != 'L') && (idb[88] != 'B')) {
        uint32_t be = (idb[20] << 8) | idb[21];
        le = !((be >= 256) && (be <= 32768) && !(be & (be - 1)));
        printf("warning: no byte order flag, assuming %s-endian\n",
               le ? "little" : "big");
    }
    bs    = g16(idb + 20);
    total = g32(idb + 8);
    root  = g32(idb + 40);
    mapfd = g32(idb + 28);
    bootfiles[0] = g32(idb + 32);
    bootfiles[1] = g32(idb + 36);
    if ((bs < 256) || (bs > 32768) || (bs & (bs - 1)) || (total < 8) ||
        (root == 0) || (root >= total) || (mapfd == 0) || (mapfd >= total)) {
        printf("not a valid OS-9000 ID block\n");
        return (2);
    }

    owner   = calloc(total, sizeof (*owner));
    refs    = calloc(total, sizeof (*refs));
    subdirs = calloc(total, sizeof (*subdirs));
    seen    = calloc(total, 1);
    isdir   = calloc(total, 1);
    buf     = malloc(bs);

    /* System area: ID block, bitmap file */
    claim(0, 1, OWNER_SYSTEM, "ID block");
    if (read_fd(mapfd, buf, "bitmap") != 0)
        return (2);
    if (g32(buf + 8) & FDF_SMALL) {
        printf("bitmap file is a small file\n");
        return (2);
    }
    maplsn    = g32(buf + 48);
    mapblocks = g32(buf + 52);
    if (((uint64_t) mapblocks * bs * 8 < total) || (maplsn == 0) ||
        (maplsn >= total) || (mapblocks > total - maplsn)) {
        printf("bitmap segment %u+%u does not cover the media\n", maplsn,
               mapblocks);
        return (2);
    }
    if (g32(buf + 56 + 4) != 0)
        ERR("bitmap file has more than one segment\n");
    claim(mapfd, 1, OWNER_SYSTEM, "bitmap FD");
    claim(maplsn, mapblocks, OWNER_SYSTEM, "bitmap");
    bitmap = malloc((size_t) mapblocks * bs);
    for (i = 0; i < mapblocks; i++) {
        if (rd(maplsn + i, bitmap + (size_t) i * bs) != 0) {
            printf("cannot read bitmap block %u\n", maplsn + i);
            return (2);
        }
    }

    /* The directory tree */
    seen[root]  = 1;
    isdir[root] = 1;
    enqueue(root, root, "");
    while (qhead < qtail) {
        work_t w = queue[qhead++];
        check_fd(w.fd, w.parent, (w.path[0] != '\0') ? w.path : "/");
    }

    /* Boot files not reached through a directory */
    for (i = 0; i < 2; i++) {
        if ((bootfiles[i] != 0) && (bootfiles[i] < total) &&
            !seen[bootfiles[i]]) {
            seen[bootfiles[i]] = 1;
            refs[bootfiles[i]] = 1;     /* Not checked */
            check_fd(bootfiles[i], 0, i ? "(bootfile)" : "(firstboot)");
        }
    }

    /* Link counts */
    for (i = 0; i < total; i++) {
        uint32_t want;
        uint32_t links;

        if (!seen[i] || (rd(i, buf) != 0) || !is_fd(buf))
            continue;
        links = g16(buf + 18);
        if (i == root)
            want = 3 + subdirs[i];
        else if (isdir[i])
            want = refs[i] + 1 + subdirs[i];
        else
            want = refs[i];
        if ((i == bootfiles[0]) || (i == bootfiles[1]))
            continue;
        if (links != want)
            ERR("FD %u: link count %u, expected %u\n", i, links, want);
    }

    /* Bitmap against ownership */
    for (i = 0; i < mapblocks * bs * 8; i++) {
        int set = (bitmap[i >> 3] & (0x80 >> (i & 7))) != 0;
        if (i >= total) {
            if (!set)
                ERR("block %u (past end of media) is free\n", i);
            continue;
        }
        if (owner[i] != OWNER_FREE)
            used++;
        if ((owner[i] != OWNER_FREE) && !set) {
            ERR("block %u in use by %u but free in bitmap\n", i, owner[i]);
            missing++;
        } else if ((owner[i] == OWNER_FREE) && set) {
            ERR("block %u allocated but not in file structure\n", i);
            unowned++;
        }
    }

    if (!quiet)
        printf("OS-9000 %s-endian, %u blocks of %u bytes, %u in use, "
               "%u objects\n", le ? "little" : "big", total, bs, used,
               objects);
    if (errors) {
        printf("%d error%s (%u blocks not in bitmap, %u lost)\n", errors,
               (errors == 1) ? "" : "s", missing, unowned);
        return (1);
    }
    if (!quiet)
        printf("no errors\n");
    return (0);
}
