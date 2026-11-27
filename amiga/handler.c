/*
 * OS-9 RBF filesystem handler for AmigaOS: startup and main loop
 *
 * DOS starts the handler as a process when its device (from a mountlist
 * or an RDB partition with DosType 'OS9\0' to 'OS9\3') is
 * first used, and sends a startup packet with the FileSysStartupMsg.
 * The handler opens the exec device, mounts the volume if a disk is
 * present, and then serves packets until ACTION_DIE.
 */

#include <string.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "handler.h"

#ifndef VER
#define VER "unknown"
#endif
#ifndef VER_DATE
#define VER_DATE __DATE__
#endif

const char         rbf_version[] = "$VER: os9-rbf-handler " VER " (" VER_DATE ")";
struct ExecBase   *SysBase;
struct DosLibrary *DOSBase;

void
timer_start(handler_t *h, ULONG ms)
{
    if (h->timer_io == NULL)
        return;
    if (h->timer_busy) {
        /* Only ever shorten a pending wait */
        if (ms >= FLUSH_DELAY_MS)
            return;
        AbortIO((struct IORequest *) h->timer_io);
        WaitIO((struct IORequest *) h->timer_io);
    }
    h->timer_io->tr_node.io_Command = TR_ADDREQUEST;
    h->timer_io->tr_time.tv_secs    = ms / 1000;
    h->timer_io->tr_time.tv_micro   = (ms % 1000) * 1000;
    SendIO((struct IORequest *) h->timer_io);
    h->timer_busy = 1;
}

/* Ticks (1/50 s) since midnight */
static ULONG
ticks_now(void)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return ((ULONG) ds.ds_Minute * 60 * TICKS_PER_SECOND + ds.ds_Tick);
}

/*
 * Data was written: write it back to the disk FLUSH_DELAY_MS after the
 * last write, but no later than FLUSH_MAX_MS after the first unflushed
 * one, so that a steady stream of writes cannot hold the flush off.
 */
void
schedule_flush(handler_t *h)
{
    ULONG now = ticks_now();

    if (!h->dirty) {
        h->dirty = 1;
        h->dirty_since = now;
    } else if (h->timer_busy && !h->doslist_pending &&
               (now >= h->dirty_since) &&       /* Not past midnight */
               (now - h->dirty_since <
                FLUSH_MAX_MS * TICKS_PER_SECOND / 1000)) {
        /* Restart the pending wait */
        AbortIO((struct IORequest *) h->timer_io);
        WaitIO((struct IORequest *) h->timer_io);
        h->timer_busy = 0;
    }
    timer_start(h, FLUSH_DELAY_MS);
}

void
flush_now(handler_t *h)
{
    if ((h->cur != NULL) && h->dirty) {
        int rc = fs_sync(h->cur->fs);
        if (rc != FS_OK)
            debugf("rbf: sync: %s\n", fs_strerror(rc));
    }
    h->dirty = 0;
    amigadev_motor_off(&h->dev);
}

static void
timer_expired(handler_t *h)
{
    WaitIO((struct IORequest *) h->timer_io);
    h->timer_busy = 0;
    if (h->doslist_pending)
        doslist_update(h);
    if (h->dirty)
        flush_now(h);
}

static int
timer_open(handler_t *h)
{
    h->timer_port = CreateMsgPort();
    if (h->timer_port == NULL)
        return (0);
    h->timer_io = (struct timerequest *)
                  CreateIORequest(h->timer_port, sizeof (struct timerequest));
    if ((h->timer_io == NULL) ||
        (OpenDevice((STRPTR) TIMERNAME, UNIT_VBLANK,
                    (struct IORequest *) h->timer_io, 0) != 0)) {
        if (h->timer_io != NULL)
            DeleteIORequest((struct IORequest *) h->timer_io);
        h->timer_io = NULL;
        DeleteMsgPort(h->timer_port);
        h->timer_port = NULL;
        return (0);
    }
    return (1);
}

static void
timer_close(handler_t *h)
{
    if (h->timer_io == NULL)
        return;
    if (h->timer_busy) {
        AbortIO((struct IORequest *) h->timer_io);
        WaitIO((struct IORequest *) h->timer_io);
    }
    CloseDevice((struct IORequest *) h->timer_io);
    DeleteIORequest((struct IORequest *) h->timer_io);
    DeleteMsgPort(h->timer_port);
    h->timer_io = NULL;
}

static struct DosPacket *
get_packet(struct MsgPort *port)
{
    struct Message *msg = GetMsg(port);

    return ((msg != NULL) ? (struct DosPacket *) msg->mn_Node.ln_Name : NULL);
}

static LONG
startup(handler_t *h, struct DosPacket *pkt)
{
    LONG err;
    BPTR fssm_bptr = pkt->dp_Arg2;

    h->devnode = BADDR(pkt->dp_Arg3);
    if ((fssm_bptr == 0) || ((ULONG) fssm_bptr < 1024))
        return (ERROR_OBJECT_WRONG_TYPE);   /* No device to work with */
    h->fssm = BADDR(fssm_bptr);

    err = amigadev_open(&h->dev, h->fssm, &h->geo);
    if (err != 0)
        return (err);
    h->buffers = (h->geo.buffers > DEFAULT_BUFFERS) ? h->geo.buffers :
                                                      DEFAULT_BUFFERS;
    if (!timer_open(h)) {
        amigadev_close(&h->dev);
        return (ERROR_NO_FREE_STORE);
    }
    h->change_sig = AllocSignal(-1);
    if (h->change_sig != (ULONG) -1)
        amigadev_change_int_on(&h->dev, &h->proc->pr_Task,
                               1UL << h->change_sig);
    amigadev_change_count(&h->dev, &h->change_count);
    return (0);
}

/* Answers packets which arrive after ACTION_DIE was accepted */
static void
refuse_packets(handler_t *h)
{
    struct DosPacket *pkt;

    while ((pkt = get_packet(h->port)) != NULL)
        ReplyPkt(pkt, DOSFALSE, ERROR_DEVICE_NOT_MOUNTED);
}

static void
shutdown(handler_t *h)
{
    int tries;

    volume_dismount(h, 0);
    for (tries = 0; h->doslist_pending && (tries < 100); tries++) {
        refuse_packets(h);
        Delay(2);
        doslist_update(h);
    }
    refuse_packets(h);
    amigadev_close(&h->dev);
    timer_close(h);
    if (h->change_sig != (ULONG) -1)
        FreeSignal(h->change_sig);
}

void
handler_main(void)
{
    struct DosPacket *pkt;
    struct Process   *proc = (struct Process *) FindTask(NULL);
    handler_t        *h;
    LONG  err;
    ULONG port_sig;
    ULONG timer_sig;
    ULONG change_sig;

    WaitPort(&proc->pr_MsgPort);
    pkt = get_packet(&proc->pr_MsgPort);

    DOSBase = (struct DosLibrary *) OpenLibrary((STRPTR) "dos.library", 36);
    if (DOSBase == NULL)
        return;                 /* Kickstart 1.x is not supported */

    h = AllocMem(sizeof (*h), MEMF_PUBLIC | MEMF_CLEAR);
    if (h == NULL) {
        ReplyPkt(pkt, DOSFALSE, ERROR_NO_FREE_STORE);
        return;
    }
    h->change_sig = (ULONG) -1;
    h->proc = proc;
    h->port = &proc->pr_MsgPort;

    err = startup(h, pkt);
    if (err != 0) {
        debugf("rbf: startup failed %ld\n", (long) err);
        if (h->devnode != NULL)
            h->devnode->dn_Task = NULL;
        ReplyPkt(pkt, DOSFALSE, err);
        FreeMem(h, sizeof (*h));
        return;
    }
    h->devnode->dn_Task = h->port;
    h->running = 1;
    ReplyPkt(pkt, DOSTRUE, 0);
    debugf("rbf: started, %lu-byte blocks, cyl %lu-%lu\n",
            (unsigned long) h->geo.block_size, (unsigned long) h->geo.low_cyl,
            (unsigned long) h->geo.high_cyl);

    volume_check_media(h);

    port_sig   = 1UL << h->port->mp_SigBit;
    timer_sig  = 1UL << h->timer_port->mp_SigBit;
    change_sig = (h->change_sig != (ULONG) -1) ? (1UL << h->change_sig) : 0;

    while (h->running) {
        ULONG sigs = Wait(port_sig | timer_sig | change_sig);

        if ((sigs & timer_sig) && h->timer_busy &&
            (CheckIO((struct IORequest *) h->timer_io) != NULL))
            timer_expired(h);
        if (sigs & change_sig) {
            /*
             * No flush here: the drive may already hold another disk.
             * volume_check_media() discards the old volume's cache.
             */
            volume_check_media(h);
        }
        while (h->running && ((pkt = get_packet(h->port)) != NULL)) {
            handle_packet(h, pkt);
            if (!h->running)
                h->devnode->dn_Task = NULL;  /* ACTION_DIE accepted */
            ReplyPkt(pkt, pkt->dp_Res1, pkt->dp_Res2);
        }
    }
    shutdown(h);
    FreeMem(h, sizeof (*h));
}
