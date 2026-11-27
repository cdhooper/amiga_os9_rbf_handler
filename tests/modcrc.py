#!/usr/bin/env python3
"""
Reads every file of an OS-9/6809 image through rbftool and checks the
CRC of each 6809 memory module found at the start of a file (modules
may be concatenated, as in OS9Boot).

usage: modcrc.py <rbftool> <image>
"""

import os
import subprocess
import sys
import tempfile

RBFTOOL, IMG = sys.argv[1], sys.argv[2]

# OS-9 module CRC: CRC-24, polynomial 0x800063, preset 0xffffff.  Over a
# whole module, its CRC bytes included, it leaves 0x800fe3.
TABLE = []
for i in range(256):
    c = i << 16
    for _ in range(8):
        c <<= 1
        if c & 0x1000000:
            c ^= 0x1800063
    TABLE.append(c & 0xffffff)


def crc24(data):
    crc = 0xffffff
    for b in data:
        crc = ((crc << 8) & 0xffffff) ^ TABLE[b ^ (crc >> 16)]
    return crc


def tool(*args):
    return subprocess.run([RBFTOOL, '-r', '-s', '256', IMG] + list(args),
                          capture_output=True, text=True, check=True).stdout


def walk(d, tmp):
    for line in tool('ls', d).splitlines():
        if not line.strip():
            continue
        name = line.split()[-1]
        path = (d + '/' + name.rstrip('/')).lstrip('/')
        if name.endswith('/'):
            yield from walk(path, tmp)
        else:
            host = os.path.join(tmp, 'f')
            tool('get', path, host)
            with open(host, 'rb') as f:
                yield path, f.read()


def main():
    files = mods = bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for path, data in walk('', tmp):
            files += 1
            o = 0
            while (o + 9 < len(data) and data[o] == 0x87 and
                   data[o + 1] == 0xcd):
                size = (data[o + 2] << 8) | data[o + 3]
                mods += 1
                if size < 9 or o + size > len(data) or \
                        crc24(data[o:o + size]) != 0x800fe3:
                    bad += 1
                    print('bad module in %s at offset %d' % (path, o))
                    break
                o += size
    print('%d files, %d modules, %d bad' % (files, mods, bad))


if __name__ == '__main__':
    main()
