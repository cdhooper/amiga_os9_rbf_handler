/*
 * Block device on a trackdisk-style exec device partition
 *
 * The partition is described by the mount's DosEnvec: LSN 0 of the
 * filesystem is the first block of LowCyl.  Transfers are split to
 * respect MaxTransfer, and buffers outside the device's DMA Mask go
 * through a bounce buffer.  Offsets beyond 4 GB use the NSD or TD64
 * 64-bit commands.
 */

#include <string.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <devices/newstyle.h>
#include <proto/exec.h>
#include "amigadev.h"
#include "handler.h"

#ifndef TD_READ64
#define TD_READ64   24
#define TD_WRITE64  25
#endif

#define BOUNCE_BLOCKS   16

#ifdef TRACE_IO
#define TRACE(...) debugf(__VA_ARGS__)
#else
#define TRACE(...) do { } while (0)
#endif

static int
do_io(amigadev_t *ad, int write, uint32_t lsn, uint32_t count, void *buf)
{
    struct IOExtTD  *iotd = ad->io;
    struct IOStdReq *io = &iotd->iotd_Req;
    uint64_t start = ((uint64_t) ad->start_hi << 32) | ad->start_lo;
    uint64_t off   = start + ((uint64_t) lsn << ad->block_shift);
    uint32_t len   = count << ad->block_shift;
    uint64_t last  = off + len - 1;
    int      plain = 0;

retry:
    TRACE("rbf: io %s lsn %lu n %lu buf %p sp %p flags %x\n",
          write ? "W" : "R", (unsigned long) lsn, (unsigned long) count,
          buf, &plain, ((struct Task *) FindTask(NULL))->tc_Flags);
    io->io_Length = len;
    io->io_Data   = buf;
    io->io_Offset = (ULONG) off;
    io->io_Actual = 0;
    if ((last >> 32) != 0) {
        io->io_Actual = (ULONG) (off >> 32);
        if (ad->cmd64 == 2)
            io->io_Command = write ? NSCMD_TD_WRITE64 : NSCMD_TD_READ64;
        else
            io->io_Command = write ? TD_WRITE64 : TD_READ64;
    } else if (ad->use_etd) {
        /* The device refuses the I/O if the disk changed since mount */
        io->io_Command = write ? ETD_WRITE : ETD_READ;
        iotd->iotd_Count    = ad->etd_count;
        iotd->iotd_SecLabel = 0;
        plain = 1;
    } else {
        io->io_Command = write ? CMD_WRITE : CMD_READ;
        plain = 1;
    }
    if (DoIO((struct IORequest *) io) != 0) {
        if ((io->io_Error == IOERR_NOCMD) && ad->use_etd &&
            ((io->io_Command & ~TDF_EXTCOM) != io->io_Command)) {
            ad->use_etd = 0;    /* Device has no ETD_ commands */
            goto retry;
        }
        ad->errors++;
        debugf("rbf: %s error %d at block %lu+%lu\n",
               write ? "write" : "read", io->io_Error, (unsigned long) lsn,
               (unsigned long) count);
        return (-1);
    }
    if (plain && (io->io_Actual != len)) {
        ad->errors++;
        return (-1);
    }
    return (0);
}

static int
xfer(amigadev_t *ad, int write, uint32_t lsn, uint32_t count, uint8_t *buf)
{
    uint32_t max_blocks = ad->max_transfer >> ad->block_shift;

    if (max_blocks == 0)
        max_blocks = 1;
    if ((uint64_t) lsn + count > ad->bd.sector_count)
        return (-1);

    while (count > 0) {
        uint32_t n = count;
        int      bounce = (((ULONG) buf & ~ad->mask) != 0) ||
                          (((ULONG) buf & 1) != 0);
        uint32_t limit = bounce ? (ad->bounce_size >> ad->block_shift) :
                                  max_blocks;

        if (limit > max_blocks)
            limit = max_blocks;
        if (n > limit)
            n = limit;
        if (bounce) {
            if (write)
                CopyMem(buf, ad->bounce, n << ad->block_shift);
            if (do_io(ad, write, lsn, n, ad->bounce) != 0)
                return (-1);
            if (!write)
                CopyMem(ad->bounce, buf, n << ad->block_shift);
        } else if (do_io(ad, write, lsn, n, buf) != 0) {
            return (-1);
        }
        buf   += n << ad->block_shift;
        lsn   += n;
        count -= n;
    }
    return (0);
}

/* No I/O at all while offline (a removed disk's volume is being dropped) */
static int
ad_read(blockdev_t *bd, uint32_t lsn, uint32_t count, void *buf)
{
    if (((amigadev_t *) bd)->offline)
        return (-1);
    return (xfer((amigadev_t *) bd, 0, lsn, count, buf));
}

static int
ad_write(blockdev_t *bd, uint32_t lsn, uint32_t count, const void *buf)
{
    if (bd->read_only || ((amigadev_t *) bd)->offline)
        return (-1);
    return (xfer((amigadev_t *) bd, 1, lsn, count, (uint8_t *) buf));
}

static int
ad_sync(blockdev_t *bd)
{
    amigadev_t      *ad = (amigadev_t *) bd;
    struct IOStdReq *io = &ad->io->iotd_Req;

    if (ad->offline)
        return (-1);
    io->io_Command = CMD_UPDATE;
    io->io_Length  = 0;
    DoIO((struct IORequest *) io);
    if ((io->io_Error != 0) && (io->io_Error != IOERR_NOCMD))
        return (-1);
    return (0);
}

/*
 * Simple device query.  Returns 0 and the result (io_Actual) in *result,
 * or nonzero if the command failed.  A failure must be kept apart from
 * the result: a4091.device reports "no disk" for TD_CHANGESTATE as
 * io_Actual = ~0 with no error.
 */
static int
query(amigadev_t *ad, UWORD cmd, ULONG *result)
{
    struct IOStdReq *io = &ad->io->iotd_Req;

    TRACE("rbf: query cmd %u dev %p unit %p\n", cmd, io->io_Device,
          io->io_Unit);

    /*
     * The result comes back in io_Actual, but some devices also write
     * through io_Data, so it must point somewhere harmless.
     */
    io->io_Command = cmd;
    io->io_Length  = 0;
    io->io_Data    = ad->bounce;
    io->io_Actual  = 0;
    if (DoIO((struct IORequest *) io) != 0)
        return (-1);
    *result = io->io_Actual;
    return (0);
}

int
amigadev_disk_present(amigadev_t *ad)
{
    ULONG state;

    if (query(ad, TD_CHANGESTATE, &state) != 0)
        return (1);             /* No command: fixed disk */
    return (state == 0);
}

int
amigadev_write_protected(amigadev_t *ad)
{
    ULONG state;

    return ((query(ad, TD_PROTSTATUS, &state) == 0) && (state != 0));
}

/*
 * Reads the disk change count.  I/O from then on is valid only while
 * the same disk stays in the drive.
 */
int
amigadev_change_count(amigadev_t *ad, ULONG *count)
{
    ULONG n;

    if (query(ad, TD_CHANGENUM, &n) != 0) {
        ad->use_etd = 0;
        return (0);
    }
    *count = n;
    ad->etd_count = n;
    return (1);
}

void
amigadev_motor_off(amigadev_t *ad)
{
    struct IOStdReq *io = &ad->io->iotd_Req;

    io->io_Command = TD_MOTOR;
    io->io_Length  = 0;
    DoIO((struct IORequest *) io);
}

/* Disk change software interrupt: wakes the handler */
static ULONG
change_int_code(amigadev_t *ad __asm("a1"))
{
    Signal(ad->chg_task, ad->chg_mask);
    return (0);
}

void
amigadev_change_int_on(amigadev_t *ad, struct Task *task, ULONG sigmask)
{
    struct IOStdReq *io;

    if (ad->chg_active || (ad->chg_io == NULL))
        return;
    ad->chg_task = task;
    ad->chg_mask = sigmask;
    ad->chg_int.is_Node.ln_Type = NT_INTERRUPT;
    ad->chg_int.is_Node.ln_Pri  = 0;
    ad->chg_int.is_Node.ln_Name = (char *) "os9-rbf-handler";
    ad->chg_int.is_Data = ad;
    ad->chg_int.is_Code = (VOID (*)()) change_int_code;

    /* The request stays with the device until TD_REMCHANGEINT */
    *ad->chg_io = *ad->io;
    io = &ad->chg_io->iotd_Req;
    io->io_Command = TD_ADDCHANGEINT;
    io->io_Data    = &ad->chg_int;
    io->io_Length  = sizeof (ad->chg_int);
    io->io_Flags   = 0;
    SendIO((struct IORequest *) io);
    /* A device without the command completes the request at once */
    if (CheckIO((struct IORequest *) io) != NULL) {
        WaitIO((struct IORequest *) io);
        return;
    }
    ad->chg_active = 1;
}

void
amigadev_change_int_off(amigadev_t *ad)
{
    struct IOStdReq *io;

    if (!ad->chg_active)
        return;
    io = &ad->chg_io->iotd_Req;
    io->io_Command = TD_REMCHANGEINT;
    DoIO((struct IORequest *) io);
    ad->chg_active = 0;
}

static void
detect_64bit(amigadev_t *ad)
{
    struct NSDeviceQueryResult nsd;
    struct IOStdReq *io = &ad->io->iotd_Req;
    UWORD *cmd;

    memset(&nsd, 0, sizeof (nsd));
    io->io_Command = NSCMD_DEVICEQUERY;
    io->io_Data    = &nsd;
    io->io_Length  = sizeof (nsd);
    ad->cmd64 = 1;              /* Assume TD64 unless NSD says otherwise */
    if ((DoIO((struct IORequest *) io) == 0) && (io->io_Actual >= 16) &&
        (nsd.nsdqr_SupportedCommands != NULL)) {
        for (cmd = nsd.nsdqr_SupportedCommands; *cmd != 0; cmd++)
            if (*cmd == NSCMD_TD_READ64)
                ad->cmd64 = 2;
    }
}

LONG
amigadev_open(amigadev_t *ad, struct FileSysStartupMsg *fssm,
              geometry_t *geo)
{
    struct DosEnvec *de = BADDR(fssm->fssm_Environ);
    UBYTE  *bname = BADDR(fssm->fssm_Device);
    char    devname[64];
    ULONG   table = de->de_TableSize;
    ULONG   sector;
    ULONG   blocks;
    uint64_t start;
    uint8_t shift = 0;
    unsigned len;

    memset(ad, 0, sizeof (*ad));
    sector = de->de_SizeBlock << 2;
    while ((1UL << shift) < sector)
        shift++;
    if (((1UL << shift) != sector) || (sector < 256) || (sector > 4096))
        return (ERROR_BAD_NUMBER);

    geo->block_size       = sector;
    geo->heads            = de->de_Surfaces;
    geo->blocks_per_track = de->de_BlocksPerTrack;
    geo->low_cyl          = de->de_LowCyl;
    geo->high_cyl         = de->de_HighCyl;
    geo->buffers          = de->de_NumBuffers;

    blocks = (de->de_HighCyl - de->de_LowCyl + 1) * de->de_Surfaces *
             de->de_BlocksPerTrack;
    start  = (uint64_t) de->de_LowCyl * de->de_Surfaces *
             de->de_BlocksPerTrack << shift;

    ad->bd.sector_size  = sector;
    ad->bd.sector_count = blocks;
    ad->bd.read  = ad_read;
    ad->bd.write = ad_write;
    ad->bd.sync  = ad_sync;
    ad->start_hi = (uint32_t) (start >> 32);
    ad->start_lo = (uint32_t) start;
    ad->block_shift  = shift;
    ad->memtype      = (table >= DE_MEMBUFTYPE) ? de->de_BufMemType :
                                                  MEMF_PUBLIC;
    ad->max_transfer = (table >= DE_MAXTRANSFER) ? de->de_MaxTransfer :
                                                   0x7fffffff;
    ad->mask         = (table >= DE_MASK) ? de->de_Mask : 0xfffffffe;
    if (ad->max_transfer < sector)
        ad->max_transfer = sector;

    len = bname[0];
    if (len >= sizeof (devname))
        len = sizeof (devname) - 1;
    memcpy(devname, bname + 1, len);
    devname[len] = '\0';

    ad->port = CreateMsgPort();
    if (ad->port == NULL)
        goto nomem;
    ad->io = (struct IOExtTD *) CreateIORequest(ad->port,
                                                sizeof (struct IOExtTD));
    ad->chg_io = (struct IOExtTD *) CreateIORequest(ad->port,
                                                    sizeof (struct IOExtTD));
    if ((ad->io == NULL) || (ad->chg_io == NULL))
        goto nomem;

    ad->bounce_size = BOUNCE_BLOCKS << shift;
    if (ad->bounce_size > ad->max_transfer)
        ad->bounce_size = (ad->max_transfer >> shift) << shift;
    ad->bounce = AllocMem(ad->bounce_size, ad->memtype | MEMF_PUBLIC);
    if ((ad->bounce != NULL) && ((ULONG) ad->bounce & ~ad->mask)) {
        /* Not reachable by the device's DMA; chip RAM always is */
        FreeMem(ad->bounce, ad->bounce_size);
        ad->bounce = AllocMem(ad->bounce_size, MEMF_CHIP | MEMF_PUBLIC);
    }
    if (ad->bounce == NULL)
        goto nomem;

    if (OpenDevice((STRPTR) devname, fssm->fssm_Unit,
                   (struct IORequest *) ad->io, fssm->fssm_Flags) != 0) {
        amigadev_close(ad);
        return (ERROR_DEVICE_NOT_MOUNTED);
    }
    ad->opened = 1;
    /*
     * Only trackdisk.device is known to handle the ETD_ commands.  Other
     * devices may not reject unknown commands safely.
     */
    ad->is_trackdisk = (strcmp(devname, "trackdisk.device") == 0);
    ad->use_etd = ad->is_trackdisk;
    if ((start + ((uint64_t) blocks << shift)) > 0xffffffffULL)
        detect_64bit(ad);
    return (0);

nomem:
    amigadev_close(ad);
    return (ERROR_NO_FREE_STORE);
}

void
amigadev_close(amigadev_t *ad)
{
    amigadev_change_int_off(ad);
    if (ad->opened)
        CloseDevice((struct IORequest *) ad->io);
    ad->opened = 0;
    if (ad->bounce != NULL)
        FreeMem(ad->bounce, ad->bounce_size);
    ad->bounce = NULL;
    if (ad->chg_io != NULL)
        DeleteIORequest((struct IORequest *) ad->chg_io);
    if (ad->io != NULL)
        DeleteIORequest((struct IORequest *) ad->io);
    if (ad->port != NULL)
        DeleteMsgPort(ad->port);
    ad->chg_io = NULL;
    ad->io     = NULL;
    ad->port   = NULL;
}
