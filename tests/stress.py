#!/usr/bin/env python3
"""
Randomized test of the filesystem core through rbftool.

A model of the expected tree (directories, file contents, attributes) is
kept in Python.  Random operations are sent to rbftool in script mode in
batches; after each batch the results are compared with the model, file
contents are read back, and rbfcheck must report a clean filesystem.

usage: stress.py [--sector N] [--cluster N] [--sectors N] [--ops N]
                 [--seed N] <workdir>
"""

import argparse
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, '..', 'host')
RBFTOOL = os.path.join(TOOLS, 'rbftool')
RBFCHECK = os.path.join(TOOLS, 'rbfcheck')

NAMECHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.$'


class Fail(Exception):
    pass


class Model:
    def __init__(self):
        self.dirs = {'': {}}         # path -> {lower name: real name}
        self.files = {}              # path -> bytearray

    @staticmethod
    def join(d, n):
        return n if d == '' else d + '/' + n

    def lookup(self, d, name):
        """Returns the existing spelling of name in dir d, or None"""
        return self.dirs[d].get(name.lower())

    def children(self, d):
        return [self.join(d, n) for n in self.dirs[d].values()]


NAME_MAX = 28                   # 29 for 6809, 43 for OS-9000 (set by main)


def fmt_opts(a):
    """Format options appended to the volume name"""
    return ',half' if a.half else ''


def check_args(a):
    """rbfcheck options for the image's layout"""
    if a.fs == '6809':
        return ['-H', str(a.sector)] if a.half else ['-s', '256', '-6']
    return ['-s', str(a.sector)]


def rand_name(rng):
    n = rng.choice([1, 2, 5, 8, 12, 20, 28, NAME_MAX])
    while True:
        s = ''.join(rng.choice(NAMECHARS) for _ in range(n))
        if any(c.isalnum() for c in s):
            return s


def rand_size(rng, ss):
    return rng.choice([0, 1, rng.randint(2, ss - 1), ss, ss + 1,
                       rng.randint(ss, 8 * ss), rng.randint(8 * ss, 40 * ss)])


class Tester:
    def __init__(self, args):
        self.a = args
        self.rng = random.Random(args.seed)
        self.work = args.workdir
        os.makedirs(self.work, exist_ok=True)
        self.img = args.image or os.path.join(self.work, 'test.img')
        if os.path.exists(os.path.join(self.work, 'log.txt')):
            os.unlink(os.path.join(self.work, 'log.txt'))
        self.model = Model()
        self.nfile = 0
        self.batch = []              # (command, expect_ok, on_result)

    def tool(self, *args, inp=None):
        cmd = [RBFTOOL, '-s', str(self.a.sector), '-t', '1500000000',
               '-b', str(self.a.buffers), '-f', self.a.fs]
        if self.a.offset:
            cmd += ['-o', str(self.a.offset), '-n', str(self.a.sectors)]
        cmd += [self.img] + list(args)
        return subprocess.run(cmd, input=inp, capture_output=True, text=True)

    def tmpfile(self, data):
        self.nfile += 1
        p = os.path.join(self.work, 'd%d' % (self.nfile % 400))
        with open(p, 'wb') as f:
            f.write(data)
        return p

    def check(self):
        r = subprocess.run([RBFCHECK] + check_args(self.a) +
                           ['-o', str(self.a.offset), self.img],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise Fail('rbfcheck:\n' + r.stdout + r.stderr)

    def queue(self, cmd, expect_ok, on_result=None):
        self.batch.append((cmd, expect_ok, on_result))

    def run_batch(self):
        script = '\n'.join(c for c, _, _ in self.batch) + '\n'
        with open(os.path.join(self.work, 'log.txt'), 'a') as f:
            f.write(script + '--\n')
        r = self.tool('script', inp=script)
        results = [l for l in r.stdout.splitlines()
                   if l == 'OK' or l.startswith('ERR')]
        if len(results) != len(self.batch):
            raise Fail('expected %d results, got %d\n%s\n%s' %
                       (len(self.batch), len(results), r.stdout, r.stderr))
        for (cmd, expect_ok, cb), res in zip(self.batch, results):
            ok = (res == 'OK')
            if expect_ok is not None and ok != expect_ok:
                raise Fail('%s: got %s, expected %s' %
                           (cmd, res, 'OK' if expect_ok else 'ERR'))
            if cb:
                cb(ok, res)
        self.batch = []

    # -- operations --------------------------------------------------------

    def pick_dir(self):
        return self.rng.choice(sorted(self.model.dirs))

    def pick_file(self):
        if not self.model.files:
            return None
        return self.rng.choice(sorted(self.model.files))

    def op_put(self):
        d = self.pick_dir()
        if self.model.files and self.rng.random() < 0.3:
            path = self.pick_file()
        else:
            name = rand_name(self.rng)
            ex = self.model.lookup(d, name)
            if ex is not None and Model.join(d, ex) in self.model.dirs:
                return
            path = Model.join(d, ex if ex is not None else name)
        data = os.urandom(rand_size(self.rng, self.a.sector))
        src = self.tmpfile(data)
        d2, n2 = path.rpartition('/')[0], path.rpartition('/')[2]
        self.model.dirs[d2][n2.lower()] = n2
        self.model.files[path] = bytearray(data)
        self.queue('put %s %s' % (src, path), True)

    def op_append(self):
        path = self.pick_file()
        if path is None:
            return
        data = os.urandom(rand_size(self.rng, self.a.sector) // 2)
        self.model.files[path] += data
        self.queue('append %s %s' % (self.tmpfile(data), path), True)

    def op_write(self):
        path = self.pick_file()
        if path is None:
            return
        cur = self.model.files[path]
        off = self.rng.randint(0, len(cur) + 3 * self.a.sector)
        data = os.urandom(rand_size(self.rng, self.a.sector) // 3)
        if data:
            if off > len(cur):
                cur += bytes(off - len(cur))
            cur[off:off + len(data)] = data
        self.queue('write %s %d %s' % (path, off, self.tmpfile(data)), True)

    def op_truncate(self):
        path = self.pick_file()
        if path is None:
            return
        cur = self.model.files[path]
        size = self.rng.randint(0, len(cur) + 2 * self.a.sector)
        if size <= len(cur):
            del cur[size:]
        else:
            cur += bytes(size - len(cur))
        self.queue('truncate %s %d' % (path, size), True)

    def op_rm(self):
        path = self.pick_file()
        if path is None:
            return
        del self.model.files[path]
        d, n = path.rpartition('/')[0], path.rpartition('/')[2]
        del self.model.dirs[d][n.lower()]
        self.queue('rm %s' % path, True)

    def op_mkdir(self):
        d = self.pick_dir()
        name = rand_name(self.rng)
        ex = self.model.lookup(d, name)
        path = Model.join(d, name)
        if ex is not None:
            self.queue('mkdir %s' % path, False)
            return
        self.model.dirs[d][name.lower()] = name
        self.model.dirs[path] = {}
        self.queue('mkdir %s' % path, True)

    def op_rmdir(self):
        cands = [p for p in self.model.dirs if p != '']
        if not cands:
            return
        path = self.rng.choice(sorted(cands))
        empty = not self.model.dirs[path]
        if empty:
            del self.model.dirs[path]
            d, n = path.rpartition('/')[0], path.rpartition('/')[2]
            del self.model.dirs[d][n.lower()]
        self.queue('rm %s' % path, empty)

    def rename_tree(self, old, new):
        for tbl in (self.model.files, self.model.dirs):
            for p in sorted(tbl):
                if p == old or p.startswith(old + '/'):
                    tbl[new + p[len(old):]] = tbl.pop(p)

    def op_mv(self):
        objs = sorted(self.model.files) + [p for p in self.model.dirs if p]
        if not objs:
            return
        src = self.rng.choice(sorted(objs))
        sd, sn = src.rpartition('/')[0], src.rpartition('/')[2]
        if self.rng.random() < 0.2:
            dd, dn = sd, sn.swapcase()          # Case-only rename
        else:
            dd = self.pick_dir()
            dn = rand_name(self.rng)
        dst = Model.join(dd, dn)
        is_dir = src in self.model.dirs
        ex = self.model.lookup(dd, dn)
        same_entry = (dd == sd and ex is not None and ex.lower() == sn.lower())
        loop = is_dir and (dd == src or dd.startswith(src + '/'))
        if loop or (ex is not None and not same_entry):
            self.queue('mv %s %s' % (src, dst), False)
            return
        del self.model.dirs[sd][sn.lower()]
        self.model.dirs[dd][dn.lower()] = dn
        self.rename_tree(src, dst)
        self.queue('mv %s %s' % (src, dst), True)

    def verify_tree(self):
        """Lists every directory and reads back every file"""
        dirs = sorted(self.model.dirs)
        script = ''.join('ls %s\n' % (d if d else '/') for d in dirs)
        r = self.tool('script', inp=script)
        self.parse_listings(r.stdout, len(dirs))
        paths = sorted(self.model.files)
        for i in range(0, len(paths), 50):
            outputs = {}
            script = ''
            for k, path in enumerate(paths[i:i + 50]):
                out = os.path.join(self.work, 'out%d' % k)
                outputs[path] = out
                script += 'get %s %s\n' % (path, out)
            r = self.tool('script', inp=script)
            self.compare_reads(outputs, r.stdout)

    def parse_listings(self, stdout, count):
        dirs = sorted(self.model.dirs)
        lines = stdout.splitlines()
        i = 0
        for k in range(count):
            names = []
            while lines[i] not in ('OK',) and not lines[i].startswith('ERR'):
                names.append(lines[i].split()[-1].rstrip('/'))
                i += 1
            if lines[i] != 'OK':
                raise Fail('ls %s: %s' % (dirs[k], lines[i]))
            i += 1
            want = sorted(self.model.dirs[dirs[k]].values())
            if sorted(names) != want:
                raise Fail('ls %s: got %s want %s' % (dirs[k], sorted(names),
                                                      want))

    def compare_reads(self, outputs, stdout):
        if 'ERR' in stdout:
            raise Fail('read failed:\n' + stdout[-2000:])
        for path, out in outputs.items():
            with open(out, 'rb') as f:
                got = f.read()
            if got != bytes(self.model.files[path]):
                raise Fail('%s: content differs (%d vs %d bytes)' %
                           (path, len(got), len(self.model.files[path])))

    def run(self):
        sectors = self.a.sectors
        if not self.a.image:
            with open(self.img, 'wb') as f:
                f.truncate(sectors * self.a.sector)
        r = self.tool('format', 'Stress' + fmt_opts(self.a),
                      str(self.a.cluster))
        if r.returncode != 0:
            raise Fail('format: ' + r.stderr)
        self.check()
        free0 = self.free()
        root0 = self.root_blocks()

        ops = [(self.op_put, 6), (self.op_append, 3), (self.op_write, 3),
               (self.op_truncate, 2), (self.op_rm, 3), (self.op_mkdir, 2),
               (self.op_rmdir, 1), (self.op_mv, 3)]
        table = [f for f, w in ops for _ in range(w)]
        done = 0
        while done < self.a.ops:
            for _ in range(40):
                self.rng.choice(table)()
                done += 1
            if self.rng.random() < 0.3:
                self.queue('remount', True)
            self.run_batch()
            self.check()
            if self.rng.random() < 0.25:
                self.verify_tree()
            # Keep the disk from filling up
            if self.free() < free0 // 2:
                for _ in range(len(self.model.files) // 2):
                    self.op_rm()
                self.run_batch()
                self.check()

        self.verify_tree()
        if self.a.keep:
            return
        # Remove everything; all space must come back
        for p in sorted(self.model.files):
            self.queue('rm %s' % p, True)
        for d in sorted(self.model.dirs, key=lambda p: -p.count('/')):
            if d:
                self.queue('rm %s' % d, True)
        self.run_batch()
        self.check()
        free1 = self.free()
        grown = self.root_blocks() - root0     # Directories never shrink
        if free1 + grown != free0:
            raise Fail('free space %d (+%d root growth) after deleting all, '
                       'was %d' % (free1, grown, free0))

    def free(self):
        return int(self.tool('free').stdout.split()[0])

    def root_blocks(self):
        out = self.tool('stat', '/').stdout
        return int(out.split('blocks')[1].split()[0])


def fill_test(args):
    """Fills the disk with files until it is full, then empties it"""
    t = Tester(args)
    with open(t.img, 'wb') as f:
        f.truncate(args.sectors * args.sector)
    r = t.tool('format', 'Full' + fmt_opts(args), str(args.cluster))
    if r.returncode != 0:
        raise Fail('format: ' + r.stderr)
    free0 = t.free()
    root0 = t.root_blocks()
    big = os.urandom(args.sector * 23 + 5)
    src = t.tmpfile(big)
    script = ''.join('put %s f%d\n' % (src, i) for i in range(4000))
    r = t.tool('script', inp=script)
    res = r.stdout.split('\n')
    nok = res.index('ERR disk full') if 'ERR disk full' in res else None
    if nok is None:
        raise Fail('disk never filled')
    t.check()
    script = ''.join('rm f%d\n' % i for i in range(nok + 1))
    r = t.tool('script', inp=script)
    t.check()
    free1 = t.free()
    grown = t.root_blocks() - root0
    if free1 + grown != free0:
        raise Fail('fill: free %d (+%d root growth) after emptying, was %d' %
                   (free1, grown, free0))
    return nok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sector', type=int, default=512)
    ap.add_argument('--cluster', type=int, default=1)
    ap.add_argument('--sectors', type=int, default=4000)
    ap.add_argument('--buffers', type=int, default=16)
    ap.add_argument('--ops', type=int, default=1000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--fill', action='store_true')
    ap.add_argument('--fs', default='os9', choices=['os9', '6809', 'os9k-be', 'os9k-le'],
                    help='filesystem to format')
    ap.add_argument('--half', action='store_true',
                    help='6809 half-sector layout (one sector per block)')
    ap.add_argument('--keep', action='store_true',
                    help='leave the final tree in place')
    ap.add_argument('--image', help='use this image (with --offset)')
    ap.add_argument('--offset', type=int, default=0,
                    help='byte offset of the filesystem in --image')
    ap.add_argument('workdir')
    a = ap.parse_args()
    global NAME_MAX
    if a.fs == '6809':
        NAME_MAX = 29
    elif a.fs != 'os9':
        NAME_MAX = 43
    try:
        if a.fill:
            n = fill_test(a)
            print('fill ok: %d files before full' % n)
        else:
            Tester(a).run()
            print('stress ok: %s sector %d cluster %d seed %d' %
                  (a.fs, a.sector, a.cluster, a.seed))
    except Fail as e:
        print('FAIL (%s sector %d cluster %d seed %d): %s' %
              (a.fs, a.sector, a.cluster, a.seed, e))
        sys.exit(1)


if __name__ == '__main__':
    main()
