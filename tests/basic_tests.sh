#!/bin/sh
#
# Freya - BASIC regression test.
#
# Every tests/basic/NAME.bas is run with `-r` (load, run, exit) and its
# output, stdout and stderr together with the exit status, is compared
# with tests/basic/NAME.out.  NAME.in, when present, is the program's
# standard input.  tests/basic/session.in is typed at the interactive
# interpreter instead and compared with session.out; the free memory
# figure depends on the machine and is filtered out.
#
# The programs run on basic-host, the interpreter compiled natively
# with the float arithmetic of the board build, so the recorded output
# is what the board prints.  basic/Makefile builds it; this script
# builds it when it is missing.  UPDATE=1 rewrites the .out files.
#
# When build/basic/basic11.vm is there (make -C basic vm, which needs
# cproc and QBE), every program runs again on that image, the same
# basic.c compiled for the virtual machine, under runbasic, and has to
# print the same bytes: there is one .out for both.  VM=1 makes a
# missing image a failure rather than a note.
#
# When qemu-arm is installed, every program runs a third time on the
# code compiled for the board, with its FPU, as samples/basic11 is:
# build/basic/basic-arm, which this script builds when it is missing.
# ARM=1 makes a missing one a failure.
#
set -e

cd "$(dirname "$0")/.."
ROOT=$(pwd)
OUT=build/basic
TMP=$OUT/testtmp
mkdir -p "$TMP"
rm -f "$TMP"/*
: > "$TMP/empty"

if [ ! -x "$OUT/basic-host" ]; then
    make -C basic >/dev/null
fi

status=0

builds="native"
if [ -f "$OUT/basic11.vm" ] && [ -x "$OUT/runbasic" ]; then
    builds="native vm"
elif [ "${VM:-0}" = 1 ]; then
    echo "  FAIL  no $OUT/basic11.vm and runbasic: make -C basic vm"
    status=1
else
    echo "  note  no $OUT/basic11.vm: the VM build is not tested (make -C basic vm)"
fi

if [ ! -x "$OUT/basic-arm" ] && command -v qemu-arm >/dev/null 2>&1 &&
   command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    make -C basic arm >/dev/null
fi
if [ -x "$OUT/basic-arm" ] && command -v qemu-arm >/dev/null 2>&1; then
    builds="$builds arm"
elif [ "${ARM:-0}" = 1 ]; then
    echo "  FAIL  no $OUT/basic-arm or no qemu-arm: make -C basic arm"
    status=1
else
    echo "  note  no $OUT/basic-arm or qemu-arm: the board's code is not tested (make -C basic arm)"
fi

# run BUILD PROGRAM-OR-EMPTY STDIN RESULT
run() {
    case $1 in
        vm)  set -- "$2" "$3" "$4" "$ROOT/$OUT/runbasic" "$ROOT/$OUT/basic11.vm" ;;
        arm) set -- "$2" "$3" "$4" qemu-arm -cpu max "$ROOT/$OUT/basic-arm" ;;
        *)   set -- "$2" "$3" "$4" "$ROOT/$OUT/basic-host" ;;
    esac
    prog=$1
    in=$2
    res=$3
    shift 3
    if [ -n "$prog" ]; then
        "$@" -r "$prog" < "$in" > "$res" 2>&1 || echo "exit $?" >> "$res"
    else
        "$@" < "$in" > "$res" 2>&1 || echo "exit $?" >> "$res"
    fi
    sed -i '/bytes of program/d' "$res"
}

for build in $builds; do
    for prog in tests/basic/*.bas tests/basic/session.in; do
        name=$(basename "$prog" .bas)
        name=${name%.in}
        case $prog in
            *.bas) src=$(cd "$(dirname "$prog")" && pwd)/$(basename "$prog")
                   in=tests/basic/$name.in
                   [ -f "$in" ] || in=$TMP/empty ;;
            *)     src=
                   in=$prog ;;
        esac
        got=$TMP/$name.$build.got
        want=tests/basic/$name.out
        # the programs may write files; keep those out of the tree
        (cd "$TMP" && run "$build" "$src" "$ROOT/$in" "$ROOT/$got")
        if [ "${UPDATE:-0}" = 1 ] && [ "$build" = native ]; then
            cp "$got" "$want"
        fi
        if [ ! -f "$want" ]; then
            echo "  FAIL  $name: no $want (run with UPDATE=1 to create it)"
            status=1
        elif cmp -s "$got" "$want"; then
            echo "  ok    $build $name"
        else
            echo "  FAIL  $build $name"
            diff "$want" "$got" | head -n 20
            status=1
        fi
    done
done

exit $status
