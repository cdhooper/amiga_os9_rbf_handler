/*
 * OS-9/68k RBF: names and directories
 */

#include <string.h>
#include "rbf68k.h"

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
 * A name is valid if F$PrsNam accepts it: 1 to 28 (OS-9/6809: 29) of
 * the characters A-Z a-z 0-9 _ . $ including at least one letter or
 * digit.
 */
int
rbf_valid_name(fs_volume_t *v, const char *name)
{
    int len;
    int alnum = 0;

    for (len = 0; name[len] != '\0'; len++) {
        int c = (unsigned char) name[len];
        if (is_alnum(c))
            alnum = 1;
        else if ((c != '_') && (c != '.') && (c != '$'))
            return (FS_EBADNAME);
    }
    if ((len == 0) || (len > RVOL(v)->name_len) || !alnum)
        return (FS_EBADNAME);
    return (FS_OK);
}

/* Converts an on-disk name (last character has bit 7 set) to a C string */
void
rbf_decode_name(const uint8_t *src, int maxlen, char *dst)
{
    int i;

    for (i = 0; i < maxlen; i++) {
        uint8_t c = src[i];
        if (c == 0)
            break;
        dst[i] = c & 0x7f;
        if (c & 0x80) {
            i++;
            break;
        }
    }
    dst[i] = '\0';
}

int
rbf_encode_name(const char *src, uint8_t *dst, int maxlen)
{
    int len = strlen(src);

    if ((len == 0) || (len > maxlen))
        return (FS_EBADNAME);
    memset(dst, 0, maxlen);
    memcpy(dst, src, len);
    dst[len - 1] |= 0x80;
    return (FS_OK);
}

/* Case-insensitive comparison of an on-disk name with a C string */
static int
name_matches(const rbf_vol_t *vol, const uint8_t *ent, const char *name)
{
    int i;

    for (i = 0; i < vol->name_len; i++) {
        int c = ent[i] & 0x7f;
        if ((ent[i] == 0) ||
            (to_upper(c) != to_upper((unsigned char) name[i])))
            return (0);
        if (ent[i] & 0x80)
            return (name[i + 1] == '\0');
    }
    return (name[i] == '\0');
}

static int
is_dot_entry(const uint8_t *ent)
{
    return ((ent[0] == ('.' | 0x80)) ||
            ((ent[0] == '.') && (ent[1] == ('.' | 0x80))));
}

/* ------------------------------------------------------------------ */
/* Directory access                                                   */
/* ------------------------------------------------------------------ */

static int
is_dir(const rbf_node_t *n)
{
    return ((n->fd[FD_ATT] & FS_ATTR_DIR) != 0);
}

static int
get_dir(rbf_vol_t *vol, fs_node_t dir, rbf_node_t **np)
{
    int rc;

    if (dir == FS_NODE_NONE)
        dir = vol->root;
    rc = node_get(vol, dir, np);
    if (rc != FS_OK)
        return (rc);
    if (!is_dir(*np)) {
        node_put(vol, *np);
        return (FS_ENOTDIR);
    }
    return (FS_OK);
}

/* Gets the 32-byte entry at pos; the caller releases *bp */
static int
dir_entry(rbf_vol_t *vol, rbf_node_t *dn, uint32_t pos, cache_buf_t **bp,
          uint8_t **entp)
{
    uint32_t lsn;
    uint32_t run;
    int      rc;

    rc = node_map(vol, dn, pos >> vol->sector_shift, &lsn, &run);
    if (rc != FS_OK)
        return ((rc == FS_ERANGE) ? FS_ECORRUPT : rc);
    rc = cache_get(&vol->cache, lsn, 0, bp);
    if (rc != FS_OK)
        return (rc);
    *entp = (*bp)->data + (pos & (vol->sector_size - 1));
    return (FS_OK);
}

/*
 * Finds the entry with the given name (name != NULL) or the given FD
 * (name == NULL).  The "." and ".." entries match only by name.
 */
static int
dir_find(rbf_vol_t *vol, rbf_node_t *dn, const char *name, uint32_t node,
         uint32_t *posp, uint32_t *lsnp, char *namebuf)
{
    uint32_t size = node_size(dn);
    uint32_t pos;
    int      rc;

    for (pos = 0; pos + DIR_ENT_SIZE <= size; pos += DIR_ENT_SIZE) {
        cache_buf_t *b;
        uint8_t     *ent;
        int          found;

        rc = dir_entry(vol, dn, pos, &b, &ent);
        if (rc != FS_OK)
            return (rc);
        if (ent[0] == 0)
            found = 0;
        else if (name != NULL)
            found = name_matches(vol, ent, name);
        else
            found = !is_dot_entry(ent) && (dir_ent_fd(vol, ent) == node);
        if (found) {
            *posp = pos;
            if (lsnp != NULL)
                *lsnp = dir_ent_fd(vol, ent);
            if (namebuf != NULL)
                rbf_decode_name(ent, vol->name_len, namebuf);
        }
        cache_put(&vol->cache, b, 0);
        if (found)
            return (FS_OK);
    }
    return (FS_ENOENT);
}

/* Directory is empty if it has nothing besides "." and ".." */
static int
dir_is_empty(rbf_vol_t *vol, rbf_node_t *dn)
{
    uint32_t size = node_size(dn);
    uint32_t pos;

    for (pos = 0; pos + DIR_ENT_SIZE <= size; pos += DIR_ENT_SIZE) {
        cache_buf_t *b;
        uint8_t     *ent;
        int          used;

        if (dir_entry(vol, dn, pos, &b, &ent) != FS_OK)
            return (0);
        used = (ent[0] != 0) && !is_dot_entry(ent);
        cache_put(&vol->cache, b, 0);
        if (used)
            return (0);
    }
    return (1);
}

static int
dir_write_entry(rbf_vol_t *vol, rbf_node_t *dn, uint32_t pos,
                const uint8_t *ent)
{
    return (node_write_bytes(vol, dn, pos, ent, DIR_ENT_SIZE,
                             node_size(dn)));
}

/* Adds an entry, reusing a free slot or extending the directory */
static int
dir_add(rbf_vol_t *vol, rbf_node_t *dn, const char *name, uint32_t lsn)
{
    uint8_t  ent[DIR_ENT_SIZE];
    uint32_t size = node_size(dn);
    uint32_t pos;
    int      rc;

    for (pos = 0; pos + DIR_ENT_SIZE <= size; pos += DIR_ENT_SIZE) {
        cache_buf_t *b;
        uint8_t     *e;
        int          free_slot;

        rc = dir_entry(vol, dn, pos, &b, &e);
        if (rc != FS_OK)
            return (rc);
        free_slot = (e[0] == 0);
        cache_put(&vol->cache, b, 0);
        if (free_slot)
            break;
    }

    rc = rbf_encode_name(name, ent, vol->name_len);
    if (rc != FS_OK)
        return (rc);
    dir_ent_set_fd(vol, ent, lsn);

    if (pos + DIR_ENT_SIZE > size) {
        uint32_t need = (pos + DIR_ENT_SIZE + vol->sector_size - 1) >>
                        vol->sector_shift;
        rc = node_grow(vol, dn, need, 1);
        if (rc != FS_OK)
            return (rc);
    }
    rc = dir_write_entry(vol, dn, pos, ent);
    if (rc != FS_OK)
        return (rc);
    if (pos + DIR_ENT_SIZE > size)
        node_set_size(dn, pos + DIR_ENT_SIZE);
    return (FS_OK);
}

static int
dir_clear_entry(rbf_vol_t *vol, rbf_node_t *dn, uint32_t pos)
{
    cache_buf_t *b;
    uint8_t     *ent;
    int          rc;

    rc = dir_entry(vol, dn, pos, &b, &ent);
    if (rc != FS_OK)
        return (rc);
    memset(ent, 0, DIR_ENT_SIZE);
    cache_put(&vol->cache, b, 1);
    return (FS_OK);
}

static void
touch(rbf_node_t *n)
{
    rbf_time_to_date(fsp_now(), n->fd + FD_DAT, 5);
    n->dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Attributes                                                         */
/* ------------------------------------------------------------------ */

static void
fill_stat_from_fd(rbf_vol_t *vol, rbf_node_t *n, fs_stat_t *st)
{
    const uint8_t *fd = n->fd;

    st->node        = n->lsn;
    st->attr        = fd[FD_ATT];
    st->type        = (fd[FD_ATT] & FS_ATTR_DIR) ? FS_TYPE_DIR : FS_TYPE_FILE;
    st->owner_group = fd[FD_OWN];
    st->owner_user  = fd[FD_OWN + 1];
    st->mtime       = rbf_date_to_time(fd + FD_DAT, 5);
    st->ctime       = rbf_date_to_time(fd + FD_DCR, 3);
    st->links       = fd[FD_LNK];
    st->size        = node_size(n);
    st->blocks      = 1 + node_alloc_sectors(vol, n);
}

static int
fill_stat(rbf_vol_t *vol, uint32_t lsn, fs_stat_t *st)
{
    rbf_node_t *n;
    int         rc;

    rc = node_get(vol, lsn, &n);
    if (rc != FS_OK)
        return (rc);
    fill_stat_from_fd(vol, n, st);
    return (node_put(vol, n));
}

/* Name of the volume, used as the name of the root directory */
static int
volume_name(rbf_vol_t *vol, char *name)
{
    cache_buf_t *b;
    int          rc;

    rc = cache_get(&vol->cache, 0, 0, &b);
    if (rc != FS_OK)
        return (rc);
    rbf_decode_name(b->data + DD_NAME, DD_NAME_LEN, name);
    cache_put(&vol->cache, b, 0);
    return (FS_OK);
}

int
rbf_lookup(fs_volume_t *v, fs_node_t dir, const char *name, fs_stat_t *st)
{
    rbf_vol_t  *vol = RVOL(v);
    rbf_node_t *dn;
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
    node_put(vol, dn);
    if (rc == FS_OK)
        rc = fill_stat(vol, lsn, st);
    return (rc);
}

int
rbf_stat(fs_volume_t *v, fs_node_t dir, fs_node_t node, fs_stat_t *st)
{
    rbf_vol_t  *vol = RVOL(v);
    rbf_node_t *dn;
    int         rc;

    st->dir    = FS_NODE_NONE;
    st->dirpos = 0;
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
        node_put(vol, dn);
    } else {
        rc = FS_OK;
    }
    if (rc == FS_OK)
        rc = fill_stat(vol, node, st);
    return (rc);
}

int
rbf_parent(fs_volume_t *v, fs_node_t dir, fs_node_t *parent)
{
    rbf_vol_t  *vol = RVOL(v);
    rbf_node_t *dn;
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
    node_put(vol, dn);
    if (rc == FS_ENOENT)
        return (FS_ECORRUPT);
    if (rc == FS_OK)
        *parent = lsn;
    return (rc);
}

int
rbf_readdir(fs_volume_t *v, fs_node_t dir, uint32_t *cookie, fs_stat_t *st)
{
    rbf_vol_t  *vol = RVOL(v);
    rbf_node_t *dn;
    uint32_t    size;
    uint32_t    pos;
    int         rc;

    rc = get_dir(vol, dir, &dn);
    if (rc != FS_OK)
        return (rc);
    size = node_size(dn);
    rc = FS_ENOENT;
    for (pos = *cookie; pos + DIR_ENT_SIZE <= size; pos += DIR_ENT_SIZE) {
        cache_buf_t *b;
        uint8_t     *ent;
        uint32_t     lsn;
        int          skip;

        rc = dir_entry(vol, dn, pos, &b, &ent);
        if (rc != FS_OK)
            break;
        skip = (ent[0] == 0) || is_dot_entry(ent);
        lsn  = dir_ent_fd(vol, ent);
        if (!skip)
            rbf_decode_name(ent, vol->name_len, st->name);
        cache_put(&vol->cache, b, 0);
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
    node_put(vol, dn);
    return (rc);
}

/* ------------------------------------------------------------------ */
/* Create, remove, rename                                             */
/* ------------------------------------------------------------------ */

int
rbf_create(fs_volume_t *v, fs_node_t dir, const char *name, uint16_t attr,
           int make_dir, fs_stat_t *st)
{
    rbf_vol_t   *vol = RVOL(v);
    rbf_node_t  *dn;
    rbf_node_t  *nn = NULL;
    cache_buf_t *b;
    uint8_t     *fd;
    uint32_t     pos;
    uint32_t     lsn;
    uint32_t     count;
    int          rc;

    if (vol->read_only)
        return (FS_EROFS);
    rc = rbf_valid_name(v, name);
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

    /* The FD takes the first sector of a new cluster */
    rc = rbf_alloc(vol, 1, 0, &lsn, &count);
    if (rc != FS_OK)
        goto done;
    rc = cache_get(&vol->cache, lsn, CACHE_NOREAD, &b);
    if (rc != FS_OK) {
        rbf_free(vol, lsn, count);
        goto done;
    }
    fd = b->data;
    memset(fd, 0, vol->sector_size);
    fd[FD_ATT] = (attr & FS_ATTR_PERMS) | (make_dir ? FS_ATTR_DIR : 0);
    rbf_time_to_date(fsp_now(), fd + FD_DAT, 5);
    memcpy(fd + FD_DCR, fd + FD_DAT, 3);
    fd[FD_LNK] = 1;
    if (count > 1) {
        /* The rest of the FD's cluster becomes the first segment */
        put24(fd + FD_SEG, lsn + 1);
        put16(fd + FD_SEG + 3, count - 1);
    }
    cache_put(&vol->cache, b, 1);

    rc = node_get(vol, lsn, &nn);
    if (rc != FS_OK) {
        rbf_free(vol, lsn, count);
        goto done;
    }

    if (make_dir) {
        uint8_t ents[2 * DIR_ENT_SIZE];

        memset(ents, 0, sizeof (ents));
        rbf_encode_name("..", ents, vol->name_len);
        dir_ent_set_fd(vol, ents, dn->lsn);
        rbf_encode_name(".", ents + DIR_ENT_SIZE, vol->name_len);
        dir_ent_set_fd(vol, ents + DIR_ENT_SIZE, lsn);
        rc = node_grow(vol, nn, 1, 1);
        if (rc == FS_OK)
            rc = node_write_bytes(vol, nn, 0, ents, sizeof (ents), 0);
        if (rc != FS_OK)
            goto undo;
        node_set_size(nn, sizeof (ents));
    }

    rc = dir_add(vol, dn, name, lsn);
    if (rc != FS_OK)
        goto undo;
    touch(dn);

    if (st != NULL) {
        fill_stat_from_fd(vol, nn, st);
        strcpy(st->name, name);
        st->dir = dn->lsn;
        dir_find(vol, dn, name, 0, &st->dirpos, NULL, NULL);
    }
    rc = node_put(vol, nn);
    goto done;

undo:
    node_release(vol, nn);
    node_put(vol, nn);
done:
    node_put(vol, dn);
    return (rc);
}

int
rbf_remove(fs_volume_t *v, fs_node_t dir, const char *name)
{
    rbf_vol_t  *vol = RVOL(v);
    rbf_node_t *dn;
    rbf_node_t *nn;
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
    if (lsn == vol->root) {
        rc = FS_EBUSY;
        goto done;
    }

    rc = node_get(vol, lsn, &nn);
    if (rc == FS_OK) {
        if (nn->refs > 1) {
            rc = FS_EBUSY;              /* Open elsewhere */
        } else if (is_dir(nn) && !dir_is_empty(vol, nn)) {
            rc = FS_ENOTEMPTY;
        } else if (nn->fd[FD_LNK] > 1) {
            nn->fd[FD_LNK]--;           /* Other links remain */
            nn->dirty = 1;
        } else {
            rc = node_release(vol, nn);
        }
        node_put(vol, nn);
    } else if (rc == FS_ECORRUPT) {
        /* Entry refers to garbage: remove the entry, free nothing */
        fsp_log("rbf: removing entry %s with bad FD %lu\n", name,
                (unsigned long) lsn);
        rc = FS_OK;
    }
    if (rc != FS_OK)
        goto done;

    rc = dir_clear_entry(vol, dn, pos);
    touch(dn);
done:
    node_put(vol, dn);
    return (rc);
}

/* Fails with FS_ELOOP if dir is node or lies below it */
static int
check_not_below(rbf_vol_t *vol, uint32_t dir, uint32_t node)
{
    unsigned depth;
    int      rc;

    for (depth = 0; depth < 1000; depth++) {
        fs_node_t parent;

        if (dir == node)
            return (FS_ELOOP);
        if (dir == vol->root)
            return (FS_OK);
        rc = rbf_parent(&vol->base, dir, &parent);
        if (rc != FS_OK)
            return (rc);
        if (parent == dir)
            return (FS_ECORRUPT);
        dir = parent;
    }
    return (FS_ECORRUPT);
}

int
rbf_rename(fs_volume_t *v, fs_node_t sdir, const char *sname,
           fs_node_t ddir, const char *dname)
{
    rbf_vol_t  *vol = RVOL(v);
    rbf_node_t *sdn;
    rbf_node_t *ddn = NULL;
    rbf_node_t *nn;
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
    rc = rbf_valid_name(v, dname);
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

        rc = rbf_encode_name(dname, ent, vol->name_len);
        dir_ent_set_fd(vol, ent, lsn);
        if (rc == FS_OK)
            rc = dir_write_entry(vol, sdn, spos, ent);
        if (rc == FS_OK)
            touch(sdn);
        goto done;
    }

    rc = node_get(vol, lsn, &nn);
    if (rc != FS_OK)
        goto done;
    if (is_dir(nn))
        rc = check_not_below(vol, ddn->lsn, lsn);
    if (rc == FS_OK)
        rc = dir_add(vol, ddn, dname, lsn);
    if (rc == FS_OK) {
        rc = dir_clear_entry(vol, sdn, spos);
        if ((rc == FS_OK) && is_dir(nn)) {
            /* Point the moved directory's ".." at its new parent */
            uint32_t ppos;
            if (dir_find(vol, nn, "..", 0, &ppos, NULL, NULL) == FS_OK) {
                uint8_t ent[DIR_ENT_SIZE];
                rbf_encode_name("..", ent, vol->name_len);
                dir_ent_set_fd(vol, ent, ddn->lsn);
                rc = dir_write_entry(vol, nn, ppos, ent);
            }
        }
        touch(sdn);
        touch(ddn);
    }
    node_put(vol, nn);
done:
    if (ddn != NULL)
        node_put(vol, ddn);
    node_put(vol, sdn);
    return (rc);
}

static int
modify_fd(fs_volume_t *v, fs_node_t node, int field, const uint8_t *data,
          int len, uint8_t keep_mask)
{
    rbf_vol_t  *vol = RVOL(v);
    rbf_node_t *n;
    int         i;
    int         rc;

    if (vol->read_only)
        return (FS_EROFS);
    rc = node_get(vol, node, &n);
    if (rc != FS_OK)
        return (rc);
    for (i = 0; i < len; i++)
        n->fd[field + i] = (n->fd[field + i] & keep_mask) |
                           (data[i] & ~keep_mask);
    n->dirty = 1;
    return (node_put(vol, n));
}

int
rbf_setattr(fs_volume_t *v, fs_node_t node, uint16_t attr)
{
    uint8_t a = attr & FS_ATTR_PERMS;

    return (modify_fd(v, node, FD_ATT, &a, 1, FS_ATTR_DIR));
}

int
rbf_setowner(fs_volume_t *v, fs_node_t node, uint16_t group, uint16_t user)
{
    uint8_t own[2];

    own[0] = group;
    own[1] = user;
    return (modify_fd(v, node, FD_OWN, own, 2, 0));
}

int
rbf_setmtime(fs_volume_t *v, fs_node_t node, fs_time_t mtime)
{
    uint8_t date[5];

    rbf_time_to_date(mtime, date, 5);
    return (modify_fd(v, node, FD_DAT, date, 5, 0));
}
