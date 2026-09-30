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

# run PROGRAM-OR-EMPTY STDIN RESULT
run() {
    bin=$ROOT/$OUT/basic-host
    if [ -n "$1" ]; then
        "$bin" -r "$1" < "$2" > "$3" 2>&1 || echo "exit $?" >> "$3"
    else
        "$bin" < "$2" > "$3" 2>&1 || echo "exit $?" >> "$3"
    fi
    sed -i '/bytes of program/d' "$3"
}

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
    got=$TMP/$name.got
    want=tests/basic/$name.out
    # the programs may write files; keep those out of the tree
    (cd "$TMP" && run "$src" "$ROOT/$in" "$ROOT/$got")
    if [ "${UPDATE:-0}" = 1 ]; then
        cp "$got" "$want"
    fi
    if [ ! -f "$want" ]; then
        echo "  FAIL  $name: no $want (run with UPDATE=1 to create it)"
        status=1
    elif cmp -s "$got" "$want"; then
        echo "  ok    $name"
    else
        echo "  FAIL  $name"
        diff "$want" "$got" | head -n 20
        status=1
    fi
done

exit $status
