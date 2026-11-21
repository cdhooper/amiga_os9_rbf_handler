# os9-rbf-handler: OS-9 RBF filesystem for AmigaOS

An AmigaOS filesystem handler for OS-9 RBF disks: floppies and hard disk
partitions written by OS-9/68k or by OS-9000 (big- or little-endian, e.g.
x86 OS-9000 disks) can be read and written from AmigaOS, and AmigaOS can
format new ones in any of the three formats. The RBF code is a C
reimplementation of the OS-9/68k RBF file manager (edition 104) and an
independent implementation of the OS-9000 RBF disk format, structured as a
portable core that the handler links statically. One handler serves both;
the format of a disk is detected when it is mounted.

Requirements: Kickstart/Workbench 2.04 or later, any 68000+ CPU, and a
trackdisk-style exec device (trackdisk.device, scsi.device, uaehf.device, ...).

## Layout

| Path | Contents |
|---|---|
| `core9/fs_api.[ch]` | Filesystem API used by the handler; backend selection |
| `core9/blockdev.h` | Block device and platform service interface |
| `core9/cache.[ch]` | Write-back sector cache (LRU) |
| `core9/blockmap.[ch]` | Filesystem block size different from the device's |
| `core9/rbf68k.h` | OS-9/68k on-disk layout and internal interfaces |
| `core9/rbf68k_vol.c` | Mount, format, allocation bitmap, dates |
| `core9/rbf68k_node.c` | File descriptors and segment lists (grow, trim, map) |
| `core9/rbf68k_file.c` | File data I/O |
| `core9/rbf68k_dir.c` | Names, directories, create/remove/rename, attributes |
| `core9k/rbf9k.h` | OS-9000 on-disk layout and internal interfaces |
| `core9k/rbf9k_vol.c` | Mount, format, byte order, allocation bitmap, times |
| `core9k/rbf9k_node.c` | FD rings, segment lists, small files |
| `core9k/rbf9k_file.c` | File data I/O |
| `core9k/rbf9k_dir.c` | Names, directories, create/remove/rename, attributes |
| `amiga/start.c` | Handler entry point (must link first) |
| `amiga/handler.c` | Startup packet, main loop, write-back timer |
| `amiga/packets.c` | DOS packet actions, path resolution |
| `amiga/volume.c` | Volume nodes, locks, disk change, inhibit |
| `amiga/amigadev.c` | Exec device block I/O (MaxTransfer, Mask, TD64/NSD) |
| `amiga/support.c` | C library subset, protection/date/error mapping |
| `amiga/printf.c` | Debug printf (serial), from smashfs |
| `host/` | `rbftool` (core on an image file), `rbfcheck` (dcheck equivalent; `rbfcheck9k.c` for OS-9000) |
| `tests/` | Host test suite |

## Building

    make                # objs/os9-rbf-handler and the host tools
    make handler        # Amiga handler only (/opt/amiga162rc25 gcc)
    make DEBUG=1 OBJDIR=objs.debug handler   # serial debug output
    make test           # host test suite

The handler is built with `-nostdlib -nostartfiles`: no C library or startup
code is linked (integer division comes from libnix13). `amiga/start.c` must be
the first object; the build fails if `handler_entry` is not at offset 0.

## Installing

Copy `objs/os9-rbf-handler` to `L:`. A mountlist entry for an OS-9 floppy in DF0:

    OD0:
        Device         = trackdisk.device
        Unit           = 0
        Flags          = 0
        Surfaces       = 2
        BlocksPerTrack = 11
        SectorSize     = 512
        Reserved       = 0
        LowCyl         = 0
        HighCyl        = 79
        Buffers        = 20
        BufMemType     = 1
        StackSize      = 8192
        Priority       = 10
        GlobVec        = -1
        DosType        = 0x4F533900
        FileSystem     = L:os9-rbf-handler
        Activate       = 1
    #

For a hard disk partition, use the partition's device, unit, geometry and
cylinder range (the OS-9 partitions made by `tools/mkrdb.py` use 16 heads,
63 blocks per track). The RBF identification sector is the first block of
`LowCyl`; `Reserved` is ignored. `GlobVec = -1` and a `StackSize` of at
least 8192 are required.

For hard disks the usual way is to put the handler in the RDB as the
filesystem for DosType `OS9\0`; Kickstart then mounts OS-9 partitions at
boot. With amitools:

    rdbtool disk.hdf fsadd objs/os9-rbf-handler dostype=0x4F533900 version=1.0 \
            stack_size=8192 global_vec=0xffffffff

and give the partitions the automount flag (HDToolBox can do the same). For
OS-9000 partitions, add the same file again for DosTypes `0x4F533901` and/or
`0x4F533902` (see below).

### DosTypes and Format

| DosType | Format writes |
|---|---|
| `0x4F533900` `OS9\0` | OS-9/68k RBF |
| `0x4F533901` `OS9\1` (`ID_OS9K_DISK_BE`) | OS-9000 RBF, big-endian |
| `0x4F533902` `OS9\2` (`ID_OS9K_DISK_LE`) | OS-9000 RBF, little-endian |

The DosType only chooses what **Format** writes: the handler uses the type
Format sends if it is one of these, otherwise (e.g. when Format is given
`FFS` or `OFS`) the DosType of the mount entry or partition. Any other type
formats OS-9/68k. **Mounting ignores the DosType**: every disk is probed for
OS-9000 first (its ID block has a sync code), then for OS-9/68k, so a
partition of any of the three types mounts whatever RBF format it holds. The
volume node and `Info` report the type of the mounted disk.
Without a filesystem for `OS9\0` in the RDB, OS-9 partitions should not be
marked automount or bootable: Kickstart 3.2.2's A3000 `scsi.device` then
goes into a reboot loop (`mkrdb.py` currently sets both flags on the first
OS-9 partition).

The RBF sector size is the device block size (`SectorSize`/`BlockSize`):
256, 512, 1024, 2048 or 4096 bytes. A disk whose LSN 0 records a different
logical sector size is not mounted.

## How OS-9 files appear in AmigaOS

**Names**: OS-9 names are 1-28 characters (OS-9000: 1-43) from
`A-Z a-z 0-9 _ . $` with at least one letter or digit; matching ignores case. Creating or renaming to any
other name fails with "object name invalid". The OS-9 `.` and `..` entries
are not shown.

**Protection bits**:

| OS-9 | AmigaOS |
|---|---|
| `r` owner read | `R` |
| `w` owner write | `W` and `D` (OS-9 has one bit for both) |
| `e` owner execute | `E` |
| `pr` public read | group and other `R` |
| `pw` public write | group and other `W` and `D` |
| `pe` public execute | group and other `E` |
| `s` non-sharable | kept, not shown |
| `d` directory | entry type |

The AmigaOS `A`, `P`, `S` and `H` bits have nowhere to go and are dropped.
Files created from AmigaOS get `pe pr e w r`; directories get
`d pe pw pr e w r`. As in FFS, a file without `r` cannot be opened, and one
without `w` cannot be written or deleted.

**Owner**: the FD's group.user (one byte each) is shown as the GID and UID.

**Dates**: the FD's last-modified date (minute resolution) is the AmigaOS
date; both are local time. The creation date (date only) is set when the
file is created and is not visible from AmigaOS.

**Comments** are not stored; setting one succeeds and does nothing.

**Volume name** is the LSN 0 volume name. Relabel changes it; Format
writes a new filesystem using the mountlist geometry, choosing a cluster size
that keeps the bitmap within 64K bytes.

### OS-9000 disks

* **Byte order**: from the ID block's end flag (`B`/`L`); all numbers on the
  disk are converted as they are read and written, names and data are not.
  A disk without the flag is judged by its block size field.
* **Block size** is the ID block's (256-4096 bytes); if it differs from the
  device block size, I/O is mapped as for OS-9/68k.
* **Permissions**: the owner bits map to `RWED` as above, the world bits to
  the group and other bits. Protect from AmigaOS sets the OS-9000 group bits
  the same as the world bits.
* **Owner** is the FD's 16-bit group and user.
* **Dates**: the last-modified time (to the second). Times count from 1970
  (or 1980 on old volumes, by the ID block flag). New files get creation,
  access, modification and change times; closing a written file updates
  modification and change times.
* **Small files**: new files start as small files (data inside the FD block)
  and become ordinary files when they outgrow it. Files whose segment list
  outgrows one FD block continue in further FD blocks, linked into a ring.
* **Links** are Unix style: a directory's count includes its `.` and each
  subdirectory's `..`. Deleted directory entries keep their name shifted by
  one byte behind a zero, as OS-9000 does.
* **Format** writes the ID block at block 0, the bitmap FD at 1, the root
  directory FD at 2, then the bitmap and the root directory. Times count
  from 1970.
* Symbolic links appear as ordinary files holding the target path; they
  cannot be created from AmigaOS. The boot file fields of the ID block are
  kept but not maintained.
* PC disks: OS-9000 on x86 puts the ID block in the second sector of the
  partition (the first holds the boot loader), so the mount entry must start
  one block later than the partition.

## Design notes

* The handler is re-entrant: a filesystem loaded from the RDB or
  FileSystem.resource runs as one process per partition on the same code,
  so all state lives in a per-process `handler_t`; the only writable
  globals are `SysBase` and `DOSBase`.
* The core API (`fs_api.h`) identifies objects by node (the FD's LSN),
  iterates directories with a byte-position cookie, and knows nothing about
  AmigaDOS. The two backends sit behind `struct fs_ops`: `fs_mount()`
  probes OS-9000 (`core9k/`) and then OS-9/68k (`core9/`); `fs_format()`
  writes the one `fs_format_opts.fs_type` names.
* The OS-9000 backend was written from the on-disk format alone; it shares
  no code with the OS-9000 RBF file manager.
* Every user of an FD shares one in-memory node, so open files and directory
  operations always see the same size and segment list.
* Allocation follows RBF: whole clusters from the bitmap, preferring to
  extend the last segment in place, at least 8 sectors (PD_SAS) per
  allocation, trimmed back to the file size on close. A new file's FD takes
  the first sector of a cluster; the rest of that cluster becomes its first
  segment.
* Data writes are written back one second after the last write, or at once
  when a written file is closed. Directory changes are also written back
  after one second.
* Record locking, the FTL delete hook, and the uncommon GetStat/SetStat codes
  are not implemented. ExAll is left to DOS's emulation. Notification is not
  supported.
* On floppies, I/O uses trackdisk's `ETD_` commands with the change count
  from mount time, so nothing cached for one disk is written to another.
* When a disk is removed with locks or files still open, its volume stays in
  the DOS list without a handler; if the same disk (name, date and disk ID)
  comes back, its locks become usable again. Open files on a removed disk
  stay unusable until closed.

## Testing

Host tests (`make test`, or `tests/run_tests.sh quick`):

* images made by `mkos9fs.py` read back correctly;
* `tests/stress.py` runs thousands of random operations (create, write at
  random offsets, append, truncate, rename across directories, mkdir,
  remove, remount) against a Python model for each sector size and cluster
  sizes 1-8, reading everything back and running `rbfcheck` after each
  batch;
* filling a disk until it is full, then emptying it, returns all space.

`rbfcheck` is independent of the core. It checks LSN 0, the directory tree,
`.` and `..`, segment ranges, cluster ownership (no sharing), bitmap
consistency in both directions, and link counts. For OS-9000 images it also
checks the FD rings, and FD parity where the FD's revision field is 1 (as
OS-9000 RBF does; `os9format` writes revision 0 with meaningless parity).

OS-9000 (`rbftool -f os9k-be|os9k-le`, `stress.py --fs ...`): the stress and
disk-full tests run for both byte orders at 256-4096 byte blocks, and files
fragmented over several FD blocks are written, shrunk and removed. With
`OS9K_IMAGE=<image> OS9K_OFFSET=<bytes>` the suite also checks a real
OS-9000 disk and writes to a copy of it; an x86 OS-9000 hard disk image
(little-endian, 3360 objects) checks clean, its files read back identical to
an independent reader, and files written to it check clean.

With `OS9DCHECK=<path to os9dcheck>` (a Linux build of Microware's OS-9000
dcheck, e.g. `bin.linux/os9dcheck` in the tools tree), the fragmented-file
images, a stress run's final tree for each byte order, and the written copy
of `OS9K_IMAGE` are also checked by it; all are reported intact. It reads
both byte orders. Give it the image by a relative name: it crashes on a
path that starts with `/`.

Images written by the core also pass the real OS-9 `dcheck`, run in WinUAE
on the OS-9 ROM with the image on `/sc1` (512-byte sectors, 1- and
4-sector clusters). Files written by OS-9 read back correctly.

Under AmigaOS 3.2.2 (WinUAE A3000/030), the handler has run on
`uaehf.device` (from a mountlist and from the RDB) and on
`trackdisk.device` (an OS-9 floppy in DF0:). Tested commands: Info, List,
Type, Copy (including `Copy ALL`), MakeDir, Rename (also across
directories), Protect, SetDate, Delete (including `Delete ALL` with
delete-protected files), Relabel, Format, assigns, and filling a floppy
until it is full. The resulting images pass `rbfcheck` and OS-9 `dcheck`
(including a floppy formatted by AmigaOS Format), and the copied files are
byte-identical.

Not tested yet: Kickstart 2.04/3.1, physical disk changes, Workbench,
real hardware. On WinUAE's emulated A3000 `scsi.device` with Kickstart
3.2.2, mounting even the ROM FastFileSystem from a mountlist crashes in
exec's task switch, and the same crash appears with this handler, so that
combination could not be tested.
