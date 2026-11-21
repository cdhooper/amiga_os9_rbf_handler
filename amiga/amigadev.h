/*
 * Block device on a trackdisk-style exec device partition
 */

#ifndef _AMIGADEV_H
#define _AMIGADEV_H

#include <exec/types.h>
#include <exec/interrupts.h>
#include <devices/trackdisk.h>
#include <dos/filehandler.h>
#include "blockdev.h"

typedef struct amigadev {
    blockdev_t        bd;           /* Must be first */
    struct MsgPort   *port;
    struct IOExtTD   *io;
    struct IOExtTD   *chg_io;       /* Owned by device while change int on */
    struct Interrupt  chg_int;
    struct Task      *chg_task;     /* Signalled on disk change */
    ULONG             chg_mask;
    uint32_t          start_hi;     /* Partition start, bytes (64-bit) */
    uint32_t          start_lo;
    uint8_t           block_shift;
    uint8_t           cmd64;        /* 0 = 32-bit, 1 = TD64, 2 = NSD */
    uint8_t           opened;
    uint8_t           chg_active;
    uint8_t           is_trackdisk;
    uint8_t           use_etd;      /* ETD_ commands guard disk changes */
    uint8_t           offline;      /* Refuse all block I/O */
    ULONG             etd_count;    /* Change count the I/O is valid for */
    uint32_t          max_transfer; /* Bytes per I/O request */
    uint32_t          mask;         /* DMA address mask */
    uint32_t          memtype;      /* Buffer memory type */
    uint8_t          *bounce;       /* DMA-safe buffer */
    uint32_t          bounce_size;
    uint32_t          errors;
} amigadev_t;

/* Device geometry taken from the DosEnvec */
typedef struct geometry {
    uint32_t block_size;
    uint32_t heads;
    uint32_t blocks_per_track;
    uint32_t low_cyl;
    uint32_t high_cyl;
    uint32_t buffers;
} geometry_t;

LONG amigadev_open(amigadev_t *ad, struct FileSysStartupMsg *fssm,
                   geometry_t *geo);
void amigadev_close(amigadev_t *ad);
int  amigadev_disk_present(amigadev_t *ad);
int  amigadev_write_protected(amigadev_t *ad);
void amigadev_motor_off(amigadev_t *ad);
int  amigadev_change_count(amigadev_t *ad, ULONG *count);
void amigadev_change_int_on(amigadev_t *ad, struct Task *task, ULONG sigmask);
void amigadev_change_int_off(amigadev_t *ad);

#endif /* _AMIGADEV_H */
