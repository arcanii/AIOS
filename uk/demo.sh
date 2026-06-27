#!/bin/sh
# demo.sh -- a short narrated tour of AIOS running on THIS machine (e.g. the RPi4).
#
# AIOS is a gVisor-style USERSPACE KERNEL: the programs below run on the AIOS ABI, trapped + serviced
# by `aios-uk` -- NOT by Linux directly. Linux is just the substrate. Build first (`make all`), then:
#     cd ~/uk && sh demo.sh
# Every AIOS run is timeout-guarded so a (rare) intermittent ptrace stall can't freeze a live demo.
# For the INTERACTIVE shell (job control, ^C/^Z), see the note at the end.
set -u
cd "$(dirname "$0")"
[ -x ./aios-uk ] || { echo "build first:  make all"; exit 1; }
say() { printf '\n\033[1;36m== %s ==\033[0m\n' "$*"; }
A()   { timeout 25 ./aios-uk "$@"; }                 # run an AIOS program, timeout-guarded

say "0. where are we"
echo "  host kernel: $(uname -srm)"
echo "  AIOS:        aios-uk -- the AIOS userspace kernel (the programs below run on it, not on Linux)"

say "1. dash -- the UNMODIFIED Debian Almquist shell -- runs ON the AIOS kernel"
A ./dash -c 'echo "  hello from dash, serviced by aios-uk (not Linux)"; echo "  dash arithmetic: 2 + 3 * 4 = $((2 + 3 * 4))"' 2>/dev/null

say "2. real coreutils (suckless sbase, UNMODIFIED) -- an AIOS pipeline"
printf 'fig\napple\nbanana\napricot\n' > /tmp/aiosdemo_fruit
echo "  grep ap <file> | wc -l   (lines containing 'ap'; expect 2):"
A ./dash -c './sbase-grep ap /tmp/aiosdemo_fruit | ./sbase-wc -l' 2>/dev/null | sed 's/^/    /'
echo "  ls / | wc -l   (count of root entries):"
A ./dash -c './sbase-ls / | ./sbase-wc -l' 2>/dev/null | sed 's/^/    /'
rm -f /tmp/aiosdemo_fruit

say "3. the BOUNDARY is ENFORCED -- a guest that smuggles a raw Linux syscall is KILLED"
A ./guest_escape 2>&1 | grep -E 'guest_escape:|SECURITY' | sed 's/^/  /'
A ./guest_escape >/dev/null 2>&1; echo "  -> guest killed (exit $?, expect 159); the host NEVER ran the guest's syscall"

say "4. CONFINEMENT -- the guest sees ONLY the AIOS root; the Pi filesystem is invisible"
sh mkaiosroot.sh /tmp/aiosdemo >/dev/null 2>&1
echo "  ls -l /bin in the AIOS image (owner names from its OWN /etc/passwd):"
AIOS_ROOT=/tmp/aiosdemo PATH=/bin timeout 25 ./aios-uk /tmp/aiosdemo/bin/sh -c 'ls -l /bin' 2>/dev/null | head -4 | sed 's/^/    /'
echo "  cat /etc/passwd (inside the AIOS root -- works):"
AIOS_ROOT=/tmp/aiosdemo PATH=/bin timeout 25 ./aios-uk /tmp/aiosdemo/bin/sh -c 'cat /etc/passwd' 2>/dev/null | sed 's/^/    /'
echo "  cat /etc/hostname (a HOST file, outside the root -- must be DENIED):"
AIOS_ROOT=/tmp/aiosdemo PATH=/bin timeout 25 ./aios-uk /tmp/aiosdemo/bin/sh -c 'cat /etc/hostname' 2>&1 | grep -v 'aios-uk\]' | sed 's/^/    /'
rm -rf /tmp/aiosdemo

say "5. the PORTABILITY PROOF -- the SAME kernel over TWO host trap mechanisms"
echo "  ptrace backend (PTRACE_SYSCALL):"
A ./dash -c 'echo "    6 * 7 = $((6 * 7))"' 2>/dev/null
echo "  rebuilding aios-uk with the seccomp backend (kernel/aios_kernel.c BYTE-IDENTICAL)..."
make --no-print-directory PAL=seccomp aios-uk >/dev/null 2>&1
echo "  seccomp backend (SECCOMP_RET_TRACE):"
A ./dash -c 'echo "    6 * 7 = $((6 * 7))"' 2>/dev/null
make --no-print-directory aios-uk >/dev/null 2>&1   # restore the default (ptrace) backend

say "done -- everything above ran on the AIOS userspace kernel"
echo "  For an INTERACTIVE AIOS shell, use the CONFINED root so bare command names resolve to the AIOS"
echo "  coreutils (an AIOS shell runs only AIOS-ABI programs -- a host binary like uname/date makes a"
echo "  real Linux syscall and the boundary will, correctly, kill it). Run a real terminal into it:"
echo "      ssh -t pi@raspberrypi.local 'cd ~/uk && sh mkaiosroot.sh /tmp/r >/dev/null 2>&1 && \\"
echo "          AIOS_ROOT=/tmp/r PATH=/bin ./aios-uk /tmp/r/bin/sh'"
echo "  ...then try:  ls -l ;  grep root /etc/passwd ;  echo \$((6*7)) ;  cat  (^Z suspends, fg resumes, ^C kills)"
