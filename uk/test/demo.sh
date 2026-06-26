#!/bin/sh
# A live demo of the AIOS userspace kernel: a REAL shell script, run by UNMODIFIED dash, driving
# UNMODIFIED sbase utilities -- all on the AIOS kernel (no host syscalls reach Linux except through
# the PAL). Run with:  ./aios-uk ./dash test/demo.sh   (from the uk/ directory)

echo "=================================================================="
echo "  AIOS userspace kernel -- dash + real sbase, UNMODIFIED, live"
echo "=================================================================="
echo

echo "[1] dash language: arithmetic, loops, conditionals"
echo "    1 << 10        = $(( 1 << 10 ))"
echo "    (2+3)*4 - 5    = $(( (2 + 3) * 4 - 5 ))"
total=0
for i in 1 2 3 4 5 6 7 8 9 10; do total=$(( total + i )); done
echo "    sum 1..10      = $total"
i=1
while test $i -le 3; do echo "    while pass #$i"; i=$(( i + 1 )); done
if test -f ./dash; then echo "    test -f ./dash : the shell can see its own binary"; fi
echo

echo "[2] pipelines through real sbase + command substitution"
echo "    echo ... | sbase-wc -w  -> $(./sbase-echo the quick brown fox jumps | ./sbase-wc -w) words"
greeting=$(./sbase-echo hello from a subshell)
echo "    \$(sbase-echo ...)       -> $greeting"
echo

echo "[3] filesystem: mkdir -p, ls -l, rm -r (real sbase, real files)"
./sbase-mkdir -p /tmp/aiosdemo/sub
./sbase-echo "a real file written through the AIOS VFS" > /tmp/aiosdemo/hello.txt
echo "    ls -l /tmp/aiosdemo:"
./sbase-ls -l /tmp/aiosdemo
./sbase-rm -r /tmp/aiosdemo
test -e /tmp/aiosdemo || echo "    rm -r /tmp/aiosdemo : tree removed"
echo

echo "[4] real signal delivery -- the kernel runs the trap handler"
trap 'echo "    >> caught SIGUSR1 (delivered by the AIOS kernel), handler ran"' USR1
kill -USR1 $$
echo "    >> ...and the script resumed right after the handler"
echo

echo "[5] exit status flows through"
false; echo "    false -> \$? = $?"
true;  echo "    true  -> \$? = $?"
echo

echo "=================================================================="
echo "  done -- a POSIX shell + utilities on a gVisor-style kernel"
echo "=================================================================="
