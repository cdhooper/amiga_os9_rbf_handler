#!/usr/bin/env python3
#
# mkmbr.py - build a PC-partitioned disk image for the host tests
#
# usage: mkmbr.py <out> <part> [<part>...]
#   part: <type-hex>:<file>   partition holding the file's contents
#         <type-hex>:@<bytes>  partition of that many zero bytes
#
# Partitions start at sector 63, one after another, each a whole number
# of 512-byte sectors; the first is marked bootable.  The MBR's boot
# code area and the gap before the first partition are random, as on
# a real disk, so nothing there looks like a filesystem.
#
import os
import struct
import sys

out, parts = sys.argv[1], sys.argv[2:]
if not 1 <= len(parts) <= 4:
    sys.exit("usage: mkmbr.py <out> <type:file|type:@bytes>...")
body = []
for p in parts:
    t, src = p.split(":", 1)
    data = (bytes(int(src[1:])) if src.startswith("@")
            else open(src, "rb").read())
    data += bytes(-len(data) % 512)
    body.append((int(t, 16), data))

mbr = bytearray(os.urandom(446)) + bytes(66)
lba = 63
for i, (t, data) in enumerate(body):
    struct.pack_into("<BBBBBBBBII", mbr, 446 + 16 * i,
                     0x80 if i == 0 else 0, 0, 0, 0, t, 0, 0, 0,
                     lba, len(data) // 512)
    lba += len(data) // 512
mbr[510:512] = b"\x55\xaa"
with open(out, "wb") as f:
    f.write(mbr)
    f.write(os.urandom(62 * 512))
    for _, data in body:
        f.write(data)
    f.write(bytes(4096))        # Space after the last partition
