/*
 * rbfcheck - check an OS-9/68k or OS-9000 RBF filesystem image
 *
 * An independent implementation of the checks OS-9 dcheck makes; it does
 * not use the filesystem core, so the core's mistakes cannot hide here.
 *
 * usage: rbfcheck [-s secsize] [-o offset] [-q] <image>
 *
 * An image whose first block carries the OS-9000 ID block sync code is
 * checked by check9k() (rbfcheck9k.c); its block size comes from the ID
 * block, so -s does not apply.
 *
 * Checks:
 *  - LSN 0 layout fields
 *  - every directory entry refers to a valid FD; "." and ".." are right
 *  - segments lie in the data area; no cluster belongs to two objects
 *  - every cluster in use is set in the bitmap, and every set bit
 *    belongs to some object (or lies past the end of the media)
 *  - file sizes fit in the allocation; directory sizes are multiples of
 *    32; link counts match the references found
 *
 * Exit status: 0 = clean, 1 = errors found, 2 = could not check.
 */

#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define OWNER_FREE      0
#define OWNER_SYSTEM    1           /* LSN 0 and bitmap */

static int      fd_img;
static uint64_t img_offset;
static uint32_t ss = 512;
static uint32_t tot, cluster, map_bytes, map_sectors, root;
static uint32_t nclusters;          /* Clusters wholly on the media */
static uint32_t bit_clusters;       /* Clusters the bitmap describes */
static uint8_t *bitmap;
static uint32_t *owner;             /* Per cluster: FD LSN of owner */
static uint8_t *refs;               /* Per sector: references to an FD */
static uint8_t *seen;               /* Per sector: FD already checked */
static int      errors;
static int      quiet;

int check9k(int fd_img, uint64_t off, int quiet);

#define ERR(...) do { errors++; if (!quiet) printf(__VA_ARGS__); } while (0)

static uint32_t g16(const uint8_t *p) { return ((p[0] << 8) | p[1]); }
static uint32_t g24(const uint8_t *p)
    { return (((uint32_t) p[0] << 16) | (p[1] << 8) | p[2]); }
static uint32_t g32(const uint8_t *p)
    { return (((uint32_t) p[0] << 24) | g24(p + 1)); }

static int
rd(uint32_t lsn, uint8_t *buf)
{
    off_t off = img_offset + (off_t) lsn * ss;
    return (pread(fd_img, buf, ss, off) == (ssize_t) ss) ? 0 : -1;
}

static int
bit_set(uint32_t cl)
{
    return ((bitmap[cl >> 3] & (0x80 >> (cl & 7))) != 0);
}

static void
claim(uint32_t lsn, uint32_t count, uint32_t who, const char *what)
{
    uint32_t s;

    for (s = lsn; s < lsn + count; s++) {
        uint32_t cl = s / cluster;
        if (owner[cl] == OWNER_FREE) {
            owner[cl] = who;
        } else if (owner[cl] != who) {
            ERR("cluster %u (sector %u) of %s %u also belongs to %u\n",
                cl, s, what, who, owner[cl]);
            return;
        }
    }
}

static void
decode(const uint8_t *p, int max, char *out, int *bad)
{
    int i;

    *bad = 0;
    for (i = 0; i < max; i++) {
        int c = p[i] & 0x7f;
        if (p[i] == 0) {
            *bad = 1;           /* No terminating bit-7 character */
            break;
        }
        out[i] = c;
        if (p[i] & 0x80) {
            i++;
            break;
        }
        if (i == max - 1)
            *bad = 1;
    }
    out[i] = '\0';
}

typedef struct work {
    uint32_t fd;
    uint32_t parent;
    char     path[512];
} work_t;

static work_t  *queue;
static unsigned qhead, qtail, qsize;

static void
enqueue(uint32_t fd, uint32_t parent, const char *path)
{
    if (qtail == qsize) {
        qsize = qsize ? qsize * 2 : 256;
        queue = realloc(queue, qsize * sizeof (*queue));
    }
    queue[qtail].fd = fd;
    queue[qtail].parent = parent;
    snprintf(queue[qtail].path, sizeof (queue[qtail].path), "%s", path);
    qtail++;
}

/* Reads byte range of a file through its segment list */
static int
file_read(const uint8_t *fdbuf, uint32_t pos, uint8_t *out, uint32_t len)
{
    uint8_t  sec[4096];
    uint32_t maxseg = (ss - 16) / 5;

    while (len > 0) {
        uint32_t idx = pos / ss;
        uint32_t base = 0;
        uint32_t i;
        uint32_t lsn = 0;
        uint32_t part;

        for (i = 0; i < maxseg; i++) {
            const uint8_t *e = fdbuf + 16 + i * 5;
            uint32_t n = g16(e + 3);
            if (n == 0)
                return (-1);
            if (idx < base + n) {
                lsn = g24(e) + idx - base;
                break;
            }
            base += n;
        }
        if ((i == maxseg) || (lsn >= tot) || (rd(lsn, sec) != 0))
            return (-1);
        part = ss - pos % ss;
        if (part > len)
            part = len;
        memcpy(out, sec + pos % ss, part);
        out += part;
        pos += part;
        len -= part;
    }
    return (0);
}

static void
check_fd(uint32_t lsn, uint32_t parent, const char *path, int expect_dir)
{
    uint8_t  fdbuf[4096];
    uint32_t maxseg = (ss - 16) / 5;
    uint32_t alloc = 0;
    uint32_t size;
    uint32_t i;
    uint32_t fd_cluster_start = lsn - lsn % cluster;
    int      ended = 0;
    int      isdir;

    if (rd(lsn, fdbuf) != 0) {
        ERR("%s: cannot read FD %u\n", path, lsn);
        return;
    }
    isdir = (fdbuf[0] & 0x80) != 0;
    if (expect_dir && !isdir)
        ERR("%s: root FD %u is not a directory\n", path, lsn);
    claim(lsn, 1, lsn, "FD");

    for (i = 0; i < maxseg; i++) {
        const uint8_t *e = fdbuf + 16 + i * 5;
        uint32_t start = g24(e);
        uint32_t n = g16(e + 3);

        if (n == 0) {
            ended = 1;
            if (start != 0)
                ERR("%s: segment %u has LSN %u but no length\n", path, i,
                    start);
            continue;
        }
        if (ended) {
            ERR("%s: segment %u follows an empty segment\n", path, i);
            continue;
        }
        if ((start < 1 + map_sectors) || (start + n > tot)) {
            ERR("%s: segment %u (%u+%u) outside data area\n", path, i,
                start, n);
            continue;
        }
        if ((start % cluster) != 0) {
            /* Only allowed directly after the FD, in the FD's cluster */
            if ((start != lsn + 1) || (start >= fd_cluster_start + cluster))
                ERR("%s: segment %u (%u+%u) not cluster aligned\n", path, i,
                    start, n);
        }
        claim(start, n, lsn, path);
        alloc += n;
    }

    size = g32(fdbuf + 9);
    if (size > (uint64_t) alloc * ss)
        ERR("%s: size %u exceeds allocation %u sectors\n", path, size, alloc);
    if (fdbuf[8] == 0)
        ERR("%s: link count 0\n", path);

    if (!isdir)
        return;

    if (size % 32)
        ERR("%s: directory size %u not a multiple of 32\n", path, size);
    if (size > (uint64_t) alloc * ss)
        return;
    {
        uint8_t *data = malloc(size + 1);
        uint32_t pos;

        if ((data == NULL) || (file_read(fdbuf, 0, data, size) != 0)) {
            ERR("%s: cannot read directory\n", path);
            free(data);
            return;
        }
        if (size < 64) {
            ERR("%s: directory lacks . and ..\n", path);
        } else {
            if ((data[0] != '.') || (data[1] != ('.' | 0x80)))
                ERR("%s: first entry is not ..\n", path);
            else if (g32(data + 28) != parent)
                ERR("%s: .. is %u, expected %u\n", path, g32(data + 28),
                    parent);
            if (data[32] != ('.' | 0x80))
                ERR("%s: second entry is not .\n", path);
            else if (g32(data + 32 + 28) != lsn)
                ERR("%s: . is %u, expected %u\n", path, g32(data + 60), lsn);
        }
        for (pos = 64; pos + 32 <= size; pos += 32) {
            const uint8_t *e = data + pos;
            char     name[32];
            char     sub[600];
            uint32_t child;
            int      bad;
            int      k;

            if (e[0] == 0)
                continue;
            decode(e, 28, name, &bad);
            if (bad)
                ERR("%s: entry at %u has an unterminated name\n", path, pos);
            for (k = 0; name[k]; k++) {
                char c = name[k];
                if (!(((c >= 'A') && (c <= 'Z')) ||
                      ((c >= 'a') && (c <= 'z')) ||
                      ((c >= '0') && (c <= '9')) ||
                      (c == '_') || (c == '.') || (c == '$')))
                    ERR("%s: name \"%s\" has illegal character\n", path, name);
            }
            child = g32(e + 28);
            snprintf(sub, sizeof (sub), "%s/%s", path, name);
            if ((child < 1 + map_sectors) || (child >= tot)) {
                ERR("%s: FD %u out of range\n", sub, child);
                continue;
            }
            if (refs[child] < 255)
                refs[child]++;
            if (!seen[child]) {
                seen[child] = 1;
                enqueue(child, lsn, sub);
            }
        }
        free(data);
    }
}

static void
usage(void)
{
    fprintf(stderr, "usage: rbfcheck [-s secsize] [-o offset] [-q] <image>\n");
    exit(2);
}

int
main(int argc, char **argv)
{
    uint8_t  lsn0[4096];
    uint32_t i;
    uint32_t lsnsize;
    uint32_t unowned = 0;
    uint32_t missing = 0;
    uint32_t used = 0;
    int      opt;

    while ((opt = getopt(argc, argv, "s:o:q")) != -1) {
        switch (opt) {
            case 's': ss = strtoul(optarg, NULL, 0); break;
            case 'o': img_offset = strtoull(optarg, NULL, 0); break;
            case 'q': quiet = 1; break;
            default:  usage();
        }
    }
    if ((optind != argc - 1) || (ss < 256) || (ss > 4096) || (ss & (ss - 1)))
        usage();
    fd_img = open(argv[optind], O_RDONLY);
    if ((fd_img < 0) || (rd(0, lsn0) != 0)) {
        perror(argv[optind]);
        return (2);
    }
    if ((lsn0[0] == 0xad) && (lsn0[1] == 0xb0) && (lsn0[2] == 0xb0) &&
        (lsn0[3] == 0xad))
        return (check9k(fd_img, img_offset, quiet));

    tot       = g24(lsn0 + 0);
    map_bytes = g16(lsn0 + 4);
    cluster   = g16(lsn0 + 6);
    root      = g24(lsn0 + 8);
    lsnsize   = g16(lsn0 + 0x68);
    if (cluster == 0)
        cluster = 1;
    if (cluster & (cluster - 1)) {
        printf("cluster size %u is not a power of 2\n", cluster);
        return (2);
    }
    if ((lsnsize != 0) && (lsnsize != ss))
        printf("warning: LSN 0 says %u-byte sectors\n", lsnsize);
    map_sectors  = (map_bytes + ss - 1) / ss;
    nclusters    = tot / cluster;
    bit_clusters = map_bytes * 8;
    if ((tot < 4) || (bit_clusters < (tot + cluster - 1) / cluster) ||
        (root < 1 + map_sectors) || (root >= tot)) {
        printf("LSN 0 is not a valid RBF identification sector\n");
        return (2);
    }

    bitmap = calloc(map_sectors, ss);
    owner  = calloc(bit_clusters + 1, sizeof (*owner));
    refs   = calloc(tot, 1);
    seen   = calloc(tot, 1);
    for (i = 0; i < map_sectors; i++) {
        if (rd(1 + i, bitmap + i * ss) != 0) {
            printf("cannot read bitmap sector %u\n", 1 + i);
            return (2);
        }
    }

    claim(0, 1 + map_sectors, OWNER_SYSTEM, "system area");

    seen[root] = 1;
    refs[root] = 1;
    enqueue(root, root, "");
    while (qhead < qtail) {
        work_t w = queue[qhead++];
        check_fd(w.fd, w.parent, (w.path[0] != '\0') ? w.path : "/",
                 w.fd == root);
    }

    /* Link counts */
    for (i = 0; i < tot; i++) {
        uint8_t fdbuf[4096];
        if (!seen[i] || (i == root))
            continue;
        if ((rd(i, fdbuf) == 0) && (fdbuf[8] != refs[i]))
            ERR("FD %u: link count %u but %u references\n", i, fdbuf[8],
                refs[i]);
    }

    /* Bitmap against ownership */
    for (i = 0; i < bit_clusters; i++) {
        int set = bit_set(i);
        if (i >= nclusters) {
            if (!set && (i * cluster < tot))
                ERR("cluster %u (partial, past end of media) is free\n", i);
            continue;
        }
        if (owner[i] != OWNER_FREE)
            used++;
        if ((owner[i] != OWNER_FREE) && !set) {
            ERR("cluster %u in use by %u but free in bitmap\n", i, owner[i]);
            missing++;
        } else if ((owner[i] == OWNER_FREE) && set) {
            ERR("cluster %u allocated but not in file structure\n", i);
            unowned++;
        }
    }

    if (!quiet) {
        printf("%u sectors, %u-sector clusters, %u clusters in use, "
               "%u objects\n", tot, cluster, used, qtail);
    }
    if (errors) {
        printf("%d error%s (%u clusters not in bitmap, %u lost)\n", errors,
               (errors == 1) ? "" : "s", missing, unowned);
        return (1);
    }
    if (!quiet)
        printf("no errors\n");
    return (0);
}
