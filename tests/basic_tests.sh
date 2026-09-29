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
# The programs run on basic-host, the interpreter compiled natively, and
# again on runbasic, the Freya VM running build/basic/basic.bin, when
# that image has been built (basic/Makefile needs cproc and QBE for it).
# Both are built by basic/Makefile's `host` target; this script builds
# them when they are missing.  UPDATE=1 rewrites the .out files.
#
set -e

cd "$(dirname "$0")/.."
ROOT=$(pwd)
OUT=build/basic
TMP=$OUT/testtmp
mkdir -p "$TMP"
rm -f "$TMP"/*
: > "$TMP/empty"

if [ ! -x "$OUT/basic-host" ] || [ ! -x "$OUT/runbasic" ]; then
    make -C basic host >/dev/null
fi

status=0
runners="host"
if [ -f "$OUT/basic.bin" ]; then
    runners="host vm"
else
    echo "  skip  $OUT/basic.bin not built; VM runs skipped"
fi

# run RUNNER PROGRAM-OR-EMPTY STDIN RESULT
run() {
    case $1 in
        host) if [ -n "$2" ]; then
                  "$ROOT/$OUT/basic-host" -r "$2" < "$3" > "$4" 2>&1 || echo "exit $?" >> "$4"
              else
                  "$ROOT/$OUT/basic-host" < "$3" > "$4" 2>&1 || echo "exit $?" >> "$4"
              fi ;;
        vm)   if [ -n "$2" ]; then
                  "$ROOT/$OUT/runbasic" -r "$2" "$ROOT/$OUT/basic.bin" < "$3" > "$4" 2>&1 || echo "exit $?" >> "$4"
              else
                  "$ROOT/$OUT/runbasic" "$ROOT/$OUT/basic.bin" < "$3" > "$4" 2>&1 || echo "exit $?" >> "$4"
              fi ;;
    esac
    sed -i '/bytes of program/d' "$4"
}

for prog in tests/basic/*.bas tests/basic/session.in; do
    name=$(basename "$prog" .bas)
    name=${name%.in}
    want=tests/basic/$name.out
    case $prog in
        *.bas) src=$(cd "$(dirname "$prog")" && pwd)/$(basename "$prog")
               in=tests/basic/$name.in
               [ -f "$in" ] || in=$TMP/empty ;;
        *)     src=
               in=$prog ;;
    esac
    for r in $runners; do
        got=$TMP/$name.$r
        # the programs may write files; keep those out of the tree
        (cd "$TMP" && run "$r" "$src" "$ROOT/$in" "$ROOT/$got")
        if [ "$r" = host ] && [ "${UPDATE:-0}" = 1 ]; then
            cp "$got" "$want"
        fi
        if [ ! -f "$want" ]; then
            echo "  FAIL  $name: no $want (run with UPDATE=1 to create it)"
            status=1
        elif cmp -s "$got" "$want"; then
            echo "  ok    $name ($r)"
        else
            echo "  FAIL  $name ($r)"
            diff "$want" "$got" | head -n 20
            status=1
        fi
    done
done

exit $status
