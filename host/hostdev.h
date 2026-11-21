/*
 * Image file block device for host tools
 */

#ifndef _HOSTDEV_H
#define _HOSTDEV_H

#include <stdint.h>
#include "blockdev.h"

typedef struct hostdev {
    blockdev_t dev;             /* Must be first */
    int        fd;
    uint64_t   offset;          /* Byte offset of LSN 0 in the file */
    uint32_t   reads;
    uint32_t   writes;
} hostdev_t;

/* sectors == 0 means the rest of the file */
int  hostdev_open(hostdev_t *hd, const char *path, uint32_t sector_size,
                  uint64_t offset, uint32_t sectors, int read_only);
void hostdev_close(hostdev_t *hd);

extern int      hostdev_verbose;
extern uint32_t hostdev_fixed_time;

#endif /* _HOSTDEV_H */
