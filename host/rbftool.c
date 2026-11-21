/*
 * rbftool - exercise the filesystem core on an image file
 *
 * usage: rbftool [options] <image> <command> [args...]
 *        rbftool [options] <image> script [<file>]   (commands from file
 *                                                     or stdin)
 *
 * Options:
 *   -s <bytes>   sector size (default 512)
 *   -o <bytes>   byte offset of the filesystem in the image
 *   -n <sectors> filesystem size (default: rest of the image)
 *   -b <bufs>    cache buffers (default 32)
 *   -r           read-only
 *   -t <secs>    use a fixed "now" (fs_time_t) for reproducible images
 *   -v           log core diagnostics
 *   -f <type>    filesystem to format: os9 (default), os9k-be, os9k-le
 *
 * Commands (paths are relative to the root, '/' separated):
 *   format <name> [cluster]     write an empty filesystem
 *   info | free
 *   ls [dir] | stat <path> | cat <path>
 *   get <path> <hostfile>       put <hostfile> <path>
 *   append <hostfile> <path>    write <path> <offset> <hostfile>
 *   truncate <path> <size>      mkdir <path>      rm <path>
 *   mv <from> <to>              chmod <path> <hexattr>
 *   chown <path> <group>.<user> touch <path> <YYYY-MM-DD> <HH:MM>
 *   relabel <name>              sync | remount
 *
 * In script mode each command prints "OK" or "ERR <message>" after its
 * output, and execution continues.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "fs_api.h"
#include "hostdev.h"

static hostdev_t    hdev;
static fs_volume_t *vol;
static unsigned     cache_bufs = 32;
static int          script_mode;
static uint8_t      fs_type = FS_FMT_OS9;

static int
do_mount(void)
{
    int rc = fs_mount(&hdev.dev, cache_bufs, &vol);
    if (rc != FS_OK)
        fprintf(stderr, "mount: %s\n", fs_strerror(rc));
    return (rc);
}

/*
 * Resolves a path.  With want_parent, returns the directory which holds
 * the last component and a pointer to that component.
 */
static int
resolve(const char *path, int want_parent, fs_stat_t *st,
        fs_node_t *parentp, const char **lastp)
{
    char       buf[1024];
    char      *p;
    char      *slash;
    fs_node_t  dir = fs_root(vol);
    int        rc;

    while (*path == '/')
        path++;
    if (strlen(path) >= sizeof (buf))
        return (FS_EINVAL);
    strcpy(buf, path);
    p = buf;
    while ((slash = strchr(p, '/')) != NULL) {
        *slash = '\0';
        rc = fs_lookup(vol, dir, p, st);
        if (rc != FS_OK)
            return (rc);
        if (st->type != FS_TYPE_DIR)
            return (FS_ENOTDIR);
        dir = st->node;
        p = slash + 1;
    }
    if (want_parent) {
        *parentp = dir;
        *lastp = path + (p - buf);
        return (FS_OK);
    }
    if (*p == '\0')
        return (fs_stat(vol, FS_NODE_NONE, dir, st));
    rc = fs_lookup(vol, dir, p, st);
    if (parentp != NULL)
        *parentp = dir;
    return (rc);
}

static void
print_time(fs_time_t t)
{
    fs_tm_t tm;
    fs_gmtime(t, &tm);
    printf("%04u-%02u-%02u %02u:%02u", tm.year, tm.month, tm.day, tm.hour,
           tm.min);
}

static void
print_attr(uint16_t a)
{
    const char *s = "dsewrewr";
    int i;
    for (i = 7; i >= 0; i--)
        putchar((a & (1 << i)) ? s[7 - i] : '-');
}

static void
print_stat(const fs_stat_t *st)
{
    print_attr(st->attr);
    printf(" %3u.%-3u ", st->owner_group, st->owner_user);
    print_time(st->mtime);
    printf(" %6lu %9lu %s%s\n", (unsigned long) st->node,
           (unsigned long) st->size, st->name,
           (st->type == FS_TYPE_DIR) ? "/" : "");
}

static int
cmd_info(void)
{
    fs_volinfo_t vi;
    int rc = fs_volinfo(vol, &vi);
    if (rc != FS_OK)
        return (rc);
    printf("type     %s\n", (vi.fs_type == FS_FMT_OS9K_LE) ?
           "OS-9000 little-endian" : (vi.fs_type == FS_FMT_OS9K_BE) ?
           "OS-9000 big-endian" : "OS-9/68k");
    printf("name     %s\n", vi.name);
    printf("sectors  %lu x %lu bytes\n", (unsigned long) vi.total_blocks,
           (unsigned long) vi.block_size);
    printf("free     %lu\n", (unsigned long) vi.free_blocks);
    printf("disk id  %04lx\n", (unsigned long) vi.disk_id);
    printf("name max %u\n", vi.name_max);
    printf("created  ");
    print_time(vi.created);
    printf("\n");
    return (FS_OK);
}

static int
cmd_free(void)
{
    fs_volinfo_t vi;
    int rc = fs_volinfo(vol, &vi);
    if (rc == FS_OK)
        printf("%lu\n", (unsigned long) vi.free_blocks);
    return (rc);
}

static int
cmd_ls(const char *path)
{
    fs_stat_t st;
    uint32_t  cookie = 0;
    fs_node_t dir;
    int       rc;

    rc = resolve(path ? path : "", 0, &st, NULL, NULL);
    if (rc != FS_OK)
        return (rc);
    if (st.type != FS_TYPE_DIR) {
        print_stat(&st);
        return (FS_OK);
    }
    dir = st.node;
    while ((rc = fs_readdir(vol, dir, &cookie, &st)) == FS_OK)
        print_stat(&st);
    return ((rc == FS_ENOENT) ? FS_OK : rc);
}

static int
cmd_stat(const char *path)
{
    fs_stat_t st;
    int rc = resolve(path, 0, &st, NULL, NULL);
    if (rc != FS_OK)
        return (rc);
    print_stat(&st);
    printf("links %u blocks %lu created ", st.links,
           (unsigned long) st.blocks);
    print_time(st.ctime);
    printf("\n");
    return (FS_OK);
}

static int
copy_out(const char *path, FILE *out)
{
    fs_stat_t  st;
    fs_file_t *f;
    char       buf[7000];   /* Not a sector multiple, on purpose */
    uint32_t   got;
    int        rc;

    rc = resolve(path, 0, &st, NULL, NULL);
    if (rc != FS_OK)
        return (rc);
    if (st.type == FS_TYPE_DIR)
        return (FS_EISDIR);
    rc = fs_open(vol, st.node, FS_OPEN_READ, &f);
    if (rc != FS_OK)
        return (rc);
    while (((rc = fs_read(f, buf, sizeof (buf), &got)) == FS_OK) &&
           (got > 0))
        fwrite(buf, 1, got, out);
    fs_close(f);
    return (rc);
}

static int
cmd_get(const char *path, const char *hostfile)
{
    FILE *out = fopen(hostfile, "wb");
    int   rc;

    if (out == NULL)
        return (FS_EIO);
    rc = copy_out(path, out);
    fclose(out);
    return (rc);
}

/* Opens path for writing, creating it if needed */
static int
open_for_write(const char *path, int truncate, fs_file_t **fp)
{
    fs_stat_t   st;
    fs_node_t   dir;
    const char *last;
    int         rc;

    rc = resolve(path, 1, &st, &dir, &last);
    if (rc != FS_OK)
        return (rc);
    rc = fs_lookup(vol, dir, last, &st);
    if (rc == FS_ENOENT)
        rc = fs_create(vol, dir, last, 0x1b, &st);
    if (rc != FS_OK)
        return (rc);
    if (st.type == FS_TYPE_DIR)
        return (FS_EISDIR);
    rc = fs_open(vol, st.node, FS_OPEN_READ | FS_OPEN_WRITE, fp);
    if ((rc == FS_OK) && truncate)
        rc = fs_truncate(*fp, 0);
    return (rc);
}

static int
copy_in(const char *hostfile, const char *path, int mode, long offset)
{
    FILE      *in = fopen(hostfile, "rb");
    fs_file_t *f;
    char       buf[5000];
    size_t     n;
    uint32_t   done;
    int        rc;

    if (in == NULL)
        return (FS_EIO);
    rc = open_for_write(path, mode == 0, &f);
    if (rc != FS_OK) {
        fclose(in);
        return (rc);
    }
    if (mode == 1)
        f->pos = fs_file_size(f);
    else if (mode == 2)
        f->pos = offset;
    while ((n = fread(buf, 1, sizeof (buf), in)) > 0) {
        rc = fs_write(f, buf, n, &done);
        if (rc != FS_OK)
            break;
    }
    fclose(in);
    {
        int rc2 = fs_close(f);
        if (rc == FS_OK)
            rc = rc2;
    }
    return (rc);
}

static int
cmd_truncate(const char *path, const char *size)
{
    fs_file_t *f;
    int        rc = open_for_write(path, 0, &f);
    int        rc2;

    if (rc != FS_OK)
        return (rc);
    rc  = fs_truncate(f, strtoul(size, NULL, 0));
    rc2 = fs_close(f);
    return ((rc != FS_OK) ? rc : rc2);
}

static int
cmd_mkdir(const char *path)
{
    fs_stat_t   st;
    fs_node_t   dir;
    const char *last;
    int rc = resolve(path, 1, &st, &dir, &last);
    if (rc != FS_OK)
        return (rc);
    return (fs_mkdir(vol, dir, last, 0xbf, &st));
}

static int
cmd_rm(const char *path)
{
    fs_stat_t   st;
    fs_node_t   dir;
    const char *last;
    int rc = resolve(path, 1, &st, &dir, &last);
    if (rc != FS_OK)
        return (rc);
    return (fs_remove(vol, dir, last));
}

static int
cmd_mv(const char *from, const char *to)
{
    fs_stat_t   st;
    fs_node_t   sdir, ddir;
    const char *slast, *dlast;
    int rc = resolve(from, 1, &st, &sdir, &slast);
    if (rc == FS_OK)
        rc = resolve(to, 1, &st, &ddir, &dlast);
    if (rc != FS_OK)
        return (rc);
    return (fs_rename(vol, sdir, slast, ddir, dlast));
}

static int
cmd_chmod(const char *path, const char *attr)
{
    fs_stat_t st;
    int rc = resolve(path, 0, &st, NULL, NULL);
    if (rc != FS_OK)
        return (rc);
    return (fs_setattr(vol, st.node, strtoul(attr, NULL, 16)));
}

static int
cmd_chown(const char *path, const char *owner)
{
    fs_stat_t    st;
    unsigned int g, u;
    int rc = resolve(path, 0, &st, NULL, NULL);
    if (rc != FS_OK)
        return (rc);
    if (sscanf(owner, "%u.%u", &g, &u) != 2)
        return (FS_EINVAL);
    return (fs_setowner(vol, st.node, g, u));
}

static int
cmd_touch(const char *path, const char *date, const char *time)
{
    fs_stat_t    st;
    fs_tm_t      tm;
    unsigned int y, mo, d, h, mi;
    int rc = resolve(path, 0, &st, NULL, NULL);
    if (rc != FS_OK)
        return (rc);
    if ((sscanf(date, "%u-%u-%u", &y, &mo, &d) != 3) ||
        (sscanf(time, "%u:%u", &h, &mi) != 2))
        return (FS_EINVAL);
    tm.year = y; tm.month = mo; tm.day = d; tm.hour = h; tm.min = mi;
    tm.sec = 0;
    return (fs_setmtime(vol, st.node, fs_mktime(&tm)));
}

static int
cmd_format(int argc, char **argv)
{
    fs_format_opts_t opts;

    memset(&opts, 0, sizeof (opts));
    opts.name = (argc > 1) ? argv[1] : "OS-9";
    opts.cluster = (argc > 2) ? strtoul(argv[2], NULL, 0) : 0;
    opts.sectors_per_track = 11;
    opts.heads = 2;
    opts.cylinders = 80;
    opts.fs_type = fs_type;
    return (fs_format(&hdev.dev, &opts));
}

#define NEED(n) do { if (argc < (n)) { rc = FS_EINVAL; goto bad; } } while (0)

static int
run_command(int argc, char **argv)
{
    const char *cmd = argv[0];
    int rc;

    if (strcmp(cmd, "info") == 0) {
        rc = cmd_info();
    } else if (strcmp(cmd, "free") == 0) {
        rc = cmd_free();
    } else if (strcmp(cmd, "ls") == 0) {
        rc = cmd_ls((argc > 1) ? argv[1] : NULL);
    } else if (strcmp(cmd, "stat") == 0) {
        NEED(2); rc = cmd_stat(argv[1]);
    } else if (strcmp(cmd, "cat") == 0) {
        NEED(2); rc = copy_out(argv[1], stdout);
    } else if (strcmp(cmd, "get") == 0) {
        NEED(3); rc = cmd_get(argv[1], argv[2]);
    } else if (strcmp(cmd, "put") == 0) {
        NEED(3); rc = copy_in(argv[1], argv[2], 0, 0);
    } else if (strcmp(cmd, "append") == 0) {
        NEED(3); rc = copy_in(argv[1], argv[2], 1, 0);
    } else if (strcmp(cmd, "write") == 0) {
        NEED(4); rc = copy_in(argv[3], argv[1], 2, strtol(argv[2], NULL, 0));
    } else if (strcmp(cmd, "truncate") == 0) {
        NEED(3); rc = cmd_truncate(argv[1], argv[2]);
    } else if (strcmp(cmd, "mkdir") == 0) {
        NEED(2); rc = cmd_mkdir(argv[1]);
    } else if (strcmp(cmd, "rm") == 0) {
        NEED(2); rc = cmd_rm(argv[1]);
    } else if (strcmp(cmd, "mv") == 0) {
        NEED(3); rc = cmd_mv(argv[1], argv[2]);
    } else if (strcmp(cmd, "chmod") == 0) {
        NEED(3); rc = cmd_chmod(argv[1], argv[2]);
    } else if (strcmp(cmd, "chown") == 0) {
        NEED(3); rc = cmd_chown(argv[1], argv[2]);
    } else if (strcmp(cmd, "touch") == 0) {
        NEED(4); rc = cmd_touch(argv[1], argv[2], argv[3]);
    } else if (strcmp(cmd, "relabel") == 0) {
        NEED(2); rc = fs_relabel(vol, argv[1]);
    } else if (strcmp(cmd, "sync") == 0) {
        rc = fs_sync(vol);
    } else if (strcmp(cmd, "remount") == 0) {
        rc = fs_unmount(vol, 0);
        vol = NULL;
        if (rc == FS_OK)
            rc = do_mount();
    } else {
        fprintf(stderr, "unknown command %s\n", cmd);
        return (FS_EINVAL);
    }
bad:
    return (rc);
}

static int
run_script(FILE *in)
{
    char  line[2048];
    char *argv[16];
    int   failures = 0;

    while (fgets(line, sizeof (line), in) != NULL) {
        int   argc = 0;
        char *p = strtok(line, " \t\r\n");
        int   rc;

        while ((p != NULL) && (argc < 16)) {
            argv[argc++] = p;
            p = strtok(NULL, " \t\r\n");
        }
        if ((argc == 0) || (argv[0][0] == '#'))
            continue;
        rc = run_command(argc, argv);
        if (rc == FS_OK) {
            printf("OK\n");
        } else {
            printf("ERR %s\n", fs_strerror(rc));
            failures++;
        }
        fflush(stdout);
        if (vol == NULL)
            break;      /* remount failed */
    }
    return (failures);
}

static void
usage(void)
{
    fprintf(stderr,
            "usage: rbftool [-s secsize] [-o offset] [-n sectors] [-b bufs]"
            " [-r] [-t time] [-v]\n"
            "               <image> <command> [args...]\n");
    exit(2);
}

int
main(int argc, char **argv)
{
    uint32_t sector_size = 512;
    uint64_t offset = 0;
    uint32_t sectors = 0;
    int      read_only = 0;
    int      rc;
    int      opt;

    while ((opt = getopt(argc, argv, "s:o:n:b:rt:vf:")) != -1) {
        switch (opt) {
            case 's': sector_size = strtoul(optarg, NULL, 0); break;
            case 'o': offset = strtoull(optarg, NULL, 0); break;
            case 'n': sectors = strtoul(optarg, NULL, 0); break;
            case 'b': cache_bufs = strtoul(optarg, NULL, 0); break;
            case 'r': read_only = 1; break;
            case 't': hostdev_fixed_time = strtoul(optarg, NULL, 0); break;
            case 'v': hostdev_verbose = 1; break;
            case 'f':
                if (strcmp(optarg, "os9") == 0)
                    fs_type = FS_FMT_OS9;
                else if (strcmp(optarg, "os9k-be") == 0)
                    fs_type = FS_FMT_OS9K_BE;
                else if (strcmp(optarg, "os9k-le") == 0)
                    fs_type = FS_FMT_OS9K_LE;
                else
                    usage();
                break;
            default:  usage();
        }
    }
    if (argc - optind < 2)
        usage();
    if (hostdev_open(&hdev, argv[optind], sector_size, offset, sectors,
                     read_only) != 0) {
        fprintf(stderr, "%s: %s\n", argv[optind], strerror(errno));
        return (1);
    }
    argv += optind + 1;
    argc -= optind + 1;

    if (strcmp(argv[0], "format") == 0) {
        rc = cmd_format(argc, argv);
        if (rc != FS_OK)
            fprintf(stderr, "format: %s\n", fs_strerror(rc));
        hostdev_close(&hdev);
        return (rc != FS_OK);
    }

    if (do_mount() != FS_OK)
        return (1);

    if (strcmp(argv[0], "script") == 0) {
        FILE *in = stdin;
        script_mode = 1;
        if ((argc > 1) && ((in = fopen(argv[1], "r")) == NULL)) {
            perror(argv[1]);
            return (1);
        }
        rc = run_script(in);
        if (in != stdin)
            fclose(in);
    } else {
        rc = run_command(argc, argv);
        if (rc != FS_OK)
            fprintf(stderr, "%s: %s\n", argv[0], fs_strerror(rc));
    }
    if (vol != NULL) {
        int rc2 = fs_unmount(vol, 0);
        if (rc2 != FS_OK) {
            fprintf(stderr, "unmount: %s\n", fs_strerror(rc2));
            rc = 1;
        }
    }
    hostdev_close(&hdev);
    return (rc != 0);
}
