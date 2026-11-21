/*
 * Volumes and locks
 *
 * When a disk with an RBF filesystem is present, the handler mounts it
 * and puts a DLT_VOLUME node in the DOS list.  When the disk is removed
 * while locks on it remain, the node stays in the list with dol_Task
 * cleared (DOS then asks for the volume to be inserted); if the same
 * disk comes back, its node and locks become live again.
 *
 * The DOS list is only modified when AttemptLockDosList() succeeds: a
 * handler must not wait for the list lock, since the holder may be
 * waiting for this handler.  Pending changes are retried from the timer.
 */

#include <string.h>
#include <exec/memory.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "handler.h"

/* ------------------------------------------------------------------ */
/* Locks                                                              */
/* ------------------------------------------------------------------ */

/* Returns nonzero if any lock other than except refers to the node */
int
node_locked(handler_t *h, volrec_t *v, fs_node_t node, rlock_t *except)
{
    rlock_t *l;

    for (l = h->locks; l != NULL; l = l->next)
        if ((l != except) && (l->vol == v) &&
            ((fs_node_t) l->fl.fl_Key == node))
            return (1);
    return (0);
}

static int
lock_conflict(handler_t *h, volrec_t *v, fs_node_t node, LONG access)
{
    rlock_t *l;

    for (l = h->locks; l != NULL; l = l->next) {
        if ((l->vol != v) || ((fs_node_t) l->fl.fl_Key != node))
            continue;
        if ((access == EXCLUSIVE_LOCK) || (l->fl.fl_Access == EXCLUSIVE_LOCK))
            return (1);
    }
    return (0);
}

rlock_t *
lock_create(handler_t *h, volrec_t *v, fs_node_t node, fs_node_t parent,
            LONG access, LONG *errp)
{
    rlock_t *l;

    if (access != EXCLUSIVE_LOCK)
        access = SHARED_LOCK;
    if (lock_conflict(h, v, node, access)) {
        *errp = ERROR_OBJECT_IN_USE;
        return (NULL);
    }
    l = AllocMem(sizeof (*l), MEMF_PUBLIC | MEMF_CLEAR);
    if (l == NULL) {
        *errp = ERROR_NO_FREE_STORE;
        return (NULL);
    }
    l->fl.fl_Key    = node;
    l->fl.fl_Access = access;
    l->fl.fl_Task   = h->port;
    l->fl.fl_Volume = MKBADDR(v->dl);
    l->vol    = v;
    l->parent = parent;
    l->next   = h->locks;
    h->locks   = l;
    v->nlocks++;
    return (l);
}

void
lock_free(handler_t *h, rlock_t *l)
{
    rlock_t **pp;
    volrec_t *v = l->vol;

    for (pp = &h->locks; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == l) {
            *pp = l->next;
            break;
        }
    }
    FreeMem(l, sizeof (*l));
    if ((--v->nlocks == 0) && (v != h->cur))
        volume_release(h, v);
}

/*
 * Closes the core's side of every open file on a volume which is being
 * unmounted.  The DOS file handles stay valid; further I/O on them fails
 * until they are closed.
 */
void
files_detach(handler_t *h, volrec_t *v)
{
    rfile_t *rf;

    for (rf = h->files; rf != NULL; rf = rf->next) {
        if ((rf->file != NULL) && (rf->lock->vol == v)) {
            rf->pos = rf->file->pos;
            fs_close(rf->file);
            rf->file = NULL;
        }
    }
}

/*
 * The disk of a volume came back: reopen its files where they were, so
 * that a program which was asked to replace the volume can carry on.
 * Changes not yet written when the disk was removed are lost.
 */
void
files_attach(handler_t *h, volrec_t *v)
{
    rfile_t *rf;

    for (rf = h->files; rf != NULL; rf = rf->next) {
        fs_file_t *f;
        int        mode = FS_OPEN_READ;

        if ((rf->file != NULL) || (rf->lock->vol != v))
            continue;
        if (rf->writable && !h->dev.bd.read_only)
            mode |= FS_OPEN_WRITE;
        if (fs_open(v->fs, rf->lock->fl.fl_Key, mode, &f) != FS_OK)
            continue;           /* Still unusable; fails until closed */
        f->pos = rf->pos;
        if (f->pos > fs_file_size(f))
            f->pos = fs_file_size(f);
        rf->file = f;
        debugf("rbf: reattached file at %lu\n", (unsigned long) f->pos);
    }
}

/* After a rename moved node to another directory */
void
locks_moved(handler_t *h, volrec_t *v, fs_node_t node, fs_node_t new_parent)
{
    rlock_t *l;

    for (l = h->locks; l != NULL; l = l->next)
        if ((l->vol == v) && ((fs_node_t) l->fl.fl_Key == node))
            l->parent = new_parent;
}

/* ------------------------------------------------------------------ */
/* DOS list                                                           */
/* ------------------------------------------------------------------ */

/*
 * Brings the DOS list in line with want_list of each volume.  Retries
 * from the timer if the list is busy.
 */
void
doslist_update(handler_t *h)
{
    struct DosList *dl;
    volrec_t      **vp;
    volrec_t       *v;

    dl = AttemptLockDosList(LDF_VOLUMES | LDF_WRITE);
    if ((ULONG) dl <= 1) {
        h->doslist_pending = 1;
        timer_start(h, 100);
        return;
    }
    h->doslist_pending = 0;
    for (vp = &h->vols; (v = *vp) != NULL; ) {
        if (v->want_list && !v->in_list) {
            if (AddDosEntry(v->dl)) {
                v->in_list = 1;
            } else {
                /* Still usable through the device name */
                debugf("rbf: AddDosEntry %s failed\n", v->name);
                v->want_list = 0;
            }
        } else if (!v->want_list && v->in_list) {
            RemDosEntry(v->dl);
            v->in_list = 0;
        }
        if (!v->want_list && !v->in_list && (v->nlocks == 0) &&
            (v != h->cur)) {
            *vp = v->next;
            FreeDosEntry(v->dl);
            FreeMem(v, sizeof (*v));
            continue;
        }
        vp = &v->next;
    }
    UnLockDosList(LDF_VOLUMES | LDF_WRITE);
}

/* Drops a volume which is neither mounted nor locked */
void
volume_release(handler_t *h, volrec_t *v)
{
    v->want_list = 0;
    doslist_update(h);
}

static volrec_t *
volume_new(handler_t *h, const fs_volinfo_t *info)
{
    volrec_t *v = AllocMem(sizeof (*v), MEMF_PUBLIC | MEMF_CLEAR);

    if (v == NULL)
        return (NULL);
    v->dl = MakeDosEntry((STRPTR) info->name, DLT_VOLUME);
    if (v->dl == NULL) {
        FreeMem(v, sizeof (*v));
        return (NULL);
    }
    strcpy(v->name, info->name);
    v->disk_id = info->disk_id;
    v->created = info->created;
    fs_time_to_datestamp(info->created,
                         &v->dl->dol_misc.dol_volume.dol_VolumeDate);
    v->dl->dol_misc.dol_volume.dol_DiskType = fs_type_to_id(info->fs_type);
    v->next = h->vols;
    h->vols  = v;
    return (v);
}

/* ------------------------------------------------------------------ */
/* Mount and dismount                                                 */
/* ------------------------------------------------------------------ */

/* Tells Workbench and other input handlers that a disk changed */
void
send_disk_event(int inserted)
{
    struct MsgPort    *port;
    struct IOStdReq   *io;
    struct InputEvent  ie;

    port = CreateMsgPort();
    if (port == NULL)
        return;
    io = (struct IOStdReq *) CreateIORequest(port, sizeof (*io));
    if (io != NULL) {
        if (OpenDevice((STRPTR) "input.device", 0,
                       (struct IORequest *) io, 0) == 0) {
            memset(&ie, 0, sizeof (ie));
            ie.ie_Class = inserted ? IECLASS_DISKINSERTED :
                                     IECLASS_DISKREMOVED;
            io->io_Command = IND_WRITEEVENT;
            io->io_Data    = &ie;
            io->io_Length  = sizeof (ie);
            DoIO((struct IORequest *) io);
            CloseDevice((struct IORequest *) io);
        }
        DeleteIORequest((struct IORequest *) io);
    }
    DeleteMsgPort(port);
}

static void
volume_mount(handler_t *h)
{
    fs_volume_t *fs;
    fs_volinfo_t info;
    volrec_t    *v;
    int          rc;

    if (h->cur != NULL)
        return;
    if (!amigadev_disk_present(&h->dev)) {
        h->media = MEDIA_NONE;
        return;
    }
    h->dev.bd.read_only = amigadev_write_protected(&h->dev);
    rc = fs_mount(&h->dev.bd, h->buffers, &fs);
    if (rc != FS_OK) {
        debugf("rbf: mount: %s\n", fs_strerror(rc));
        h->media = (rc == FS_EIO) ? MEDIA_UNREADABLE : MEDIA_FOREIGN;
        return;
    }
    rc = fs_volinfo(fs, &info);
    if ((rc != FS_OK) || (info.name[0] == '\0')) {
        if (rc == FS_OK)
            strcpy(info.name, "OS-9");
        else {
            fs_unmount(fs, 1);
            h->media = MEDIA_UNREADABLE;
            return;
        }
    }

    /* The same disk back again? */
    for (v = h->vols; v != NULL; v = v->next) {
        if ((v->fs == NULL) && (v->disk_id == info.disk_id) &&
            (v->created == info.created) && (strcmp(v->name, info.name) == 0))
            break;
    }
    if (v == NULL) {
        v = volume_new(h, &info);
        if (v == NULL) {
            fs_unmount(fs, 1);
            h->media = MEDIA_UNREADABLE;
            return;
        }
    }
    v->fs = fs;
    v->dl->dol_Task = h->port;
    files_attach(h, v);
    v->want_list = 1;
    h->cur   = v;
    h->media = MEDIA_MOUNTED;
    doslist_update(h);
    debugf("rbf: mounted %s\n", v->name);
}

/*
 * Unmounts the current volume.  With discard, cached changes are thrown
 * away (the disk has already gone).
 *
 * When discarding, the device is held offline: closing the open files
 * still updates the old volume's bitmap and FDs in the cache, and an
 * eviction there would otherwise write the old disk's sectors onto
 * whatever is now in the drive.  ETD_ commands cannot catch this, since
 * the change count has already been updated for the new disk, and most
 * devices have no ETD_ commands.
 */
void
volume_dismount(handler_t *h, int discard)
{
    volrec_t *v = h->cur;

    if (v == NULL)
        return;
    if (discard)
        h->dev.offline = 1;
    files_detach(h, v);
    fs_unmount(v->fs, discard);
    h->dev.offline = 0;
    v->fs = NULL;
    v->dl->dol_Task = NULL;
    h->cur   = NULL;
    h->dirty = 0;
    if (v->nlocks == 0)
        v->want_list = 0;
    doslist_update(h);
}

/*
 * Called at startup, on a disk change interrupt, and when inhibit ends.
 * Brings the mounted state in line with the media in the drive.
 */
void
volume_check_media(handler_t *h)
{
    ULONG count;
    int   changed = 0;

    if (h->inhibit)
        return;
    if (amigadev_change_count(&h->dev, &count)) {
        changed = (count != h->change_count);
        h->change_count = count;
    }
    if (changed && (h->cur != NULL)) {
        /* Whatever was cached for the old disk is lost */
        volume_dismount(h, 1);
        send_disk_event(0);
    }
    if ((h->cur == NULL) && amigadev_disk_present(&h->dev)) {
        volume_mount(h);
        if (changed)
            send_disk_event(1);
    } else if (h->cur == NULL) {
        h->media = MEDIA_NONE;
    }
}

LONG
volume_relabel(handler_t *h, const char *name)
{
    volrec_t *v = h->cur;
    struct DosList *dl;
    UBYTE    *bname;
    UBYTE    *old;
    int       len = strlen(name);
    int       rc;

    rc = fs_relabel(v->fs, name);
    if (rc != FS_OK)
        return (fs_error_to_dos(rc));
    fs_sync(v->fs);
    strcpy(v->name, name);

    /* Rename the volume node; its name was allocated by MakeDosEntry */
    bname = AllocVec(len + 2, MEMF_PUBLIC);
    if (bname == NULL)
        return (0);
    cstr_to_bstr(name, bname, len + 2);
    dl = AttemptLockDosList(LDF_VOLUMES | LDF_WRITE);
    if ((ULONG) dl <= 1) {
        FreeVec(bname);
        return (0);             /* The disk has the new name regardless */
    }
    old = BADDR(v->dl->dol_Name);
    v->dl->dol_Name = MKBADDR(bname);
    UnLockDosList(LDF_VOLUMES | LDF_WRITE);
    FreeVec(old);
    return (0);
}
