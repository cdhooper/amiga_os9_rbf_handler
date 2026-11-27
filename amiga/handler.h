/*
 * OS-9 RBF filesystem handler for AmigaOS (OS-9/6809, OS-9/68k and
 * OS-9000 RBF)
 */

#ifndef _HANDLER_H
#define _HANDLER_H

#include <exec/types.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include "fs_api.h"
#include "amigadev.h"

#define HANDLER_NAME    "os9-rbf-handler"
#define ID_OS9_6809_DISK 0x4f533900UL   /* 'OS9\0': OS-9/6809 RBF */
#define ID_OS9_DISK     0x4f533901UL    /* 'OS9\1': OS-9/68k RBF */
#define ID_OS9K_DISK_BE 0x4f533902UL    /* 'OS9\2': OS-9000, big-endian */
#define ID_OS9K_DISK_LE 0x4f533903UL    /* 'OS9\3': OS-9000, little-endian */
#define ID_OS9_MASK     0xffffff00UL    /* 'OS9' */

/* DOS type of a mounted filesystem (fs_volinfo.fs_type) */
static inline ULONG
fs_type_to_id(uint8_t fs_type)
{
    switch (fs_type) {
        case FS_FMT_OS9_6809: return (ID_OS9_6809_DISK);
        case FS_FMT_OS9K_BE:  return (ID_OS9K_DISK_BE);
        case FS_FMT_OS9K_LE:  return (ID_OS9K_DISK_LE);
        default:              return (ID_OS9_DISK);
    }
}

#ifndef ID_BUSY
#define ID_BUSY         0x42555359UL    /* 'BUSY': inhibited */
#endif

#ifndef ACTION_GET_DISK_FSSM
#define ACTION_GET_DISK_FSSM    4201
#define ACTION_FREE_DISK_FSSM   4202
#endif

/* Attributes of objects created from AmigaOS */
#define NEW_FILE_ATTR   (FS_ATTR_READ | FS_ATTR_WRITE | FS_ATTR_EXEC | \
                         FS_ATTR_PREAD | FS_ATTR_PEXEC)
#define NEW_DIR_ATTR    (FS_ATTR_PERMS & ~FS_ATTR_NONSHARE)

#define FLUSH_DELAY_MS  1000    /* Write-back delay after the last write */
#define FLUSH_MAX_MS    5000    /* ... and after the first unflushed write */
#define DEFAULT_BUFFERS 32
#define MIN_BUFFERS     8
#define MAX_PATH        256

typedef struct volrec volrec_t;
typedef struct rlock  rlock_t;

/*
 * A volume this handler has seen.  It stays in the DOS list while it is
 * mounted, or while locks on it remain after its disk was removed.
 */
struct volrec {
    volrec_t       *next;
    struct DosList *dl;         /* DLT_VOLUME node */
    fs_volume_t    *fs;         /* NULL while the disk is not inserted */
    uint32_t        disk_id;
    fs_time_t       created;
    char            name[FS_VOLNAME_MAX + 1];
    unsigned        nlocks;     /* Locks and open files */
    uint8_t         in_list;    /* Added to the DOS list */
    uint8_t         want_list;  /* Should be in the DOS list */
};

/* A FileLock as handed to DOS, with handler state after it */
struct rlock {
    struct FileLock fl;         /* Must be first: BPTR to this is the lock */
    rlock_t        *next;       /* All locks of the handler */
    volrec_t       *vol;
    fs_node_t       parent;     /* Directory holding the object */
};

/* Per open file: DOS passes this back as fh_Arg1 */
typedef struct rfile rfile_t;
struct rfile {
    rfile_t   *next;            /* All open files */
    rlock_t   *lock;
    fs_file_t *file;            /* NULL while its disk is out */
    uint32_t   pos;             /* Position kept while detached */
    uint8_t    writable;
    uint8_t    dirty;
};

/* Disk states reported by Info() */
enum media_state {
    MEDIA_NONE,             /* No disk */
    MEDIA_UNREADABLE,       /* I/O error reading the disk */
    MEDIA_FOREIGN,          /* Not an RBF filesystem */
    MEDIA_MOUNTED,
};

/*
 * Handler state.  One instance per handler process: the same code may run
 * as several processes (one per partition using the filesystem), so the
 * handler keeps no writable global state.
 */
typedef struct handler {
    struct Process          *proc;
    struct DosPacket        *pkt;           /* Packet being handled */
    struct MsgPort          *port;          /* pr_MsgPort */
    struct DeviceNode       *devnode;
    struct FileSysStartupMsg *fssm;
    amigadev_t               dev;
    geometry_t               geo;
    unsigned                 buffers;
    volrec_t                *vols;          /* All known volumes */
    volrec_t                *cur;           /* Mounted volume, or NULL */
    rlock_t                 *locks;         /* All locks */
    rfile_t                 *files;         /* All open files */
    enum media_state         media;
    unsigned                 inhibit;       /* ACTION_INHIBIT nesting */
    uint8_t                  soft_protect;  /* ACTION_WRITE_PROTECT */
    ULONG                    protect_key;
    uint8_t                  running;
    uint8_t                  dirty;         /* Needs a sync */
    ULONG                    dirty_since;   /* Ticks at first unflushed write */
    uint8_t                  doslist_pending;
    ULONG                    change_sig;
    ULONG                    change_count;
    struct MsgPort          *timer_port;
    struct timerequest      *timer_io;
    uint8_t                  timer_busy;
} handler_t;

extern struct ExecBase   *SysBase;
extern struct DosLibrary *DOSBase;

/* handler.c */
void handler_main(void);
void timer_start(handler_t *h, ULONG ms);
void schedule_flush(handler_t *h);
void flush_now(handler_t *h);

/* volume.c */
void      volume_check_media(handler_t *h);
void      volume_dismount(handler_t *h, int discard);
void      volume_release(handler_t *h, volrec_t *v);
void      doslist_update(handler_t *h);
LONG      volume_relabel(handler_t *h, const char *name);
rlock_t  *lock_create(handler_t *h, volrec_t *v, fs_node_t node,
                      fs_node_t parent, LONG access, LONG *errp);
void      lock_free(handler_t *h, rlock_t *l);
int       node_locked(handler_t *h, volrec_t *v, fs_node_t node,
                      rlock_t *except);
void      locks_moved(handler_t *h, volrec_t *v, fs_node_t node,
                      fs_node_t new_parent);
void      send_disk_event(int inserted);
void      files_detach(handler_t *h, volrec_t *v);
void      files_attach(handler_t *h, volrec_t *v);

/* packets.c */
void handle_packet(handler_t *h, struct DosPacket *pkt);

/* support.c */
LONG      fs_error_to_dos(int err);
ULONG     attr_to_protection(uint16_t attr);
uint16_t  protection_to_attr(ULONG prot, uint16_t old);
void      fs_time_to_datestamp(fs_time_t t, struct DateStamp *ds);
fs_time_t datestamp_to_fs_time(const struct DateStamp *ds);
void      bstr_to_cstr(BSTR b, char *buf, int size);
void      cstr_to_bstr(const char *s, UBYTE *b, int size);
#ifdef DEBUG
void      debugf(const char *fmt, ...)
              __attribute__((format(__printf__, 1, 2)));
#else
#define debugf(...) do { } while (0)
#endif

#endif /* _HANDLER_H */
