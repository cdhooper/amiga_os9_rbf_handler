/*
 * Handler support: platform services for the filesystem core, the small
 * part of the C library the handler needs, and conversions between
 * AmigaDOS and filesystem conventions.
 */

#include <string.h>
#include <stdarg.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "handler.h"
#include "printf.h"

/* ------------------------------------------------------------------ */
/* C library subset (the handler links no C library)                  */
/* ------------------------------------------------------------------ */

void *
memcpy(void *dst, const void *src, size_t len)
{
    uint8_t       *d = dst;
    const uint8_t *s = src;

    if ((((ULONG) d | (ULONG) s) & 1) == 0) {
        while (len >= 4) {
            *(uint32_t *) d = *(const uint32_t *) s;
            d += 4;
            s += 4;
            len -= 4;
        }
    }
    while (len-- > 0)
        *d++ = *s++;
    return (dst);
}

void *
memmove(void *dst, const void *src, size_t len)
{
    uint8_t       *d = dst;
    const uint8_t *s = src;

    if ((d <= s) || (d >= s + len))
        return (memcpy(dst, src, len));
    d += len;
    s += len;
    while (len-- > 0)
        *--d = *--s;
    return (dst);
}

void *
memset(void *dst, int c, size_t len)
{
    uint8_t *d = dst;

    if ((((ULONG) d & 1) == 0) && (len >= 8)) {
        uint32_t v = (uint8_t) c;
        v |= v << 8;
        v |= v << 16;
        while (len >= 4) {
            *(uint32_t *) d = v;
            d += 4;
            len -= 4;
        }
    }
    while (len-- > 0)
        *d++ = c;
    return (dst);
}

int
memcmp(const void *a, const void *b, size_t len)
{
    const uint8_t *p = a;
    const uint8_t *q = b;

    for (; len > 0; len--, p++, q++)
        if (*p != *q)
            return (*p - *q);
    return (0);
}

size_t
strlen(const char *s)
{
    const char *p = s;
    while (*p != '\0')
        p++;
    return (p - s);
}

char *
strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return (dst);
}

int
strcmp(const char *a, const char *b)
{
    while ((*a != '\0') && (*a == *b)) {
        a++;
        b++;
    }
    return ((unsigned char) *a - (unsigned char) *b);
}

char *
strchr(const char *s, int c)
{
    for (; *s != '\0'; s++)
        if (*s == (char) c)
            return ((char *) s);
    return ((c == '\0') ? (char *) s : NULL);
}

/* ------------------------------------------------------------------ */
/* Platform services for the filesystem core                          */
/* ------------------------------------------------------------------ */

void *
fsp_alloc(uint32_t size)
{
    return (AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR));
}

void
fsp_free(void *ptr, uint32_t size)
{
    if (ptr != NULL)
        FreeMem(ptr, size);
}

uint32_t
fsp_now(void)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return (datestamp_to_fs_time(&ds));
}

#ifdef DEBUG
void
debugf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
#endif

void
fsp_log(const char *fmt, ...)
{
#ifdef DEBUG
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
#else
    (void) fmt;
#endif
}

/* ------------------------------------------------------------------ */
/* Conversions                                                        */
/* ------------------------------------------------------------------ */

LONG
fs_error_to_dos(int err)
{
    switch (err) {
        case FS_OK:         return (0);
        case FS_ENOENT:     return (ERROR_OBJECT_NOT_FOUND);
        case FS_EEXIST:     return (ERROR_OBJECT_EXISTS);
        case FS_ENOTDIR:    return (ERROR_OBJECT_WRONG_TYPE);
        case FS_EISDIR:     return (ERROR_OBJECT_WRONG_TYPE);
        case FS_ENOTEMPTY:  return (ERROR_DIRECTORY_NOT_EMPTY);
        case FS_ENOSPC:     return (ERROR_DISK_FULL);
        case FS_EFBIG:      return (ERROR_DISK_FULL);
        case FS_EROFS:      return (ERROR_DISK_WRITE_PROTECTED);
        case FS_EBADNAME:   return (ERROR_INVALID_COMPONENT_NAME);
        case FS_ENOMEM:     return (ERROR_NO_FREE_STORE);
        case FS_EBUSY:      return (ERROR_OBJECT_IN_USE);
        case FS_ELOOP:      return (ERROR_OBJECT_IN_USE);
        case FS_ERANGE:     return (ERROR_SEEK_ERROR);
        case FS_EINVAL:     return (ERROR_BAD_NUMBER);
        case FS_ENOTFS:     return (ERROR_NOT_A_DOS_DISK);
        case FS_ESECTSIZE:  return (ERROR_NOT_A_DOS_DISK);
        case FS_ECORRUPT:   return (ERROR_NOT_A_DOS_DISK);
        case FS_EIO:
        default:            return (ERROR_SEEK_ERROR);
    }
}

/*
 * OS-9 attributes to AmigaDOS protection bits.  Owner RWED are active
 * low; group and other bits are active high.  OS-9 has a single write
 * permission, which AmigaDOS shows as both W and D.
 */
ULONG
attr_to_protection(uint16_t attr)
{
    ULONG prot = 0;

    if ((attr & FS_ATTR_READ) == 0)
        prot |= FIBF_READ;
    if ((attr & FS_ATTR_WRITE) == 0)
        prot |= FIBF_WRITE | FIBF_DELETE;
    if ((attr & FS_ATTR_EXEC) == 0)
        prot |= FIBF_EXECUTE;
    if (attr & FS_ATTR_PREAD)
        prot |= FIBF_GRP_READ | FIBF_OTR_READ;
    if (attr & FS_ATTR_PWRITE)
        prot |= FIBF_GRP_WRITE | FIBF_GRP_DELETE |
                FIBF_OTR_WRITE | FIBF_OTR_DELETE;
    if (attr & FS_ATTR_PEXEC)
        prot |= FIBF_GRP_EXECUTE | FIBF_OTR_EXECUTE;
    return (prot);
}

/*
 * AmigaDOS protection bits to OS-9 attributes.  The non-sharable and
 * directory bits are kept; A, P, S and H have nowhere to go.
 */
uint16_t
protection_to_attr(ULONG prot, uint16_t old)
{
    uint16_t attr = old & (FS_ATTR_NONSHARE | FS_ATTR_DIR);

    if ((prot & FIBF_READ) == 0)
        attr |= FS_ATTR_READ;
    if ((prot & FIBF_WRITE) == 0)
        attr |= FS_ATTR_WRITE;
    if ((prot & FIBF_EXECUTE) == 0)
        attr |= FS_ATTR_EXEC;
    if (prot & (FIBF_GRP_READ | FIBF_OTR_READ))
        attr |= FS_ATTR_PREAD;
    if (prot & (FIBF_GRP_WRITE | FIBF_OTR_WRITE))
        attr |= FS_ATTR_PWRITE;
    if (prot & (FIBF_GRP_EXECUTE | FIBF_OTR_EXECUTE))
        attr |= FS_ATTR_PEXEC;
    return (attr);
}

/* Both count local time from 1978-01-01 */
void
fs_time_to_datestamp(fs_time_t t, struct DateStamp *ds)
{
    ds->ds_Days   = t / 86400;
    ds->ds_Minute = (t % 86400) / 60;
    ds->ds_Tick   = (t % 60) * TICKS_PER_SECOND;
}

fs_time_t
datestamp_to_fs_time(const struct DateStamp *ds)
{
    return (ds->ds_Days * 86400 + ds->ds_Minute * 60 +
            ds->ds_Tick / TICKS_PER_SECOND);
}

void
bstr_to_cstr(BSTR b, char *buf, int size)
{
    const UBYTE *p = BADDR(b);
    int          len = 0;

    if (p != NULL) {
        len = p[0];
        if (len >= size)
            len = size - 1;
        memcpy(buf, p + 1, len);
    }
    buf[len] = '\0';
}

/* Writes a counted string which is also NUL terminated */
void
cstr_to_bstr(const char *s, UBYTE *b, int size)
{
    int len = strlen(s);

    if (len > size - 2)
        len = size - 2;
    b[0] = len;
    memcpy(b + 1, s, len);
    b[len + 1] = '\0';
}
