#!/bin/sh
# Freya - checks for the shell language built as a Linux program.
#
#   sh linux/test.sh build/linux/fsh
#
# Each check runs fsh on a script and compares what it printed on
# standard output, and its exit status, with what is expected.

FSH=${1:-build/linux/fsh}
case $FSH in /*) ;; *) FSH=$(pwd)/$FSH ;; esac

pass=0
fail=0
dir=$(cd "$(mktemp -d)" && pwd -P)
trap 'rm -rf "$dir"' EXIT
cd "$dir" || exit 1

# check <name> <expected stdout> <expected status> <script>
check() {
    out=$(printf '%s\n' "$4" | "$FSH" 2>/dev/null)
    st=$?
    if [ "$out" = "$2" ] && [ "$st" = "$3" ]; then
        echo "  ok    $1"
        pass=$((pass + 1))
    else
        echo "  FAIL  $1"
        echo "        expected status $3: $(printf '%s' "$2" | tr '\n' '|')"
        echo "        got      status $st: $(printf '%s' "$out" | tr '\n' '|')"
        fail=$((fail + 1))
    fi
}

nl='
'

check "an expression prints its value" "5" 0 'echo(2 + 3)'
check "output lines end in a plain newline" "a${nl}b" 0 'echo("a"); echo("b")'
check "a function returns a value" "49" 0 'fn sq
  return $1 * $1
end
set a sq(7)
echo $a'
check "a loop repeats" "x${nl}x${nl}x" 0 'loop 3
  echo x
end'
check "the status of the last command is the exit status" \
      "nosuch: command not found (try 'help()')" 127 'nosuch'
check "exit() sets the exit status and stops" "a" 7 'echo a
exit(7)
echo b'
check "run() at the top level writes to the terminal" "hello 42" 0 \
      'run("echo", "hello", 42)'
check "run() at the top level leaves the program's status in \$?" "3" 0 \
      'run("sh", "-c", "exit 3")
echo $?'
check "run() in an expression returns standard output" "[abc]" 0 \
      'set out run("printf", "abc")
echo "[$out]"'
check "run() in an expression keeps the newline" "[a${nl}]" 0 \
      'set out run("echo", "a")
echo "[$out]"'
check "run() also returns the exit status" "4" 0 \
      'set out, rc run("sh", "-c", "printf x; exit 4")
echo $rc'
check "run() of a missing program is status 127" "127" 0 \
      'set out, rc run("no-such-program-here")
echo $rc'
check "run() with nothing printed is an empty string" "[]" 0 \
      'set out run("true")
echo "[$out]"'
check "run() takes numbers as their text" "1 2.5 true" 0 \
      'set out run("echo", 1, 2.5, true)
echo $out'
check "run() as a command needs a program" \
      "usage: run <program> [arg ...]" 1 'run()'
check "write() and cat() use plain newlines" "one two" 0 \
      'write("f.txt", "one", "two")
cat("f.txt")'
check "the file written has no carriage return" "8" 0 \
      'write("f2.txt", "one", "two")
set out run("sh", "-c", "wc -c < f2.txt")
echo $out'
check "open, write and read a file" "line" 0 \
      'set f open("g.txt", "w")
set n write($f, "line", 10b, "next")
set c close($f)
set f open("g.txt")
set l read($f)
set c close($f)
echo $l'
check "a relative path is the working directory's" "created $dir/sub${nl}yes" 0 \
      'mkdir("sub")
cd("sub")
write("h.txt", "x")
cd("..")
set out run("sh", "-c", "test -f sub/h.txt && printf yes")
echo $out'
check "rm -r removes a directory" "created $dir/d2${nl}removed $dir/d2${nl}gone" 0 \
      'mkdir("d2")
write("d2/a", "x")
rm("-r", "d2")
set out, rc run("test", "-e", "d2")
if $rc == 1
  echo gone
end'
check "patterns match" "key" 0 \
      'set m match("key=value", "(%w+)=")
echo $m'
check "a hardware command is not there" \
      "pin: command not found (try 'help()')" 127 'pin PA0 out'
check "a hardware function is not there" "set: no such function: adc" 1 \
      'set v adc("temp")'
check "a script thread runs" "w${nl}w" 0 \
      'fn worker
  loop 2
    echo w
    sleep(1)
  end
end
set t spawn("worker", 3)
set r join($t)'

printf 'echo(6 * 7)\n' > s.fsh
out=$("$FSH" s.fsh)
if [ "$out" = 42 ]; then echo "  ok    a script file runs"; pass=$((pass + 1))
else echo "  FAIL  a script file runs: $out"; fail=$((fail + 1)); fi

out=$("$FSH" -c 'echo(1 + 1)')
if [ "$out" = 2 ]; then echo "  ok    -c runs its text"; pass=$((pass + 1))
else echo "  FAIL  -c runs its text: $out"; fail=$((fail + 1)); fi

check "cpubench refuses a bad argument" \
      "usage: cpubench [-i|-f] [-t seconds]${nl}  -i integer (Dhrystone), -f float (Whetstone), -t 1..20 seconds each" 1 \
      'cpubench("-t", 0)'

out=$("$FSH" -c 'cpubench("-i", "-t", 1)')
if [ $? -eq 0 ] && printf '%s\n' "$out" | grep -q '^  check         : ok$'
then echo "  ok    cpubench runs Dhrystone and checks it"; pass=$((pass + 1))
else echo "  FAIL  cpubench runs Dhrystone and checks it: $out"; fail=$((fail + 1)); fi

out=$("$FSH" -c 'cpubench("-f", "-t", 1)')
if [ $? -eq 0 ] && printf '%s\n' "$out" | grep -q '^  N4 result     : 12.000000$'
then echo "  ok    cpubench runs Whetstone"; pass=$((pass + 1))
else echo "  FAIL  cpubench runs Whetstone: $out"; fail=$((fail + 1)); fi

echo
echo "$pass checks, $fail failures"
[ "$fail" -eq 0 ]
