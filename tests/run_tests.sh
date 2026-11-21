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
#     fragmented across several FD blocks.
#  5. If OS9K_IMAGE names an OS-9000 disk image (OS9K_OFFSET: byte offset
#     of its ID block), reading it, and writing to a copy of it.
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
MKOS9FS=$FS/../../tools/mkos9fs.py
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

    if [ -n "$OS9DCHECK" ]; then
        out=$(python3 "$HERE/stress.py" --fs $fs --keep --sector 512 \
              --sectors 32768 --buffers 8 --ops $OPS --seed 5 \
              "$W/kd_$fs" 2>&1)
        echo "$out" | grep -q '^stress ok' && dcheck "$W/kd_$fs/test.img"
        result "os9dcheck after stress $fs" $? "$out"
    fi
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

echo "$pass passed, $fail failed"
[ $fail = 0 ] && rm -rf "$W"
[ $fail = 0 ]
