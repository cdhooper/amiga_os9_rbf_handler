/*
 * OS-9000 RBF: names and directories
 */

#include <string.h>
#include "rbf9k.h"

/* ------------------------------------------------------------------ */
/* Names                                                              */
/* ------------------------------------------------------------------ */

static int
is_alnum(int c)
{
    return (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
            ((c >= '0') && (c <= '9')));
}

static int
to_upper(int c)
{
    return (((c >= 'a') && (c <= 'z')) ? c - 'a' + 'A' : c);
}

/*
 * A name is valid if F_PRSNAM accepts it: 1 to 43 of the characters
 * A-Z a-z 0-9 _ . $ including at least one letter or digit.
 */
int
r9k_valid_name(fs_volume_t *v, const char *name)
{
    int len;
    int alnum = 0;

    (void) v;
    for (len = 0; name[len] != '\0'; len++) {
        int c = (unsigned char) name[len];
        if (is_alnum(c))
            alnum = 1;
        else if ((c != '_') && (c != '.') && (c != '$'))
            return (FS_EBADNAME);
    }
    if ((len == 0) || (len > R9K_NAME_MAX) || !alnum)
        return (FS_EBADNAME);
    return (FS_OK);
}

/* Case-insensitive comparison of an entry's name with a C string */
static int
name_matches(const uint8_t *ent, const char *name)
{
    int i;

    for (i = 0; i < DIR_NAME_LEN; i++) {
        if (ent[i] == 0)
            return (name[i] == '\0');
        if (to_upper(ent[i]) != to_upper((unsigned char) name[i]))
            return (0);
    }
    return (name[i] == '\0');
}

static int
is_dot_entry(const uint8_t *ent)
{
    return ((ent[0] == '.') &&
            ((ent[1] == 0) || ((ent[1] == '.') && (ent[2] == 0))));
}

static void
make_entry(r9k_vol_t *vol, uint8_t *ent, const char *name, uint32_t lsn)
{
    memset(ent, 0, DIR_ENT_SIZE);
    memcpy(ent, name, strlen(name));
    r9_put32(vol, ent + DIR_FD, lsn);
}

/* ------------------------------------------------------------------ */
/* Directory access                                                   */
/* ------------------------------------------------------------------ */

static int
is_dir(const r9k_vol_t *vol, const r9k_node_t *n)
{
    return ((r9k_node_flag(vol, n) & FDF_DIR) != 0);
}

static int
get_dir(r9k_vol_t *vol, fs_node_t dir, r9k_node_t **np)
{
    int rc;

    if (dir == FS_NODE_NONE)
        dir = vol->root;
    rc = r9k_node_get(vol, dir, np);
    if (rc != FS_OK)
        return (rc);
    if (!is_dir(vol, *np)) {
        r9k_node_put(vol, *np);
        return (FS_ENOTDIR);
    }
    return (FS_OK);
}

/*
 * Gets the 64-byte entry at pos.  Its block stays pinned in the cache
 * until ent_put(); a small directory's entries are in its FD.
 */
typedef struct {
    cache_buf_t *b;
    uint8_t     *ent;
} ent_ref_t;

static int
ent_get(r9k_vol_t *vol, r9k_node_t *dn, uint32_t pos, ent_ref_t *ref)
{
    uint32_t lsn;
    uint32_t run;
    int      rc;

    ref->b = NULL;
    if (r9k_node_is_small(vol, dn)) {
        if (pos + DIR_ENT_SIZE > r9k_small_max(vol))
            return (FS_ECORRUPT);
        ref->ent = dn->fd + FD_HDR + pos;
        return (FS_OK);
    }
    rc = r9k_node_map(vol, dn, pos >> vol->bshift, &lsn, &run);
    if (rc != FS_OK)
        return ((rc == FS_ERANGE) ? FS_ECORRUPT : rc);
    rc = cache_get(&vol->cache, lsn, 0, &ref->b);
    if (rc != FS_OK)
        return (rc);
    ref->ent = ref->b->data + (pos & (vol->bsize - 1));
    return (FS_OK);
}

static void
ent_put(r9k_vol_t *vol, r9k_node_t *dn, ent_ref_t *ref, int dirty)
{
    if (ref->b != NULL)
        cache_put(&vol->cache, ref->b, dirty);
    else if (dirty)
        dn->dirty = 1;
}

/*
 * Finds the entry with the given name (name != NULL) or the given FD
 * (name == NULL).  The "." and ".." entries match only by name.
 */
static int
dir_find(r9k_vol_t *vol, r9k_node_t *dn, const char *name, uint32_t node,
         uint32_t *posp, uint32_t *lsnp, char *namebuf)
{
    uint32_t size = r9k_node_size(vol, dn);
    uint32_t pos;
    int      rc;

    for (pos = 0; pos + DIR_ENT_SIZE <= size; pos += DIR_ENT_SIZE) {
        ent_ref_t r;
        int       found;

        rc = ent_get(vol, dn, pos, &r);
        if (rc != FS_OK)
            return (rc);
        if (r.ent[0] == 0)
            found = 0;
        else if (name != NULL)
            found = name_matches(r.ent, name);
        else
            found = !is_dot_entry(r.ent) &&
                    (r9_get32(vol, r.ent + DIR_FD) == node);
        if (found) {
            *posp = pos;
            if (lsnp != NULL)
                *lsnp = r9_get32(vol, r.ent + DIR_FD);
            if (namebuf != NULL)
                r9k_decode_name(r.ent, DIR_NAME_LEN, namebuf);
        }
        ent_put(vol, dn, &r, 0);
        if (found)
            return (FS_OK);
    }
    return (FS_ENOENT);
}

/* A directory is empty if it has nothing besides "." and ".." */
static int
dir_is_empty(r9k_vol_t *vol, r9k_node_t *dn)
{
    uint32_t size = r9k_node_size(vol, dn);
    uint32_t pos;

    for (pos = 0; pos + DIR_ENT_SIZE <= size; pos += DIR_ENT_SIZE) {
        ent_ref_t r;
        int       used;

        if (ent_get(vol, dn, pos, &r) != FS_OK)
            return (0);
        used = (r.ent[0] != 0) && !is_dot_entry(r.ent);
        ent_put(vol, dn, &r, 0);
        if (used)
            return (0);
    }
    return (1);
}

/* Adds an entry, reusing a free slot or extending the directory */
static int
dir_add(r9k_vol_t *vol, r9k_node_t *dn, const char *name, uint32_t lsn)
{
    uint8_t  ent[DIR_ENT_SIZE];
    uint32_t size = r9k_node_size(vol, dn);
    uint32_t pos;
    int      rc;

    for (pos = 0; pos + DIR_ENT_SIZE <= size; pos += DIR_ENT_SIZE) {
        ent_ref_t r;
        int       free_slot;

        rc = ent_get(vol, dn, pos, &r);
        if (rc != FS_OK)
            return (rc);
        free_slot = (r.ent[0] == 0);
        ent_put(vol, dn, &r, 0);
        if (free_slot)
            break;
    }

    make_entry(vol, ent, name, lsn);
    if (pos + DIR_ENT_SIZE > size) {
        rc = r9k_reserve(vol, dn, pos + DIR_ENT_SIZE);
        if (rc != FS_OK)
            return (rc);
    }
    rc = r9k_write_bytes(vol, dn, pos, ent, DIR_ENT_SIZE, size);
    if (rc != FS_OK)
        return (rc);
    if (pos + DIR_ENT_SIZE > size)
        r9k_node_set_size(vol, dn, pos + DIR_ENT_SIZE);
    return (FS_OK);
}

/*
 * Deletes the entry at pos.  As OS-9000 does, the name moves up one byte
 * behind a zero, which keeps the old name readable.
 */
static int
dir_clear_entry(r9k_vol_t *vol, r9k_node_t *dn, uint32_t pos)
{
    ent_ref_t r;
    int       rc;

    rc = ent_get(vol, dn, pos, &r);
    if (rc != FS_OK)
        return (rc);
    memmove(r.ent + 1, r.ent, DIR_NAME_LEN);
    r.ent[0] = 0;
    ent_put(vol, dn, &r, 1);
    return (FS_OK);
}

/* Rewrites the FD address of the entry at pos */
static int
dir_set_fd(r9k_vol_t *vol, r9k_node_t *dn, uint32_t pos, uint32_t lsn)
{
    ent_ref_t r;
    int       rc;

    rc = ent_get(vol, dn, pos, &r);
    if (rc != FS_OK)
        return (rc);
    r9_put32(vol, r.ent + DIR_FD, lsn);
    ent_put(vol, dn, &r, 1);
    return (FS_OK);
}

static void
touch(r9k_vol_t *vol, r9k_node_t *n)
{
    r9k_node_stamp(vol, n, FD_MTIME);
    r9k_node_stamp(vol, n, FD_UTIME);
}

static void
adjust_links(r9k_vol_t *vol, r9k_node_t *n, int delta)
{
    uint32_t links = r9_get16(vol, n->fd + FD_LINKS);

    if ((delta < 0) && (links == 0))
        return;
    if ((delta > 0) && (links == 0xffff))
        return;
    r9_put16(vol, n->fd + FD_LINKS, links + delta);
    n->dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Attributes                                                         */
/* ------------------------------------------------------------------ */

static uint16_t
flag_to_attr(uint32_t flag)
{
    uint16_t a = 0;

    if (flag & FDF_OWNER_R) a |= FS_ATTR_READ;
    if (flag & FDF_OWNER_W) a |= FS_ATTR_WRITE;
    if (flag & FDF_OWNER_X) a |= FS_ATTR_EXEC;
    if (flag & FDF_WORLD_R) a |= FS_ATTR_PREAD;
    if (flag & FDF_WORLD_W) a |= FS_ATTR_PWRITE;
    if (flag & FDF_WORLD_X) a |= FS_ATTR_PEXEC;
    if (flag & FDF_EXCL)    a |= FS_ATTR_NONSHARE;
    if (flag & FDF_DIR)     a |= FS_ATTR_DIR;
    return (a);
}

/* Public permissions set both the group and the world bits */
static uint32_t
attr_to_flag(uint16_t a)
{
    uint32_t f = 0;

    if (a & FS_ATTR_READ)     f |= FDF_OWNER_R;
    if (a & FS_ATTR_WRITE)    f |= FDF_OWNER_W;
    if (a & FS_ATTR_EXEC)     f |= FDF_OWNER_X;
    if (a & FS_ATTR_PREAD)    f |= FDF_WORLD_R | FDF_GROUP_R;
    if (a & FS_ATTR_PWRITE)   f |= FDF_WORLD_W | FDF_GROUP_W;
    if (a & FS_ATTR_PEXEC)    f |= FDF_WORLD_X | FDF_GROUP_X;
    if (a & FS_ATTR_NONSHARE) f |= FDF_EXCL;
    return (f);
}

static void
fill_stat_from_fd(r9k_vol_t *vol, r9k_node_t *n, fs_stat_t *st)
{
    const uint8_t *fd = n->fd;
    uint32_t       flag = r9k_node_flag(vol, n);

    st->node        = n->lsn;
    st->attr        = flag_to_attr(flag);
    st->type        = (flag & FDF_DIR) ? FS_TYPE_DIR : FS_TYPE_FILE;
    st->owner_group = r9_get16(vol, fd + FD_GROUP);
    st->owner_user  = r9_get16(vol, fd + FD_OWNER);
    st->mtime       = r9k_to_time(vol, r9_get32(vol, fd + FD_MTIME));
    st->ctime       = r9k_to_time(vol, r9_get32(vol, fd + FD_CTIME));
    st->links       = r9_get16(vol, fd + FD_LINKS);
    st->size        = r9k_node_size(vol, n);
    st->blocks      = 1 + n->next_blks + n->alloc;
}

static int
fill_stat(r9k_vol_t *vol, uint32_t lsn, fs_stat_t *st)
{
    r9k_node_t *n;
    int         rc;

    rc = r9k_node_get(vol, lsn, &n);
    if (rc != FS_OK)
        return (rc);
    fill_stat_from_fd(vol, n, st);
    return (r9k_node_put(vol, n));
}

/* Name of the volume, used as the name of the root directory */
static int
volume_name(r9k_vol_t *vol, char *name)
{
    cache_buf_t *b;
    int          rc;

    rc = cache_get(&vol->cache, 0, 0, &b);
    if (rc != FS_OK)
        return (rc);
    r9k_decode_name(b->data + ID_NAME, ID_NAME_LEN - 1, name);
    cache_put(&vol->cache, b, 0);
    return (FS_OK);
}

int
r9k_lookup(fs_volume_t *v, fs_node_t dir, const char *name, fs_stat_t *st)
{
    r9k_vol_t  *vol = R9VOL(v);
    r9k_node_t *dn;
    uint32_t    pos;
    uint32_t    lsn;
    int         rc;

    if ((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0))
        return (FS_ENOENT);  /* OS-9 links; not names on the Amiga side */
    rc = get_dir(vol, dir, &dn);
    if (rc != FS_OK)
        return (rc);
    rc = dir_find(vol, dn, name, 0, &pos, &lsn, st->name);
    if (rc == FS_OK) {
        st->dir    = dn->lsn;
        st->dirpos = pos;
    }
    r9k_node_put(vol, dn);
    if (rc == FS_OK)
        rc = fill_stat(vol, lsn, st);
    return (rc);
}

int
r9k_stat(fs_volume_t *v, fs_node_t dir, fs_node_t node, fs_stat_t *st)
{
    r9k_vol_t  *vol = R9VOL(v);
    r9k_node_t *dn;
    int         rc;

    st->dir     = FS_NODE_NONE;
    st->dirpos  = 0;
    st->name[0] = '\0';
    if (node == vol->root) {
        rc = volume_name(vol, st->name);
    } else if (dir != FS_NODE_NONE) {
        rc = get_dir(vol, dir, &dn);
        if (rc != FS_OK)
            return (rc);
        rc = dir_find(vol, dn, NULL, node, &st->dirpos, NULL, st->name);
        if (rc == FS_OK)
            st->dir = dn->lsn;
        r9k_node_put(vol, dn);
    } else {
        rc = FS_OK;
    }
    if (rc == FS_OK)
        rc = fill_stat(vol, node, st);
    return (rc);
}

int
r9k_parent(fs_volume_t *v, fs_node_t dir, fs_node_t *parent)
{
    r9k_vol_t  *vol = R9VOL(v);
    r9k_node_t *dn;
    uint32_t    pos;
    uint32_t    lsn;
    int         rc;

    *parent = FS_NODE_NONE;
    if ((dir == FS_NODE_NONE) || (dir == vol->root))
        return (FS_OK);
    rc = get_dir(vol, dir, &dn);
    if (rc != FS_OK)
        return (rc);
    rc = dir_find(vol, dn, "..", 0, &pos, &lsn, NULL);
    r9k_node_put(vol, dn);
    if (rc == FS_ENOENT)
        return (FS_ECORRUPT);
    if (rc == FS_OK)
        *parent = lsn;
    return (rc);
}

int
r9k_readdir(fs_volume_t *v, fs_node_t dir, uint32_t *cookie, fs_stat_t *st)
{
    r9k_vol_t  *vol = R9VOL(v);
    r9k_node_t *dn;
    uint32_t    size;
    uint32_t    pos;
    int         rc;

    rc = get_dir(vol, dir, &dn);
    if (rc != FS_OK)
        return (rc);
    size = r9k_node_size(vol, dn);
    rc = FS_ENOENT;
    for (pos = *cookie; pos + DIR_ENT_SIZE <= size; pos += DIR_ENT_SIZE) {
        ent_ref_t r;
        uint32_t  lsn;
        int       skip;

        rc = ent_get(vol, dn, pos, &r);
        if (rc != FS_OK)
            break;
        skip = (r.ent[0] == 0) || is_dot_entry(r.ent);
        lsn  = r9_get32(vol, r.ent + DIR_FD);
        if (!skip)
            r9k_decode_name(r.ent, DIR_NAME_LEN, st->name);
        ent_put(vol, dn, &r, 0);
        rc = FS_ENOENT;
        if (skip)
            continue;

        st->dir    = dn->lsn;
        st->dirpos = pos;
        if (fill_stat(vol, lsn, st) != FS_OK) {
            /* Show a damaged entry so that it can be seen and removed */
            st->node   = lsn;
            st->attr   = 0;
            st->type   = FS_TYPE_FILE;
            st->size   = 0;
            st->blocks = 0;
            st->links  = 0;
            st->mtime  = 0;
            st->ctime  = 0;
            st->owner_group = 0;
            st->owner_user  = 0;
        }
        *cookie = pos + DIR_ENT_SIZE;
        rc = FS_OK;
        break;
    }
    r9k_node_put(vol, dn);
    return (rc);
}

/* ------------------------------------------------------------------ */
/* Create, remove, rename                                             */
/* ------------------------------------------------------------------ */

/*
 * New files start as small files (their first bytes live in the FD).
 * New directories get one block holding ".." and ".".
 */
int
r9k_create(fs_volume_t *v, fs_node_t dir, const char *name, uint16_t attr,
           int make_dir, fs_stat_t *st)
{
    r9k_vol_t   *vol = R9VOL(v);
    r9k_node_t  *dn;
    r9k_node_t  *nn = NULL;
    cache_buf_t *b;
    uint8_t     *fd;
    uint32_t     pos;
    uint32_t     lsn;
    uint32_t     count;
    uint32_t     now;
    int          rc;

    if (vol->read_only)
        return (FS_EROFS);
    rc = r9k_valid_name(v, name);
    if (rc != FS_OK)
        return (rc);
    rc = get_dir(vol, dir, &dn);
    if (rc != FS_OK)
        return (rc);
    rc = dir_find(vol, dn, name, 0, &pos, NULL, NULL);
    if (rc != FS_ENOENT) {
        if (rc == FS_OK)
            rc = FS_EEXIST;
        goto done;
    }

    rc = r9k_alloc(vol, 1, 0, &lsn, &count);
    if (rc != FS_OK)
        goto done;
    rc = cache_get(&vol->cache, lsn, CACHE_NOREAD, &b);
    if (rc != FS_OK) {
        r9k_free(vol, lsn, count);
        goto done;
    }
    fd  = b->data;
    now = r9k_from_time(vol, fsp_now());
    memset(fd, 0, vol->bsize);
    r9_put32(vol, fd + FD_SYNC, R9K_FD_SYNC);
    r9_put32(vol, fd + FD_FLAG, attr_to_flag(attr) |
                                (make_dir ? FDF_DIR : FDF_SMALL));
    r9_put16(vol, fd + FD_LINKS, make_dir ? 2 : 1);
    r9_put32(vol, fd + FD_CTIME, now);
    r9_put32(vol, fd + FD_ATIME, now);
    r9_put32(vol, fd + FD_MTIME, now);
    r9_put32(vol, fd + FD_UTIME, now);
    r9_put16(vol, fd + FD_REV, R9K_FD_REV);
    if (make_dir) {
        r9_put32(vol, fd + vol->bsize - 8, lsn);
        r9_put32(vol, fd + vol->bsize - 4, lsn);
    }
    r9k_set_parity(fd, vol->bsize);
    cache_put(&vol->cache, b, 1);

    rc = r9k_node_get(vol, lsn, &nn);
    if (rc != FS_OK) {
        r9k_free(vol, lsn, count);
        goto done;
    }

    if (make_dir) {
        uint8_t ents[2 * DIR_ENT_SIZE];

        make_entry(vol, ents, "..", dn->lsn);
        make_entry(vol, ents + DIR_ENT_SIZE, ".", lsn);
        rc = r9k_node_grow(vol, nn, 1, 1);
        if (rc == FS_OK)
            rc = r9k_write_bytes(vol, nn, 0, ents, sizeof (ents), 0);
        if (rc != FS_OK)
            goto undo;
        r9k_node_set_size(vol, nn, sizeof (ents));
    }

    rc = dir_add(vol, dn, name, lsn);
    if (rc != FS_OK)
        goto undo;
    if (make_dir)
        adjust_links(vol, dn, 1);       /* The new ".." */
    touch(vol, dn);

    if (st != NULL) {
        fill_stat_from_fd(vol, nn, st);
        strcpy(st->name, name);
        st->dir = dn->lsn;
        dir_find(vol, dn, name, 0, &st->dirpos, NULL, NULL);
    }
    rc = r9k_node_put(vol, nn);
    goto done;

undo:
    r9k_node_release(vol, nn);
    r9k_node_put(vol, nn);
done:
    r9k_node_put(vol, dn);
    return (rc);
}

int
r9k_remove(fs_volume_t *v, fs_node_t dir, const char *name)
{
    r9k_vol_t  *vol = R9VOL(v);
    r9k_node_t *dn;
    r9k_node_t *nn;
    uint32_t    pos;
    uint32_t    lsn;
    int         rc;

    if (vol->read_only)
        return (FS_EROFS);
    if ((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0))
        return (FS_EINVAL);
    rc = get_dir(vol, dir, &dn);
    if (rc != FS_OK)
        return (rc);
    rc = dir_find(vol, dn, name, 0, &pos, &lsn, NULL);
    if (rc != FS_OK)
        goto done;
    if ((lsn == vol->root) || (lsn == vol->map_fd)) {
        rc = FS_EBUSY;
        goto done;
    }

    rc = r9k_node_get(vol, lsn, &nn);
    if (rc == FS_OK) {
        if (nn->refs > 1) {
            rc = FS_EBUSY;              /* Open elsewhere */
        } else if (is_dir(vol, nn)) {
            if (!dir_is_empty(vol, nn)) {
                rc = FS_ENOTEMPTY;
            } else {
                rc = r9k_node_release(vol, nn);
                if (rc == FS_OK)
                    adjust_links(vol, dn, -1);  /* Its ".." is gone */
            }
        } else if (r9_get16(vol, nn->fd + FD_LINKS) > 1) {
            adjust_links(vol, nn, -1);  /* Other links remain */
            r9k_node_stamp(vol, nn, FD_UTIME);
        } else {
            rc = r9k_node_release(vol, nn);
        }
        r9k_node_put(vol, nn);
    } else if (rc == FS_ECORRUPT) {
        /* Entry refers to garbage: remove the entry, free nothing */
        fsp_log("rbf9k: removing entry %s with bad FD %lu\n", name,
                (unsigned long) lsn);
        rc = FS_OK;
    }
    if (rc != FS_OK)
        goto done;

    rc = dir_clear_entry(vol, dn, pos);
    touch(vol, dn);
done:
    r9k_node_put(vol, dn);
    return (rc);
}

/* Fails with FS_ELOOP if dir is node or lies below it */
static int
check_not_below(r9k_vol_t *vol, uint32_t dir, uint32_t node)
{
    unsigned depth;
    int      rc;

    for (depth = 0; depth < 1000; depth++) {
        fs_node_t parent;

        if (dir == node)
            return (FS_ELOOP);
        if (dir == vol->root)
            return (FS_OK);
        rc = r9k_parent(&vol->base, dir, &parent);
        if (rc != FS_OK)
            return (rc);
        if (parent == dir)
            return (FS_ECORRUPT);
        dir = parent;
    }
    return (FS_ECORRUPT);
}

int
r9k_rename(fs_volume_t *v, fs_node_t sdir, const char *sname,
           fs_node_t ddir, const char *dname)
{
    r9k_vol_t  *vol = R9VOL(v);
    r9k_node_t *sdn;
    r9k_node_t *ddn = NULL;
    r9k_node_t *nn;
    uint32_t    spos;
    uint32_t    dpos;
    uint32_t    lsn;
    int         rc;

    if (vol->read_only)
        return (FS_EROFS);
    if (sdir == FS_NODE_NONE)
        sdir = vol->root;
    if (ddir == FS_NODE_NONE)
        ddir = vol->root;
    rc = r9k_valid_name(v, dname);
    if (rc != FS_OK)
        return (rc);
    rc = get_dir(vol, sdir, &sdn);
    if (rc != FS_OK)
        return (rc);
    rc = dir_find(vol, sdn, sname, 0, &spos, &lsn, NULL);
    if (rc != FS_OK)
        goto done;
    rc = get_dir(vol, ddir, &ddn);
    if (rc != FS_OK)
        goto done;

    rc = dir_find(vol, ddn, dname, 0, &dpos, NULL, NULL);
    if (rc == FS_OK) {
        if ((ddn != sdn) || (dpos != spos)) {
            rc = FS_EEXIST;
            goto done;
        }
        /* Same entry: only the spelling changes */
    } else if (rc != FS_ENOENT) {
        goto done;
    }

    if (ddn == sdn) {
        uint8_t ent[DIR_ENT_SIZE];

        make_entry(vol, ent, dname, lsn);
        rc = r9k_write_bytes(vol, sdn, spos, ent, DIR_ENT_SIZE,
                             r9k_node_size(vol, sdn));
        if (rc == FS_OK)
            touch(vol, sdn);
        goto done;
    }

    rc = r9k_node_get(vol, lsn, &nn);
    if (rc != FS_OK)
        goto done;
    if (is_dir(vol, nn))
        rc = check_not_below(vol, ddn->lsn, lsn);
    if (rc == FS_OK)
        rc = dir_add(vol, ddn, dname, lsn);
    if (rc == FS_OK) {
        rc = dir_clear_entry(vol, sdn, spos);
        if ((rc == FS_OK) && is_dir(vol, nn)) {
            /* Point the moved directory's ".." at its new parent */
            uint32_t ppos;
            if (dir_find(vol, nn, "..", 0, &ppos, NULL, NULL) == FS_OK)
                rc = dir_set_fd(vol, nn, ppos, ddn->lsn);
            adjust_links(vol, sdn, -1);
            adjust_links(vol, ddn, 1);
        }
        r9k_node_stamp(vol, nn, FD_UTIME);
        touch(vol, sdn);
        touch(vol, ddn);
    }
    r9k_node_put(vol, nn);
done:
    if (ddn != NULL)
        r9k_node_put(vol, ddn);
    r9k_node_put(vol, sdn);
    return (rc);
}

int
r9k_setattr(fs_volume_t *v, fs_node_t node, uint16_t attr)
{
    r9k_vol_t  *vol = R9VOL(v);
    r9k_node_t *n;
    uint32_t    flag;
    int         rc;

    if (vol->read_only)
        return (FS_EROFS);
    rc = r9k_node_get(vol, node, &n);
    if (rc != FS_OK)
        return (rc);
    flag = r9k_node_flag(vol, n) & ~(FDF_PERMS | FDF_EXCL);
    r9k_node_set_flag(vol, n, flag | attr_to_flag(attr));
    r9k_node_stamp(vol, n, FD_UTIME);
    return (r9k_node_put(vol, n));
}

int
r9k_setowner(fs_volume_t *v, fs_node_t node, uint16_t group, uint16_t user)
{
    r9k_vol_t  *vol = R9VOL(v);
    r9k_node_t *n;
    int         rc;

    if (vol->read_only)
        return (FS_EROFS);
    rc = r9k_node_get(vol, node, &n);
    if (rc != FS_OK)
        return (rc);
    r9_put16(vol, n->fd + FD_GROUP, group);
    r9_put16(vol, n->fd + FD_OWNER, user);
    r9k_node_stamp(vol, n, FD_UTIME);
    return (r9k_node_put(vol, n));
}

int
r9k_setmtime(fs_volume_t *v, fs_node_t node, fs_time_t mtime)
{
    r9k_vol_t  *vol = R9VOL(v);
    r9k_node_t *n;
    int         rc;

    if (vol->read_only)
        return (FS_EROFS);
    rc = r9k_node_get(vol, node, &n);
    if (rc != FS_OK)
        return (rc);
    r9_put32(vol, n->fd + FD_MTIME, r9k_from_time(vol, mtime));
    n->dirty = 1;
    return (r9k_node_put(vol, n));
}
