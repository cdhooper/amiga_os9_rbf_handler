/*
 * DOS packet handling
 *
 * Each action function reads its arguments from the current packet and
 * returns dp_Res1; on failure it sets dp_Res2 to the error code.
 *
 * Path syntax: anything up to a ':' names the volume and selects its
 * root; components are separated by '/'; an empty component means the
 * parent directory, and a trailing '/' is ignored.
 */

#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "handler.h"

#define ARG1    (h->pkt->dp_Arg1)
#define ARG2    (h->pkt->dp_Arg2)
#define ARG3    (h->pkt->dp_Arg3)
#define ARG4    (h->pkt->dp_Arg4)

static LONG
fail(handler_t *h, LONG err)
{
    h->pkt->dp_Res2 = err;
    return (DOSFALSE);
}

/* Error for operations attempted while no volume is mounted */
static LONG
no_volume_error(handler_t *h)
{
    if (h->inhibit)
        return (ERROR_DEVICE_NOT_MOUNTED);
    switch (h->media) {
        case MEDIA_NONE:        return (ERROR_NO_DISK);
        case MEDIA_FOREIGN:     return (ERROR_NOT_A_DOS_DISK);
        default:                return (ERROR_NOT_A_DOS_DISK);
    }
}

/* Volume a lock refers to; a NULL lock means the mounted volume's root */
static volrec_t *
lock_volume(handler_t *h, rlock_t *l, LONG *errp)
{
    if (h->cur == NULL) {
        *errp = (l == NULL) ? no_volume_error(h) : ERROR_DEVICE_NOT_MOUNTED;
        return (NULL);
    }
    if ((l != NULL) && (l->vol != h->cur)) {
        *errp = ERROR_DEVICE_NOT_MOUNTED;
        return (NULL);
    }
    return (h->cur);
}

static LONG
check_writable(handler_t *h)
{
    if (h->soft_protect || h->dev.bd.read_only)
        return (ERROR_DISK_WRITE_PROTECTED);
    return (0);
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                    */
/* ------------------------------------------------------------------ */

typedef struct where {
    fs_node_t node;             /* Object reached */
    fs_node_t parent;           /* Directory holding it (none for root) */
} where_t;

static LONG
go_parent(volrec_t *v, where_t *w)
{
    if ((w->node == fs_root(v->fs)) || (w->parent == FS_NODE_NONE))
        return (ERROR_OBJECT_NOT_FOUND);
    w->node = w->parent;
    if (fs_parent(v->fs, w->node, &w->parent) != FS_OK)
        return (ERROR_OBJECT_NOT_FOUND);
    return (0);
}

/*
 * Follows path from the lock.  With last != NULL, the final component is
 * not looked up but copied to last (empty if the path ends in a
 * directory), and w is the directory which would hold it.
 */
static LONG
walk(volrec_t *v, rlock_t *base, const char *path, where_t *w, char *last)
{
    char        comp[FS_NAME_MAX + 1];
    const char *colon = strchr(path, ':');
    fs_stat_t   st;
    int         rc;

    /*
     * DOS passes names with their device part.  For "DEV:x" and "VOL:x"
     * the lock is NULL (the root); for an assign "ASSIGN:x" it is the
     * assigned directory; ":x" always means the root of the volume.
     */
    if ((base != NULL) && (path[0] != ':')) {
        w->node   = base->fl.fl_Key;
        w->parent = base->parent;
    } else {
        w->node   = fs_root(v->fs);
        w->parent = FS_NODE_NONE;
    }
    if (colon != NULL)
        path = colon + 1;
    if (last != NULL)
        last[0] = '\0';

    while (*path != '\0') {
        const char *slash = strchr(path, '/');
        unsigned    len = (slash != NULL) ? (unsigned) (slash - path) :
                                            strlen(path);

        if (len > FS_NAME_MAX)
            return ((last != NULL) && (slash == NULL) ?
                    ERROR_INVALID_COMPONENT_NAME : ERROR_OBJECT_NOT_FOUND);
        memcpy(comp, path, len);
        comp[len] = '\0';

        if ((last != NULL) && (slash == NULL)) {
            strcpy(last, comp);
            return (0);
        }
        if (len == 0) {
            rc = go_parent(v, w);
            if (rc != 0)
                return (rc);
        } else {
            rc = fs_lookup(v->fs, w->node, comp, &st);
            if (rc == FS_ENOTDIR)
                return (ERROR_DIR_NOT_FOUND);
            if (rc != FS_OK)
                return (fs_error_to_dos(rc));
            w->parent = w->node;
            w->node   = st.node;
        }
        if (slash == NULL)
            break;
        path = slash + 1;
    }
    return (0);
}

/* Resolves a lock and name argument pair to an object */
static volrec_t *
resolve(handler_t *h, BPTR lock_bptr, BSTR name_bstr, where_t *w, char *last,
        LONG *errp)
{
    rlock_t  *base = BADDR(lock_bptr);
    char      path[MAX_PATH];
    volrec_t *v;

    v = lock_volume(h, base, errp);
    if (v == NULL)
        return (NULL);
    bstr_to_cstr(name_bstr, path, sizeof (path));
    *errp = walk(v, base, path, w, last);
    return ((*errp == 0) ? v : NULL);
}

static int
stat_where(volrec_t *v, const where_t *w, fs_stat_t *st)
{
    return (fs_stat(v->fs, w->parent, w->node, st));
}

/* ------------------------------------------------------------------ */
/* Locks                                                              */
/* ------------------------------------------------------------------ */

static LONG
action_locate_object(handler_t *h)
{
    where_t   w;
    volrec_t *v;
    rlock_t  *l;
    LONG      err;

    v = resolve(h, ARG1, ARG2, &w, NULL, &err);
    if (v == NULL)
        return (fail(h, err));
    l = lock_create(h, v, w.node, w.parent, ARG3, &err);
    return ((l != NULL) ? (LONG) MKBADDR(l) : fail(h, err));
}

static LONG
action_free_lock(handler_t *h)
{
    rlock_t *l = BADDR(ARG1);

    if (l != NULL)
        lock_free(h, l);
    return (DOSTRUE);
}

static LONG
dup_lock(handler_t *h, rlock_t *l)
{
    volrec_t *v;
    rlock_t  *nl;
    LONG      err;

    v = lock_volume(h, l, &err);
    if (v == NULL)
        return (fail(h, err));
    if (l == NULL)
        nl = lock_create(h, v, fs_root(v->fs), FS_NODE_NONE, SHARED_LOCK,
                         &err);
    else
        nl = lock_create(h, v, l->fl.fl_Key, l->parent, SHARED_LOCK, &err);
    return ((nl != NULL) ? (LONG) MKBADDR(nl) : fail(h, err));
}

static LONG
action_copy_dir(handler_t *h)
{
    return (dup_lock(h, BADDR(ARG1)));
}

static LONG
lock_parent(handler_t *h, rlock_t *l)
{
    volrec_t *v;
    rlock_t  *nl;
    where_t   w;
    LONG      err;

    v = lock_volume(h, l, &err);
    if (v == NULL)
        return (fail(h, err));
    if ((l == NULL) || ((fs_node_t) l->fl.fl_Key == fs_root(v->fs))) {
        h->pkt->dp_Res2 = 0;       /* The root has no parent */
        return (0);
    }
    w.node   = l->fl.fl_Key;
    w.parent = l->parent;
    err = go_parent(v, &w);
    if (err != 0)
        return (fail(h, err));
    nl = lock_create(h, v, w.node, w.parent, SHARED_LOCK, &err);
    return ((nl != NULL) ? (LONG) MKBADDR(nl) : fail(h, err));
}

static LONG
action_parent(handler_t *h)
{
    return (lock_parent(h, BADDR(ARG1)));
}

static LONG
action_same_lock(handler_t *h)
{
    rlock_t *l1 = BADDR(ARG1);
    rlock_t *l2 = BADDR(ARG2);

    if ((l1 == NULL) || (l2 == NULL) || (l1->vol != l2->vol)) {
        h->pkt->dp_Res2 = LOCK_DIFFERENT;
        return (DOSFALSE);
    }
    if (l1->fl.fl_Key == l2->fl.fl_Key) {
        h->pkt->dp_Res2 = LOCK_SAME;
        return (DOSTRUE);
    }
    h->pkt->dp_Res2 = LOCK_SAME_VOLUME;
    return (DOSFALSE);
}

static LONG
action_change_mode(handler_t *h)
{
    rlock_t *l;
    LONG     access;

    if (ARG1 == CHANGE_FH) {
        struct FileHandle *fh = BADDR(ARG2);
        rfile_t *rf = (rfile_t *) fh->fh_Arg1;
        l = rf->lock;
        access = (ARG3 == MODE_NEWFILE) ? EXCLUSIVE_LOCK : SHARED_LOCK;
    } else {
        l = BADDR(ARG2);
        access = (ARG3 == EXCLUSIVE_LOCK) ? EXCLUSIVE_LOCK : SHARED_LOCK;
    }
    if (l == NULL)
        return (fail(h, ERROR_OBJECT_WRONG_TYPE));
    if ((access == EXCLUSIVE_LOCK) &&
        node_locked(h, l->vol, l->fl.fl_Key, l))
        return (fail(h, ERROR_OBJECT_IN_USE));
    l->fl.fl_Access = access;
    return (DOSTRUE);
}

/* ------------------------------------------------------------------ */
/* Examine                                                            */
/* ------------------------------------------------------------------ */

static void
fill_fib(struct FileInfoBlock *fib, const fs_stat_t *st, int is_root)
{
    LONG type;

    if (is_root)
        type = ST_ROOT;
    else
        type = (st->type == FS_TYPE_DIR) ? ST_USERDIR : ST_FILE;
    fib->fib_DirEntryType = type;
    fib->fib_EntryType    = type;
    cstr_to_bstr(st->name, fib->fib_FileName, sizeof (fib->fib_FileName));
    fib->fib_Protection = attr_to_protection(st->attr);
    fib->fib_Size       = (st->type == FS_TYPE_DIR) ? 0 : st->size;
    fib->fib_NumBlocks  = st->blocks;
    fs_time_to_datestamp(st->mtime, &fib->fib_Date);
    fib->fib_Comment[0] = '\0';
    fib->fib_Comment[1] = '\0';
    fib->fib_OwnerUID   = st->owner_user;
    fib->fib_OwnerGID   = st->owner_group;
}

static LONG
examine_lock(handler_t *h, rlock_t *l, struct FileInfoBlock *fib)
{
    volrec_t *v;
    fs_stat_t st;
    where_t   w;
    LONG      err;
    int       rc;

    v = lock_volume(h, l, &err);
    if (v == NULL)
        return (fail(h, err));
    w.node   = (l != NULL) ? (fs_node_t) l->fl.fl_Key : fs_root(v->fs);
    w.parent = (l != NULL) ? l->parent : FS_NODE_NONE;
    rc = stat_where(v, &w, &st);
    if (rc != FS_OK)
        return (fail(h, fs_error_to_dos(rc)));
    fill_fib(fib, &st, w.node == fs_root(v->fs));
    fib->fib_DiskKey = 0;       /* Directory position for ExNext */
    return (DOSTRUE);
}

static LONG
action_examine_object(handler_t *h)
{
    return (examine_lock(h, BADDR(ARG1), BADDR(ARG2)));
}

static LONG
action_examine_next(handler_t *h)
{
    rlock_t              *l   = BADDR(ARG1);
    struct FileInfoBlock *fib = BADDR(ARG2);
    volrec_t *v;
    fs_stat_t st;
    uint32_t  cookie;
    fs_node_t dir;
    LONG      err;
    int       rc;

    v = lock_volume(h, l, &err);
    if (v == NULL)
        return (fail(h, err));
    dir    = (l != NULL) ? (fs_node_t) l->fl.fl_Key : fs_root(v->fs);
    cookie = fib->fib_DiskKey;
    rc = fs_readdir(v->fs, dir, &cookie, &st);
    if (rc == FS_ENOENT)
        return (fail(h, ERROR_NO_MORE_ENTRIES));
    if (rc == FS_ENOTDIR)
        return (fail(h, ERROR_OBJECT_WRONG_TYPE));
    if (rc != FS_OK)
        return (fail(h, fs_error_to_dos(rc)));
    fill_fib(fib, &st, 0);
    fib->fib_DiskKey = cookie;
    return (DOSTRUE);
}

static LONG
fill_info(handler_t *h, struct InfoData *id)
{
    memset(id, 0, sizeof (*id));
    id->id_NumSoftErrors = h->dev.errors;
    id->id_UnitNumber    = h->fssm->fssm_Unit;
    id->id_DiskState     = (check_writable(h) != 0) ? ID_WRITE_PROTECTED :
                                                     ID_VALIDATED;
    id->id_BytesPerBlock = h->geo.block_size;
    id->id_NumBlocks     = h->dev.bd.sector_count;
    id->id_NumBlocksUsed = h->dev.bd.sector_count;

    if (h->cur != NULL) {
        fs_volinfo_t vi;
        if (fs_volinfo(h->cur->fs, &vi) == FS_OK) {
            id->id_BytesPerBlock = vi.block_size;   /* May differ */
            id->id_NumBlocks     = vi.total_blocks;
            id->id_NumBlocksUsed = vi.total_blocks - vi.free_blocks;
        }
        /*
         * Report a DOS disk so that Workbench shows the volume; the
         * volume node itself carries the OS9 type (fs_type_to_id()).
         */
        id->id_DiskType   = ID_DOS_DISK;
        id->id_VolumeNode = MKBADDR(h->cur->dl);
        id->id_InUse      = (h->cur->nlocks != 0) ? DOSTRUE : DOSFALSE;
    } else if (h->inhibit) {
        id->id_DiskType = ID_BUSY;
    } else if (h->media == MEDIA_NONE) {
        id->id_DiskType = ID_NO_DISK_PRESENT;
    } else if (h->media == MEDIA_FOREIGN) {
        id->id_DiskType = ID_NOT_REALLY_DOS;
    } else {
        id->id_DiskType = ID_UNREADABLE_DISK;
    }
    return (DOSTRUE);
}

static LONG
action_disk_info(handler_t *h)
{
    return (fill_info(h, BADDR(ARG1)));
}

static LONG
action_info(handler_t *h)
{
    rlock_t *l = BADDR(ARG1);

    if ((l != NULL) && (l->vol != h->cur))
        return (fail(h, ERROR_DEVICE_NOT_MOUNTED));
    return (fill_info(h, BADDR(ARG2)));
}

/* ------------------------------------------------------------------ */
/* Files                                                              */
/* ------------------------------------------------------------------ */

static LONG
open_file(handler_t *h, struct FileHandle *fh, rlock_t *l, int writable,
          int truncate)
{
    rfile_t   *rf;
    fs_file_t *f;
    int        rc;

    rc = fs_open(l->vol->fs, l->fl.fl_Key,
                 FS_OPEN_READ | (writable ? FS_OPEN_WRITE : 0), &f);
    if ((rc == FS_OK) && truncate) {
        rc = fs_truncate(f, 0);
        if (rc != FS_OK)
            fs_close(f);
    }
    if (rc != FS_OK)
        return (fs_error_to_dos(rc));
    rf = AllocMem(sizeof (*rf), MEMF_PUBLIC | MEMF_CLEAR);
    if (rf == NULL) {
        fs_close(f);
        return (ERROR_NO_FREE_STORE);
    }
    rf->lock     = l;
    rf->file     = f;
    rf->writable = writable;
    rf->dirty    = truncate;
    rf->next     = h->files;
    h->files      = rf;
    fh->fh_Arg1  = (LONG) rf;
    fh->fh_Port  = DOSFALSE;    /* Not interactive */
    if (truncate)
        schedule_flush(h);
    return (0);
}

/* ACTION_FINDINPUT, ACTION_FINDOUTPUT, ACTION_FINDUPDATE */
static LONG
action_find(handler_t *h)
{
    struct FileHandle *fh = BADDR(ARG1);
    char      name[FS_NAME_MAX + 1];
    where_t   w;
    volrec_t *v;
    fs_stat_t st;
    rlock_t  *l;
    LONG      access = SHARED_LOCK;
    LONG      err;
    int       existed = 1;
    int       writable;
    int       rc;

    v = resolve(h, ARG2, ARG3, &w, name, &err);
    if (v == NULL)
        return (fail(h, err));
    if (name[0] == '\0')
        return (fail(h, ERROR_OBJECT_WRONG_TYPE));

    rc = fs_lookup(v->fs, w.node, name, &st);
    if ((rc == FS_ENOENT) && (h->pkt->dp_Type != ACTION_FINDINPUT)) {
        err = check_writable(h);
        if (err != 0)
            return (fail(h, err));
        rc = fs_create(v->fs, w.node, name, NEW_FILE_ATTR, &st);
        if (rc != FS_OK)
            return (fail(h, fs_error_to_dos(rc)));
        existed = 0;
        schedule_flush(h);
    } else if (rc == FS_ENOTDIR) {
        return (fail(h, ERROR_DIR_NOT_FOUND));
    } else if (rc != FS_OK) {
        return (fail(h, fs_error_to_dos(rc)));
    }
    if (st.type == FS_TYPE_DIR)
        return (fail(h, ERROR_OBJECT_WRONG_TYPE));

    writable = (check_writable(h) == 0) && (st.attr & FS_ATTR_WRITE);
    if (h->pkt->dp_Type == ACTION_FINDOUTPUT) {
        access = EXCLUSIVE_LOCK;
        if ((err = check_writable(h)) != 0)
            return (fail(h, err));
        if (!(st.attr & FS_ATTR_WRITE))
            return (fail(h, ERROR_WRITE_PROTECTED));
    } else if (!(st.attr & FS_ATTR_READ)) {
        return (fail(h, ERROR_READ_PROTECTED));
    }

    l = lock_create(h, v, st.node, w.node, access, &err);
    if (l == NULL)
        return (fail(h, err));
    err = open_file(h, fh, l, writable,
                    (h->pkt->dp_Type == ACTION_FINDOUTPUT) && existed);
    if (err != 0) {
        lock_free(h, l);
        return (fail(h, err));
    }
    return (DOSTRUE);
}

static LONG
action_fh_from_lock(handler_t *h)
{
    struct FileHandle *fh = BADDR(ARG1);
    rlock_t  *l = BADDR(ARG2);
    volrec_t *v;
    fs_stat_t st;
    where_t   w;
    LONG      err;
    int       rc;

    v = lock_volume(h, l, &err);
    if ((v == NULL) || (l == NULL))
        return (fail(h, (v == NULL) ? err : ERROR_OBJECT_WRONG_TYPE));
    w.node   = l->fl.fl_Key;
    w.parent = l->parent;
    rc = stat_where(v, &w, &st);
    if (rc != FS_OK)
        return (fail(h, fs_error_to_dos(rc)));
    if (st.type == FS_TYPE_DIR)
        return (fail(h, ERROR_OBJECT_WRONG_TYPE));
    /* The lock now belongs to the file handle */
    err = open_file(h, fh, l, (check_writable(h) == 0) &&
                    (st.attr & FS_ATTR_WRITE), 0);
    return ((err == 0) ? DOSTRUE : fail(h, err));
}

/* Open file from fh_Arg1, or NULL if it is no longer usable */
static rfile_t *
arg_file(LONG arg, LONG *errp)
{
    rfile_t *rf = (rfile_t *) arg;

    if (rf == NULL) {
        *errp = ERROR_REQUIRED_ARG_MISSING;
        return (NULL);
    }
    if (rf->file == NULL) {
        *errp = ERROR_DEVICE_NOT_MOUNTED;  /* Disk was removed */
        return (NULL);
    }
    return (rf);
}

static LONG
action_read(handler_t *h)
{
    rfile_t *rf;
    uint32_t done;
    LONG     err;
    int      rc;

    rf = arg_file(ARG1, &err);
    if (rf == NULL) {
        h->pkt->dp_Res2 = err;
        return (-1);
    }
    if (ARG3 <= 0)
        return (0);
    rc = fs_read(rf->file, (void *) ARG2, ARG3, &done);
    if (rc != FS_OK) {
        h->pkt->dp_Res2 = fs_error_to_dos(rc);
        return (-1);
    }
    return (done);
}

static LONG
action_write(handler_t *h)
{
    rfile_t *rf;
    uint32_t done;
    LONG     err;
    int      rc;

    rf = arg_file(ARG1, &err);
    if (rf == NULL) {
        h->pkt->dp_Res2 = err;
        return (-1);
    }
    if (!rf->writable) {
        err = check_writable(h);
        h->pkt->dp_Res2 = (err != 0) ? err : ERROR_WRITE_PROTECTED;
        return (-1);
    }
    if (ARG3 <= 0)
        return (0);
    rc = fs_write(rf->file, (const void *) ARG2, ARG3, &done);
    if (done > 0) {
        rf->dirty = 1;
        schedule_flush(h);
    }
    if (rc != FS_OK) {
        h->pkt->dp_Res2 = fs_error_to_dos(rc);
        return ((done > 0) ? (LONG) done : -1);
    }
    return (done);
}

/* New position for ACTION_SEEK and ACTION_SET_FILE_SIZE, or -1 */
static LONG
seek_target(rfile_t *rf, LONG offset, LONG mode)
{
    LONG base;

    if (mode == OFFSET_BEGINNING)
        base = 0;
    else if (mode == OFFSET_END)
        base = fs_file_size(rf->file);
    else
        base = rf->file->pos;
    if ((offset > 0) && (base + offset < base))
        return (-1);
    base += offset;
    return ((base < 0) ? -1 : base);
}

static LONG
action_seek(handler_t *h)
{
    rfile_t *rf;
    LONG     old;
    LONG     pos;
    LONG     err;

    rf = arg_file(ARG1, &err);
    if (rf == NULL) {
        h->pkt->dp_Res2 = err;
        return (-1);
    }
    old = rf->file->pos;
    pos = seek_target(rf, ARG2, ARG3);
    if ((pos < 0) || ((uint32_t) pos > fs_file_size(rf->file))) {
        h->pkt->dp_Res2 = ERROR_SEEK_ERROR;
        return (-1);
    }
    rf->file->pos = pos;
    return (old);
}

static LONG
action_set_file_size(handler_t *h)
{
    rfile_t *rf;
    LONG     size;
    LONG     err;
    int      rc;

    rf = arg_file(ARG1, &err);
    if (rf == NULL) {
        h->pkt->dp_Res2 = err;
        return (-1);
    }
    if (!rf->writable) {
        err = check_writable(h);
        h->pkt->dp_Res2 = (err != 0) ? err : ERROR_WRITE_PROTECTED;
        return (-1);
    }
    size = seek_target(rf, ARG2, ARG3);
    if (size < 0) {
        h->pkt->dp_Res2 = ERROR_SEEK_ERROR;
        return (-1);
    }
    rc = fs_truncate(rf->file, size);
    if (rc != FS_OK) {
        h->pkt->dp_Res2 = fs_error_to_dos(rc);
        return (-1);
    }
    if (rf->file->pos > (uint32_t) size)
        rf->file->pos = size;
    rf->dirty = 1;
    schedule_flush(h);
    return (size);
}

static LONG
action_end(handler_t *h)
{
    rfile_t  *rf = (rfile_t *) ARG1;
    rfile_t **pp;
    LONG      err = 0;

    if (rf == NULL)
        return (DOSTRUE);
    for (pp = &h->files; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == rf) {
            *pp = rf->next;
            break;
        }
    }
    if (rf->file != NULL) {
        int rc = fs_close(rf->file);
        if (rc != FS_OK)
            err = fs_error_to_dos(rc);
        if (rf->dirty) {
            h->dirty = 1;        /* Close updated the FD in the cache */
            flush_now(h);        /* Written files reach the disk on close */
        }
    }
    lock_free(h, rf->lock);
    FreeMem(rf, sizeof (*rf));
    return ((err == 0) ? DOSTRUE : fail(h, err));
}

static LONG
action_copy_dir_fh(handler_t *h)
{
    rfile_t *rf = (rfile_t *) ARG1;

    if (rf == NULL)
        return (fail(h, ERROR_REQUIRED_ARG_MISSING));
    return (dup_lock(h, rf->lock));
}

static LONG
action_parent_fh(handler_t *h)
{
    rfile_t *rf = (rfile_t *) ARG1;

    if (rf == NULL)
        return (fail(h, ERROR_REQUIRED_ARG_MISSING));
    return (lock_parent(h, rf->lock));
}

static LONG
action_examine_fh(handler_t *h)
{
    rfile_t *rf = (rfile_t *) ARG1;

    if (rf == NULL)
        return (fail(h, ERROR_REQUIRED_ARG_MISSING));
    return (examine_lock(h, rf->lock, BADDR(ARG2)));
}

/* ------------------------------------------------------------------ */
/* Directory changes                                                  */
/* ------------------------------------------------------------------ */

static LONG
action_delete_object(handler_t *h)
{
    char      name[FS_NAME_MAX + 1];
    where_t   w;
    volrec_t *v;
    fs_stat_t st;
    LONG      err;
    int       rc;

    v = resolve(h, ARG1, ARG2, &w, name, &err);
    if (v == NULL)
        return (fail(h, err));
    if ((err = check_writable(h)) != 0)
        return (fail(h, err));
    if (name[0] == '\0')
        return (fail(h, ERROR_OBJECT_WRONG_TYPE));
    rc = fs_lookup(v->fs, w.node, name, &st);
    if (rc != FS_OK)
        return (fail(h, fs_error_to_dos(rc)));
    if (node_locked(h, v, st.node, NULL))
        return (fail(h, ERROR_OBJECT_IN_USE));
    if ((st.type == FS_TYPE_FILE) && !(st.attr & FS_ATTR_WRITE))
        return (fail(h, ERROR_DELETE_PROTECTED));
    rc = fs_remove(v->fs, w.node, name);
    if (rc != FS_OK)
        return (fail(h, fs_error_to_dos(rc)));
    schedule_flush(h);
    return (DOSTRUE);
}

static LONG
action_rename_object(handler_t *h)
{
    char      sname[FS_NAME_MAX + 1];
    char      dname[FS_NAME_MAX + 1];
    where_t   sw;
    where_t   dw;
    volrec_t *v;
    fs_stat_t st;
    LONG      err;
    int       rc;

    v = resolve(h, ARG1, ARG2, &sw, sname, &err);
    if (v == NULL)
        return (fail(h, err));
    if (resolve(h, ARG3, ARG4, &dw, dname, &err) != v)
        return (fail(h, (err != 0) ? err : ERROR_RENAME_ACROSS_DEVICES));
    if ((err = check_writable(h)) != 0)
        return (fail(h, err));
    if ((sname[0] == '\0') || (dname[0] == '\0'))
        return (fail(h, ERROR_OBJECT_WRONG_TYPE));
    rc = fs_lookup(v->fs, sw.node, sname, &st);
    if (rc == FS_OK)
        rc = fs_rename(v->fs, sw.node, sname, dw.node, dname);
    if (rc != FS_OK)
        return (fail(h, fs_error_to_dos(rc)));
    if (dw.node != sw.node)
        locks_moved(h, v, st.node, dw.node);
    schedule_flush(h);
    return (DOSTRUE);
}

static LONG
action_create_dir(handler_t *h)
{
    char      name[FS_NAME_MAX + 1];
    where_t   w;
    volrec_t *v;
    fs_stat_t st;
    rlock_t  *l;
    LONG      err;
    int       rc;

    v = resolve(h, ARG1, ARG2, &w, name, &err);
    if (v == NULL)
        return (fail(h, err));
    if ((err = check_writable(h)) != 0)
        return (fail(h, err));
    if (name[0] == '\0')
        return (fail(h, ERROR_OBJECT_EXISTS));
    rc = fs_mkdir(v->fs, w.node, name, NEW_DIR_ATTR, &st);
    if (rc != FS_OK)
        return (fail(h, fs_error_to_dos(rc)));
    schedule_flush(h);
    l = lock_create(h, v, st.node, w.node, SHARED_LOCK, &err);
    return ((l != NULL) ? (LONG) MKBADDR(l) : fail(h, err));
}

/* Resolves ARG2 (lock) and ARG3 (name) for the SET_* actions */
static volrec_t *
resolve_for_set(handler_t *h, fs_stat_t *st, LONG *errp)
{
    where_t   w;
    volrec_t *v;
    int       rc;

    v = resolve(h, ARG2, ARG3, &w, NULL, errp);
    if (v == NULL)
        return (NULL);
    if ((*errp = check_writable(h)) != 0)
        return (NULL);
    rc = stat_where(v, &w, st);
    if (rc != FS_OK) {
        *errp = fs_error_to_dos(rc);
        return (NULL);
    }
    return (v);
}

static LONG
action_set_protect(handler_t *h)
{
    volrec_t *v;
    fs_stat_t st;
    LONG      err;
    int       rc;

    v = resolve_for_set(h, &st, &err);
    if (v == NULL)
        return (fail(h, err));
    rc = fs_setattr(v->fs, st.node, protection_to_attr(ARG4, st.attr));
    if (rc != FS_OK)
        return (fail(h, fs_error_to_dos(rc)));
    schedule_flush(h);
    return (DOSTRUE);
}

static LONG
action_set_date(handler_t *h)
{
    volrec_t *v;
    fs_stat_t st;
    LONG      err;
    int       rc;

    v = resolve_for_set(h, &st, &err);
    if (v == NULL)
        return (fail(h, err));
    rc = fs_setmtime(v->fs, st.node,
                     datestamp_to_fs_time((struct DateStamp *) ARG4));
    if (rc != FS_OK)
        return (fail(h, fs_error_to_dos(rc)));
    schedule_flush(h);
    return (DOSTRUE);
}

static LONG
action_set_owner(handler_t *h)
{
    volrec_t *v;
    fs_stat_t st;
    LONG      err;
    int       rc;

    v = resolve_for_set(h, &st, &err);
    if (v == NULL)
        return (fail(h, err));
    /* ARG4: UID in the upper word, GID in the lower */
    rc = fs_setowner(v->fs, st.node, ARG4 & 0xffff, (ULONG) ARG4 >> 16);
    if (rc != FS_OK)
        return (fail(h, fs_error_to_dos(rc)));
    schedule_flush(h);
    return (DOSTRUE);
}

/* ------------------------------------------------------------------ */
/* Volume and handler control                                         */
/* ------------------------------------------------------------------ */

static LONG
action_rename_disk(handler_t *h)
{
    char name[FS_VOLNAME_MAX + 2];
    LONG err;

    if (h->cur == NULL)
        return (fail(h, no_volume_error(h)));
    if ((err = check_writable(h)) != 0)
        return (fail(h, err));
    bstr_to_cstr(ARG1, name, sizeof (name));
    err = volume_relabel(h, name);
    return ((err == 0) ? DOSTRUE : fail(h, err));
}

static LONG
action_serialize_disk(handler_t *h)
{
    struct DateStamp ds;
    uint32_t id;
    LONG     err;
    int      rc;

    if (h->cur == NULL)
        return (fail(h, no_volume_error(h)));
    if ((err = check_writable(h)) != 0)
        return (fail(h, err));
    DateStamp(&ds);
    id = ((ds.ds_Minute * 3000 + ds.ds_Tick) * 2654435761UL) >> 16;
    rc = fs_set_disk_id(h->cur->fs, id | 1);
    if (rc == FS_OK)
        rc = fs_sync(h->cur->fs);
    if (rc != FS_OK)
        return (fail(h, fs_error_to_dos(rc)));
    h->cur->disk_id = (id | 1) & 0xffff;
    return (DOSTRUE);
}

static LONG
action_inhibit(handler_t *h)
{
    if (ARG1 != DOSFALSE) {
        if (h->inhibit++ == 0) {
            flush_now(h);
            volume_dismount(h, 0);
        }
    } else if (h->inhibit > 0) {
        if (--h->inhibit == 0) {
            volume_check_media(h);
            if (h->cur != NULL)
                send_disk_event(1);
        }
    }
    return (DOSTRUE);
}

/*
 * Chooses the filesystem Format writes from the DOS type: the one in the
 * packet if it is one of ours, otherwise the one of the mount entry or
 * partition (Format sends DOS\x types for its OFS/FFS options).
 * ID_OS9K_DISK_BE and ID_OS9K_DISK_LE give OS-9000; anything else gives
 * OS-9/68k.  Mounting does not look at the DOS type.
 */
static uint8_t
format_type(handler_t *h, ULONG dostype)
{
    struct DosEnvec *de = BADDR(h->fssm->fssm_Environ);

    if (((dostype & 0xffffff00UL) != (ID_OS9_DISK & 0xffffff00UL)) &&
        (de != NULL) && (de->de_TableSize >= DE_DOSTYPE))
        dostype = de->de_DosType;
    switch (dostype) {
        case ID_OS9K_DISK_BE: return (FS_FMT_OS9K_BE);
        case ID_OS9K_DISK_LE: return (FS_FMT_OS9K_LE);
        default:              return (FS_FMT_OS9);
    }
}

static LONG
action_format(handler_t *h)
{
    char             name[FS_VOLNAME_MAX + 2];
    fs_format_opts_t opts;
    LONG             err;
    int              rc;

    if ((h->cur != NULL) && (h->cur->nlocks != 0))
        return (fail(h, ERROR_OBJECT_IN_USE));
    if ((err = check_writable(h)) != 0)
        return (fail(h, err));
    volume_dismount(h, 0);

    bstr_to_cstr(ARG1, name, sizeof (name));
    memset(&opts, 0, sizeof (opts));
    opts.name              = name;
    opts.sectors_per_track = h->geo.blocks_per_track;
    opts.heads             = h->geo.heads;
    opts.cylinders         = h->geo.high_cyl - h->geo.low_cyl + 1;
    opts.fs_type           = format_type(h, (ULONG) ARG2);
    rc = fs_format(&h->dev.bd, &opts);
    amigadev_motor_off(&h->dev);
    if (!h->inhibit)
        volume_check_media(h);
    if (rc != FS_OK)
        return (fail(h, fs_error_to_dos(rc)));
    return (DOSTRUE);
}

static LONG
action_write_protect(handler_t *h)
{
    if (ARG1 != DOSFALSE) {
        if (!h->soft_protect) {
            flush_now(h);
            h->soft_protect = 1;
            h->protect_key  = ARG2;
        }
    } else if (h->soft_protect) {
        if ((h->protect_key != 0) && ((ULONG) ARG2 != h->protect_key))
            return (fail(h, ERROR_INVALID_LOCK));
        h->soft_protect = 0;
    }
    return (DOSTRUE);
}

static LONG
action_current_volume(handler_t *h)
{
    return ((h->cur != NULL) ? (LONG) MKBADDR(h->cur->dl) : 0);
}

static LONG
action_more_cache(handler_t *h)
{
    LONG n = (LONG) h->buffers + ARG1;

    if (n < MIN_BUFFERS)
        n = MIN_BUFFERS;
    if (n > 1024)
        n = 1024;
    if ((h->cur != NULL) && (fs_set_cache(h->cur->fs, n) != FS_OK))
        return (fail(h, ERROR_NO_FREE_STORE));
    h->buffers = n;
    return (n);
}

static LONG
action_flush(handler_t *h)
{
    h->dirty = 1;
    flush_now(h);
    return (DOSTRUE);
}

static LONG
action_die(handler_t *h)
{
    if ((h->locks != NULL) || (h->files != NULL))
        return (fail(h, ERROR_OBJECT_IN_USE));
    h->running = 0;
    return (DOSTRUE);
}

/* ------------------------------------------------------------------ */

void
handle_packet(handler_t *h, struct DosPacket *packet)
{
    LONG res1;

    h->pkt = packet;
    h->pkt->dp_Res2 = 0;
    debugf("rbf: pkt %ld %lx %lx %lx %lx\n", (long) h->pkt->dp_Type,
            (unsigned long) ARG1, (unsigned long) ARG2,
            (unsigned long) ARG3, (unsigned long) ARG4);

    switch (h->pkt->dp_Type) {
        case ACTION_NIL:              res1 = DOSTRUE;                   break;
        case ACTION_LOCATE_OBJECT:    res1 = action_locate_object(h);    break;
        case ACTION_FREE_LOCK:        res1 = action_free_lock(h);        break;
        case ACTION_COPY_DIR:         res1 = action_copy_dir(h);         break;
        case ACTION_PARENT:           res1 = action_parent(h);           break;
        case ACTION_SAME_LOCK:        res1 = action_same_lock(h);        break;
        case ACTION_CHANGE_MODE:      res1 = action_change_mode(h);      break;
        case ACTION_EXAMINE_OBJECT:   res1 = action_examine_object(h);   break;
        case ACTION_EXAMINE_NEXT:     res1 = action_examine_next(h);     break;
        case ACTION_DISK_INFO:        res1 = action_disk_info(h);        break;
        case ACTION_INFO:             res1 = action_info(h);             break;
        case ACTION_FINDINPUT:
        case ACTION_FINDOUTPUT:
        case ACTION_FINDUPDATE:       res1 = action_find(h);             break;
        case ACTION_FH_FROM_LOCK:     res1 = action_fh_from_lock(h);     break;
        case ACTION_READ:             res1 = action_read(h);             break;
        case ACTION_WRITE:            res1 = action_write(h);            break;
        case ACTION_SEEK:             res1 = action_seek(h);             break;
        case ACTION_SET_FILE_SIZE:    res1 = action_set_file_size(h);    break;
        case ACTION_END:              res1 = action_end(h);              break;
        case ACTION_COPY_DIR_FH:      res1 = action_copy_dir_fh(h);      break;
        case ACTION_PARENT_FH:        res1 = action_parent_fh(h);        break;
        case ACTION_EXAMINE_FH:       res1 = action_examine_fh(h);       break;
        case ACTION_DELETE_OBJECT:    res1 = action_delete_object(h);    break;
        case ACTION_RENAME_OBJECT:    res1 = action_rename_object(h);    break;
        case ACTION_CREATE_DIR:       res1 = action_create_dir(h);       break;
        case ACTION_SET_PROTECT:      res1 = action_set_protect(h);      break;
        case ACTION_SET_DATE:         res1 = action_set_date(h);         break;
        case ACTION_SET_OWNER:        res1 = action_set_owner(h);        break;
        case ACTION_SET_COMMENT:      res1 = DOSTRUE;   /* Not stored */ break;
        case ACTION_RENAME_DISK:      res1 = action_rename_disk(h);      break;
        case ACTION_SERIALIZE_DISK:   res1 = action_serialize_disk(h);   break;
        case ACTION_INHIBIT:          res1 = action_inhibit(h);          break;
        case ACTION_FORMAT:           res1 = action_format(h);           break;
        case ACTION_WRITE_PROTECT:    res1 = action_write_protect(h);    break;
        case ACTION_CURRENT_VOLUME:   res1 = action_current_volume(h);   break;
        case ACTION_IS_FILESYSTEM:    res1 = DOSTRUE;                   break;
        case ACTION_MORE_CACHE:       res1 = action_more_cache(h);       break;
        case ACTION_FLUSH:            res1 = action_flush(h);            break;
        case ACTION_DISK_CHANGE:
            volume_check_media(h);
            res1 = DOSTRUE;
            break;
        case ACTION_GET_DISK_FSSM:    res1 = (LONG) h->fssm;             break;
        case ACTION_FREE_DISK_FSSM:   res1 = DOSTRUE;                   break;
        case ACTION_DIE:              res1 = action_die(h);              break;
        default:
            /* ExAll, notification and record locks: DOS emulates or fails */
            res1 = fail(h, ERROR_ACTION_NOT_KNOWN);
            break;
    }
    h->pkt->dp_Res1 = res1;
}
