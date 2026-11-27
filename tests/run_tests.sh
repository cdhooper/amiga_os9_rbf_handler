#!/bin/sh
#
# Host test suite for the RBF filesystem core.
#
#  1. Images built by mkos9fs.py are read correctly and check clean.
#  2. Randomized operations against a model, for every supported sector
#     size and several cluster sizes (stress.py), each step checked by
#     rbfcheck.
#  3. Filling the disk until it is full, then emptying it.
#  4. The same for OS-9000 RBF, big- and little-endian, plus files
#     fragmented across several FD blocks, and volumes whose ID block
#     follows a 512-byte PC boot sector.  Each filesystem type in an MBR
#     partition, and PC boot sectors that are not filesystems.
#  5. If OS9K_IMAGE names an OS-9000 disk image (OS9K_OFFSET: byte offset
#     of its ID block), reading it, and writing to a copy of it.
#  6. OS-9/6809: randomized operations and disk full, with 256-byte
#     sectors stored packed and in the half-sector layout; format options
#     and the type each format mounts as.  If OS9_6809_DISKS names a
#     directory of OS-9/6809 .dsk images, each is read and the CRCs of
#     its modules checked, and a copy is written to.
#
# If OS9DCHECK names Microware's os9dcheck (a Linux build of the OS-9000
# dcheck, e.g. bin.linux/os9dcheck), OS-9000 images are also checked with
# it: the fragmented-file images, and a stress run's final tree.
#
# usage: tests/run_tests.sh [quick]
#
HERE=$(cd "$(dirname "$0")" && pwd)
FS=$(dirname "$HERE")
TOOLS=$FS/host
MKOS9FS=${MKOS9FS:-$FS/../../tools/mkos9fs.py}
W=$HERE/work
OPS=3000
SEEDS="1 2 3"
[ "$1" = quick ] && { OPS=600; SEEDS=1; }

rm -rf "$W"
mkdir -p "$W"
pass=0
fail=0

dcheck() {     # image: 0 if OS9DCHECK is unset or finds no errors
    [ -z "$OS9DCHECK" ] && return 0
    # os9dcheck takes a name starting with '/' as an OS-9 device: pass
    # the image relative to its directory
    (cd "$(dirname "$1")" && "$OS9DCHECK" "./$(basename "$1")" 2>&1) |
        grep -q 'file structure is intact'
}

result() {     # name status [output]
    if [ "$2" = 0 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $1"
        [ -n "$3" ] && echo "$3" | tail -5 | sed 's/^/    /'
    fi
}

# 1. Read an image made by mkos9fs.py
python3 "$MKOS9FS" "$W/mk.adf" "$FS/core9" >/dev/null
out=$("$TOOLS/rbfcheck" "$W/mk.adf" 2>&1)
result "mkos9fs image checks clean" $? "$out"
ok=0
for f in "$FS"/core9/*; do
    "$TOOLS/rbftool" "$W/mk.adf" cat "$(basename "$f")" | cmp -s - "$f" || ok=1
done
result "mkos9fs image file contents" $ok

# 2. Randomized operations
for ss in 256 512 1024 2048 4096; do
    for cl in 1 2 4 8; do
        for seed in $SEEDS; do
            sectors=$((16 * 1024 * 1024 / ss))
            out=$(python3 "$HERE/stress.py" --sector $ss --cluster $cl \
                  --sectors $sectors --buffers 8 --ops $OPS --seed $seed \
                  "$W/s_${ss}_${cl}_${seed}" 2>&1)
            echo "$out" | grep -q '^stress ok'
            result "stress sector $ss cluster $cl seed $seed" $? "$out"
        done
    done
done

# 3. Disk full
for ss in 256 512 4096; do
    for cl in 1 4; do
        out=$(python3 "$HERE/stress.py" --fill --sector $ss --cluster $cl \
              --sectors $((2 * 1024 * 1024 / ss)) "$W/f_${ss}_${cl}" 2>&1)
        echo "$out" | grep -q '^fill ok'
        result "disk full sector $ss cluster $cl" $? "$out"
    done
done

# 4. OS-9000
for fs in os9k-be os9k-le; do
    for ss in 256 512 1024 4096; do
        for seed in $SEEDS; do
            sectors=$((16 * 1024 * 1024 / ss))
            out=$(python3 "$HERE/stress.py" --fs $fs --sector $ss \
                  --sectors $sectors --buffers 8 --ops $OPS --seed $seed \
                  "$W/k_${fs}_${ss}_${seed}" 2>&1)
            echo "$out" | grep -q '^stress ok'
            result "stress $fs sector $ss seed $seed" $? "$out"
        done
    done
    for ss in 256 4096; do
        out=$(python3 "$HERE/stress.py" --fs $fs --fill --sector $ss \
              --sectors $((2 * 1024 * 1024 / ss)) "$W/kf_${fs}_${ss}" 2>&1)
        echo "$out" | grep -q '^fill ok'
        result "disk full $fs sector $ss" $? "$out"
    done

    # Two files appended in turn: each needs several FD blocks
    img=$W/frag_$fs.img
    : > "$img"; truncate -s 4M "$img"
    head -c 3000 /dev/urandom > "$W/chunk"
    : > "$W/a.want"
    : > "$W/scr"
    for i in $(seq 80); do
        cat "$W/chunk" >> "$W/a.want"
        printf 'append %s A\nappend %s B\n' "$W/chunk" "$W/chunk" >> "$W/scr"
    done
    T="$TOOLS/rbftool -s 256"
    ok=0
    $T -f $fs "$img" format Frag >/dev/null
    free0=$($T "$img" free)
    $T "$img" script < "$W/scr" >/dev/null
    dcheck "$img" || ok=1
    blocks=$($T "$img" stat A | sed -n 's/.*blocks \([0-9]*\).*/\1/p')
    [ "$blocks" -gt 940 ] || ok=1          # 938 data blocks, 4 FD blocks
    "$TOOLS/rbfcheck" "$img" >/dev/null || ok=1
    $T "$img" get A "$W/a.got" && cmp -s "$W/a.got" "$W/a.want" || ok=1
    printf 'truncate A 10000\nrm B\n' | $T "$img" script >/dev/null
    "$TOOLS/rbfcheck" "$img" >/dev/null || ok=1
    dcheck "$img" || ok=1
    head -c 10000 "$W/a.want" > "$W/a2.want"
    $T "$img" get A "$W/a.got" && cmp -s "$W/a.got" "$W/a2.want" || ok=1
    $T "$img" rm A >/dev/null
    "$TOOLS/rbfcheck" "$img" >/dev/null || ok=1
    [ "$($T "$img" free)" = "$free0" ] || ok=1
    result "$fs files in several FD blocks" $ok

    # PC layout: the ID block follows a 512-byte boot sector (x86)
    for ss in 256 512 1024; do
        img=$W/pc_${fs}_$ss.img
        : > "$W/fsimg"; truncate -s 1440K "$W/fsimg"
        ok=0
        "$TOOLS/rbftool" -s $ss -f $fs "$W/fsimg" format PC >/dev/null ||
            ok=1
        printf 'put %s chunk\nmkdir D\n' "$W/chunk" |
            "$TOOLS/rbftool" -s $ss "$W/fsimg" script >/dev/null || ok=1
        # Padded to whole 1024-byte device blocks
        { head -c 512 /dev/urandom; cat "$W/fsimg"; head -c 512 /dev/zero; } \
            > "$img"
        cp "$img" "$W/pc.orig"
        for dss in 256 512 1024; do
            "$TOOLS/rbftool" -s $dss "$img" ls / 2>/dev/null |
                grep -q 'chunk$' || ok=1
        done
        printf 'put %s chunk2\nrm chunk\n' "$W/chunk" |
            "$TOOLS/rbftool" -s $ss "$img" script >/dev/null || ok=1
        "$TOOLS/rbftool" -s $ss "$img" get chunk2 "$W/got" &&
            cmp -s "$W/got" "$W/chunk" || ok=1
        "$TOOLS/rbfcheck" "$img" >/dev/null || ok=1
        cmp -s -n 512 "$img" "$W/pc.orig" || ok=1  # Boot sector untouched
        result "$fs found after a PC boot sector, $ss-byte blocks" $ok
    done

    if [ -n "$OS9DCHECK" ]; then
        out=$(python3 "$HERE/stress.py" --fs $fs --keep --sector 512 \
              --sectors 32768 --buffers 8 --ops $OPS --seed 5 \
              "$W/kd_$fs" 2>&1)
        echo "$out" | grep -q '^stress ok' && dcheck "$W/kd_$fs/test.img"
        result "os9dcheck after stress $fs" $? "$out"
    fi
done

# 4b. MBR-partitioned disks: the RBF partition is found and used, and
# nothing outside it is written.  A FAT-type decoy partition of zeros
# comes first (second for OS-9/68k, which rbfcheck takes as the first
# partition).  OS-9000 partitions begin with a PC boot sector.
for fs in os9k-be os9k-le 68k 6809; do
    ok=0
    : > "$W/fsimg"; truncate -s 2M "$W/fsimg"
    fss=512; [ $fs = 6809 ] && fss=256
    "$TOOLS/rbftool" -s $fss -f $fs "$W/fsimg" format Part >/dev/null || ok=1
    "$TOOLS/rbftool" -s $fss "$W/fsimg" put "$W/chunk" chunk >/dev/null ||
        ok=1
    case $fs in
        os9k-*) { head -c 512 /dev/urandom; cat "$W/fsimg"; } > "$W/pimg"
                python3 "$HERE/mkmbr.py" "$W/mbr.img" 06:@65536 \
                    09:"$W/pimg" ;;
        *)      python3 "$HERE/mkmbr.py" "$W/mbr.img" 4c:"$W/fsimg" \
                    06:@65536 ;;
    esac
    cp "$W/mbr.img" "$W/mbr.orig"
    "$TOOLS/rbftool" "$W/mbr.img" info 2>&1 | grep -q '^name     Part$' ||
        ok=1
    printf 'put %s c2\nmkdir D\nrm chunk\n' "$W/chunk" |
        "$TOOLS/rbftool" "$W/mbr.img" script >/dev/null || ok=1
    "$TOOLS/rbftool" "$W/mbr.img" get c2 "$W/got" &&
        cmp -s "$W/got" "$W/chunk" || ok=1
    "$TOOLS/rbfcheck" -s $fss "$W/mbr.img" >/dev/null || ok=1
    # Changed bytes must all lie inside the RBF partition
    python3 - "$W/mbr.orig" "$W/mbr.img" $fs <<'PYEOF' || ok=1
import struct, sys
a, b = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read()
i = 1 if sys.argv[3].startswith("os9k") else 0
lba, n = struct.unpack_from("<II", a, 446 + 16 * i + 8)
lo, hi = lba * 512 + (512 if i else 0), (lba + n) * 512
diff = [k for k in range(len(a)) if a[k] != b[k]]
sys.exit(1 if (len(a) != len(b) or not diff or
               any(k < lo or k >= hi for k in diff)) else 0)
PYEOF
    result "$fs in an MBR partition" $ok
done

# Not filesystems: a PC boot sector whose "partition table" is code, and
# boot code where OS-9/68k keeps its sector size, must be reported as
# not RBF rather than as a sector size problem
for t in table lsnsize; do
    head -c 1474560 /dev/urandom > "$W/junk.img"
    python3 - "$W/junk.img" $t <<'PYEOF'
import sys
f = open(sys.argv[1], "r+b")
f.write(b"\xeb\x0e")                  # x86 jmp, as PC boot sectors begin
f.seek(0x68); f.write(b"\x31\xd2")     # not a power of two
f.seek(510); f.write(b"\x55\xaa")
if sys.argv[2] == "table":
    f.seek(446); f.write(bytes([0x4f] * 16))
PYEOF
    out=$("$TOOLS/rbftool" "$W/junk.img" info 2>&1)
    [ "$out" = "mount: not an RBF filesystem" ]
    result "PC boot sector ($t) is not RBF" $? "$out"
done

# 5. A real OS-9000 disk
if [ -n "$OS9K_IMAGE" ]; then
    off=${OS9K_OFFSET:-0}
    out=$("$TOOLS/rbfcheck" -o $off "$OS9K_IMAGE" 2>&1)
    result "OS9K_IMAGE checks clean" $? "$out"
    cp "$OS9K_IMAGE" "$W/real.img"
    head -c 3000000 /dev/urandom > "$W/big"
    printf 'put %s NEW1\nmkdir NEWDIR\nput %s NEWDIR/x\nmv NEW1 NEWDIR/y\n' \
        "$W/big" "$W/chunk" | "$TOOLS/rbftool" -o $off "$W/real.img" script \
        > /dev/null
    ok=0
    "$TOOLS/rbfcheck" -o $off "$W/real.img" >/dev/null || ok=1
    if [ -n "$OS9DCHECK" ]; then
        tail -c +$((off + 1)) "$W/real.img" > "$W/real_fs.img"
        dcheck "$W/real_fs.img" || ok=1
    fi
    "$TOOLS/rbftool" -o $off "$W/real.img" get NEWDIR/y "$W/got" &&
        cmp -s "$W/got" "$W/big" || ok=1
    result "OS9K_IMAGE copy written and checked" $ok
fi

# 6. OS-9/6809
for spec in "256 1" "256 4" "512 1" "512 2 --half" "1024 1 --half"; do
    set -- $spec
    for seed in $SEEDS; do
        out=$(python3 "$HERE/stress.py" --fs 6809 --sector $1 --cluster $2 $3 \
              --sectors $((8 * 1024 * 1024 / $1)) --buffers 8 --ops $OPS \
              --seed $seed "$W/x_$1_$2_$seed$3" 2>&1)
        echo "$out" | grep -q '^stress ok'
        result "stress 6809 device $1 cluster $2 $3 seed $seed" $? "$out"
    done
done
for spec in "256" "512 --half"; do
    set -- $spec
    out=$(python3 "$HERE/stress.py" --fs 6809 --fill --sector $1 $2 \
          --sectors $((2 * 1024 * 1024 / $1)) "$W/xf_$1$2" 2>&1)
    echo "$out" | grep -q '^fill ok'
    result "disk full 6809 device $1 $2" $? "$out"
done

# Format options: device sector size, name with options, type, sector size
img=$W/opt.img
while read dss name ss type; do
    : > "$img"; truncate -s 4M "$img"
    ok=0
    "$TOOLS/rbftool" -s $dss "$img" format "$name" || ok=1
    info=$("$TOOLS/rbftool" -s $dss "$img" info)
    echo "$info" | grep -q "^type     $type\$" || ok=1
    echo "$info" | grep -q "x $ss bytes" || ok=1
    result "format \"$name\" on $dss-byte device: $type, $ss-byte sectors" \
        $ok "$info"
done <<FMTS
512 Plain 512 OS-9/68k
256 V,6809 256 OS-9/6809
512 V,6809 256 OS-9/6809
512 V|6809|half 256 OS-9/6809
512 V/68k/ss=256 256 OS-9/68k
512 V,9k 512 OS-9000 big-endian
512 V,os9k,LE,ss=2048 2048 OS-9000 little-endian
512 V,le,be 512 OS-9000 big-endian
512 V,6809,cl=4,id=1234,owner=2.3 256 OS-9/6809
FMTS
for name in "V,frob" "V,ss=300" "V,68k,half" "V,6809,ss=512" "V,le,cl=2" \
            "V,cl=0" "V,id=xyz" "V,6809,own=1.300"; do
    : > "$img"; truncate -s 4M "$img"
    "$TOOLS/rbftool" -s 512 "$img" format "$name" >/dev/null 2>&1
    [ $? != 0 ]
    result "format \"$name\" refused" $?
done
: > "$img"; truncate -s 4M "$img"
"$TOOLS/rbftool" -s 512 "$img" format "V,6809,id=1234,owner=2.3"
ok=0
[ "$(od -A n -t x1 -j 11 -N 2 "$img")" = " 02 03" ] || ok=1     # DD_OWN
[ "$(od -A n -t x1 -j 14 -N 2 "$img")" = " 12 34" ] || ok=1     # DD_DSK
[ -z "$(od -A n -t x1 -j 96 -N 12 "$img" | tr -d ' 0\n')" ] || ok=1
result "6809 LSN 0 owner, disk ID, no OS-9/68k fields" $ok

# Real OS-9/6809 disks
if [ -n "$OS9_6809_DISKS" ]; then
    for dsk in "$OS9_6809_DISKS"/*.dsk "$OS9_6809_DISKS"/*.DSK; do
        [ -f "$dsk" ] || continue
        b=$(basename "$dsk")
        "$TOOLS/rbftool" -r -s 256 "$dsk" info 2>/dev/null |
            grep -q '^type     OS-9/6809$' || continue
        out=$(python3 "$HERE/modcrc.py" "$TOOLS/rbftool" "$dsk" 2>&1)
        echo "$out" | grep -q ' 0 bad$'
        result "$b: files read, module CRCs good" $? "$out"

        # Write to a copy: it must gain no rbfcheck errors
        cp "$dsk" "$W/real6809.dsk"
        printf '%s\n' "put $W/chunk Name_that_is_29_characters_x" \
            "mkdir Dir_with_a_29_character_name" \
            "mv Name_that_is_29_characters_x Dir_with_a_29_character_name/f" |
            "$TOOLS/rbftool" -s 256 "$W/real6809.dsk" script >/dev/null
        ok=0
        "$TOOLS/rbftool" -s 256 "$W/real6809.dsk" get \
            Dir_with_a_29_character_name/f "$W/got" &&
            cmp -s "$W/got" "$W/chunk" || ok=1
        e0=$("$TOOLS/rbfcheck" -q -s 256 "$dsk" | grep -c .)
        e1=$("$TOOLS/rbfcheck" -q -s 256 "$W/real6809.dsk" | grep -c .)
        [ "$e0" = "$e1" ] || ok=1
        result "$b: copy written and checked" $ok
    done
fi

echo "$pass passed, $fail failed"
[ $fail = 0 ] && rm -rf "$W"
[ $fail = 0 ]
