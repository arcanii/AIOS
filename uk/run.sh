#!/bin/sh
# Build + run the AIOS userspace-kernel first-light inside an aarch64 Linux container.
#
# The Mac host is darwin and cannot ptrace Linux, so we run in colima's aarch64 Linux VM via
# docker. --cap-add=SYS_PTRACE allows PTRACE_SYSEMU + process_vm_readv on the guest. The repo's
# uk/ tree is mounted read-write so build artifacts land back on the host.
#
# Expected: the guest prints its line via the AIOS kernel, then exits through the AIOS ABI with
# code 42 (so "guest exit status: 42" below is SUCCESS, not an error).
set -eu

UK_DIR=$(cd "$(dirname "$0")" && pwd)
IMAGE=${IMAGE:-gcc:13}

docker run --rm --platform linux/arm64 --cap-add=SYS_PTRACE \
    -v "$UK_DIR":/uk -w /uk "$IMAGE" \
    sh -c 'make --no-print-directory clean && make --no-print-directory all &&
           echo "=== M1: guest_hello (WRITE + EXIT) ===" &&
           ./aios-uk ./guest_hello; echo "  [exit $?]";
           echo "=== M2: guest_fileio (VFS: OPEN/WRITE/READ/CLOSE) ===" &&
           ./aios-uk ./guest_fileio; echo "  [exit $?]";
           echo "=== host shell confirms the AIOS program wrote a REAL file: ===";
           ls -l /tmp/aios_m2.txt && cat /tmp/aios_m2.txt;
           echo "=== M3a: guest_cat /etc/hostname (real cat, filename from argv) ===" &&
           ./aios-uk ./guest_cat /etc/hostname; echo "  [exit $?]";
           echo "=== M3b: prog_args (ordinary C: main/printf/malloc/argv via libaios) ===" &&
           ./aios-uk ./prog_args first second; echo "  [exit $?]";
           echo "=== M3c.1: prog_wc -- a real wc on a file, then on host-piped stdin ===" &&
           ./aios-uk ./prog_wc /etc/hostname; echo "  [file exit $?]";
           printf "one two three\nfour five\n" | ./aios-uk ./prog_wc; echo "  [stdin exit $?]";
           echo "=== M3c.2: prog_bigalloc -- real mmap-backed malloc (kernel injects mmap) ===" &&
           ./aios-uk ./prog_bigalloc; echo "  [exit $?]";
           echo "=== M3c.3: prog_tail -n 3 /etc/os-release (fstat + lseek) ===" &&
           ./aios-uk ./prog_tail -n 3 /etc/os-release; echo "  [exit $?]";
           echo "=== M3d.1: prog_exec -- replace my image (AIOS_SYS_EXEC) ===" &&
           ./aios-uk ./prog_exec ./prog_args one two; echo "  [exec->prog_args exit $?]";
           ./aios-uk ./prog_exec /no/such/program; echo "  [exec-failure exit $?]";
           echo "=== M3d.2: prog_spawn -- the shell core (fork -> child exec -> parent waitpid) ===" &&
           ./aios-uk ./prog_spawn ./prog_args hello world; echo "  [spawn exit $?]";
           ./aios-uk ./prog_spawn ./prog_wc /etc/hostname; echo "  [spawn wc exit $?]";
           echo "=== M3d.2: prog_fork -- N children fork/exit/wait (exit 0 iff sum==6) ===" &&
           ./aios-uk ./prog_fork; echo "  [exit $?]";
           echo "=== M3d.3: prog_pipe -- fork + pipe, reader parks then drains, EOF on close ===" &&
           ./aios-uk ./prog_pipe; echo "  [exit $?]";
           echo "=== M3d.3: prog_pipeline -- prog_args | prog_wc (pipe + dup2 + exec) ===" &&
           ./aios-uk ./prog_pipeline; echo "  [exit $?]";
           echo "=== M3d.3: prog_pipebig -- 200KB through a pipe (writer + reader park) ===" &&
           ./aios-uk ./prog_pipebig; echo "  [exit $?]";
           echo "=== M3d.4: prog_sh -- an AIOS shell running real pipelines (capstone) ===" &&
           printf "./prog_args alpha beta | ./prog_wc\n./prog_args x | ./prog_wc | ./prog_wc\nexit\n" \
             | ./aios-uk ./prog_sh 2>/dev/null; echo "  [shell stdout above: 2-stage then 3-stage wc]";
           echo "=== M3e.1: prog_libc -- real C, standard headers only, compiled -nostdinc ===" &&
           ./aios-uk ./prog_libc 2>/dev/null; echo "  [exit $?]";
           echo "=== M3e.2: prog_stdio -- FILE* buffered stdio (fopen/fprintf/fgets/fwrite) ===" &&
           ./aios-uk ./prog_stdio; echo "  [exit $?]";
           echo "=== M3e.3: prog_errno -- errno + strerror + perror (ENOENT/EBADF) ===" &&
           ./aios-uk ./prog_errno; echo "  [exit $?]";
           echo "=== M3e.4: prog_fs -- mkdir/stat/rename/getcwd/chdir/unlink/rmdir/getpid ===" &&
           ./aios-uk ./prog_fs; echo "  [exit $?]";
           echo "=== M3e.5: prog_getopt -- getopt option parsing + qsort generic sort ===" &&
           ./aios-uk ./prog_getopt; echo "  [exit $?]";
           echo "=== M3e.6: prog_dir -- opendir/readdir/closedir (AIOS_SYS_GETDENTS, for ls) ===" &&
           ./aios-uk ./prog_dir; echo "  [exit $?]";
           echo "=== M3f: REAL vendored sbase, compiled UNMODIFIED against AIOS libc ===" &&
           ./aios-uk ./sbase-true  2>/dev/null; echo "  [sbase true  exit $? (expect 0)]";
           ./aios-uk ./sbase-false 2>/dev/null; echo "  [sbase false exit $? (expect 1)]";
           ./aios-uk ./sbase-echo hello from vendored sbase 2>/dev/null; echo "  [sbase echo exit $?]";
           ./aios-uk ./sbase-cat /etc/hostname 2>/dev/null; echo "  [sbase cat file exit $?]";
           printf "piped into real sbase cat\n" | ./aios-uk ./sbase-cat 2>/dev/null; echo "  [sbase cat stdin exit $?]";
           printf "one two three\nfour five\nsix\n" | ./aios-uk ./sbase-wc 2>/dev/null; echo "  [sbase wc (expect 3 6 28) exit $?]";
           rm -rf /tmp/aios_mkd && ./aios-uk ./sbase-mkdir -p /tmp/aios_mkd/x/y 2>/dev/null &&
             { test -d /tmp/aios_mkd/x/y && echo "  [sbase mkdir -p made /tmp/aios_mkd/x/y OK]"; }; rm -rf /tmp/aios_mkd;
           echo "--- M3g: sbase rm -r a nested tree (openat/fstatat/unlinkat/faccessat + recurse) ---" &&
           mkdir -p /tmp/aios_rmt/a/b && touch /tmp/aios_rmt/x /tmp/aios_rmt/a/y /tmp/aios_rmt/a/b/z &&
           ./aios-uk ./sbase-rm -r /tmp/aios_rmt 2>/dev/null &&
             { test -e /tmp/aios_rmt && echo "  [sbase rm -r FAILED -- tree survived]" || echo "  [sbase rm -r removed the tree OK]"; }; rm -rf /tmp/aios_rmt;
           echo "--- M3h: sbase ls + ls -l (readdir + stat + readlink + time/strftime + printf widths) ---" &&
           rm -rf /tmp/aios_lst && mkdir -p /tmp/aios_lst/sub && echo hi >/tmp/aios_lst/afile && ln -s afile /tmp/aios_lst/alink &&
           { echo "  plain ls:"; ./aios-uk ./sbase-ls /tmp/aios_lst 2>/dev/null | sed "s/^/    /"; echo "  ls -l:"; ./aios-uk ./sbase-ls -l /tmp/aios_lst 2>/dev/null | sed "s/^/    /"; }; rm -rf /tmp/aios_lst;
           echo "--- more vendored sbase utils, UNMODIFIED: head / tail / cp / mv ---" &&
           printf "l1\nl2\nl3\nl4\nl5\n" >/tmp/aios_u.txt;
           printf "  head -n 2:  "; ./aios-uk ./sbase-head -n 2 /tmp/aios_u.txt 2>/dev/null | tr "\n" " "; echo;
           printf "  tail -n 2:  "; ./aios-uk ./sbase-tail -n 2 /tmp/aios_u.txt 2>/dev/null | tr "\n" " "; echo;
           rm -f /tmp/aios_cp.txt; ./aios-uk ./sbase-cp /tmp/aios_u.txt /tmp/aios_cp.txt 2>/dev/null;
           { cmp -s /tmp/aios_u.txt /tmp/aios_cp.txt && echo "  cp:         byte-identical copy OK"; };
           ./aios-uk ./sbase-mv /tmp/aios_cp.txt /tmp/aios_mv.txt 2>/dev/null;
           { test ! -e /tmp/aios_cp.txt && test -f /tmp/aios_mv.txt && echo "  mv:         src gone, dst present OK"; };
           rm -rf /tmp/aios_ct; mkdir -p /tmp/aios_ct/sub && echo x >/tmp/aios_ct/a && echo y >/tmp/aios_ct/sub/b;
           ./aios-uk ./sbase-cp -r /tmp/aios_ct /tmp/aios_ct2 2>/dev/null;
           { test -f /tmp/aios_ct2/sub/b && echo "  cp -r:      nested tree copied OK"; };
           printf "  dash pipe (head|tail): "; ./aios-uk ./dash -c "printf \"a\nb\nc\nd\n\" | ./sbase-head -n 3 | ./sbase-tail -n 1" 2>/dev/null;
           rm -rf /tmp/aios_u.txt /tmp/aios_mv.txt /tmp/aios_ct /tmp/aios_ct2;
           echo "--- file-metadata utils: ln / ln -s / chmod, and cp -p preserves mode+times ---" &&
           rm -rf /tmp/aios_md; mkdir -p /tmp/aios_md && echo data >/tmp/aios_md/target;
           ./aios-uk ./sbase-ln -s target /tmp/aios_md/lnk 2>/dev/null;
           { test -L /tmp/aios_md/lnk && echo "  ln -s:      created a symlink OK"; };
           ./aios-uk ./sbase-ln /tmp/aios_md/target /tmp/aios_md/hard 2>/dev/null;
           { test "$(stat -c %h /tmp/aios_md/target)" -ge 2 && echo "  ln (hard):  nlink>=2 OK"; };
           ./aios-uk ./sbase-chmod 0640 /tmp/aios_md/target 2>/dev/null;
           { test "$(stat -c %a /tmp/aios_md/target)" = 640 && echo "  chmod 0640: mode set OK"; };
           touch -d "2020-01-02 03:04:05" /tmp/aios_md/src 2>/dev/null; chmod 0641 /tmp/aios_md/src;
           ./aios-uk ./sbase-cp -p /tmp/aios_md/src /tmp/aios_md/pcopy 2>/dev/null;
           { test "$(stat -c %a%Y /tmp/aios_md/src)" = "$(stat -c %a%Y /tmp/aios_md/pcopy)" && echo "  cp -p:      mode + mtime preserved OK"; };
           rm -rf /tmp/aios_md;
           echo "--- sbase sort, UNMODIFIED (lexical / -u / -n incl. decimals via a real strtod / -rn) ---" &&
           printf "  lexical:  "; printf "cherry\napple\nbanana\napple\n" | ./aios-uk ./sbase-sort 2>/dev/null | tr "\n" " "; echo;
           printf "  sort -u:  "; printf "cherry\napple\nbanana\napple\n" | ./aios-uk ./sbase-sort -u 2>/dev/null | tr "\n" " "; echo;
           printf "  sort -n:  "; printf "1.5\n0.2\n10.25\n1.05\n2\n" | ./aios-uk ./sbase-sort -n 2>/dev/null | tr "\n" " "; echo;
           printf "  sort -rn: "; printf "10\n2\n1\n22\n3\n" | ./aios-uk ./sbase-sort -rn 2>/dev/null | tr "\n" " "; echo;
           echo "=== grep: the first regex util -- a real BRE/ERE engine in libaios, sbase grep UNMODIFIED ===" &&
           echo "--- prog_regex: the regcomp/regexec battery (BRE/ERE, classes, anchors, intervals) ---" &&
           ./aios-uk ./prog_regex 2>/dev/null | sed "s/^/    /";
           ./aios-uk ./prog_regex >/dev/null 2>&1; rxrc=$?; echo "  [prog_regex exit $rxrc (expect 0)]";
           printf "fig\napple\nbanana\ncherry\napricot\n" >/tmp/aios_fruit.txt;
           printf "  grep an (BRE substr):   "; ./aios-uk ./sbase-grep an /tmp/aios_fruit.txt 2>/dev/null | tr "\n" " "; echo;
           printf "  grep -E a(pp|pr):       "; ./aios-uk ./sbase-grep -E "a(pp|pr)" /tmp/aios_fruit.txt 2>/dev/null | tr "\n" " "; echo;
           printf "  grep -i APPLE:          "; ./aios-uk ./sbase-grep -i APPLE /tmp/aios_fruit.txt 2>/dev/null | tr "\n" " "; echo;
           printf "  grep -v a (lines no a): "; ./aios-uk ./sbase-grep -v a /tmp/aios_fruit.txt 2>/dev/null | tr "\n" " "; echo;
           printf "  grep -c a (count):      "; ./aios-uk ./sbase-grep -c a /tmp/aios_fruit.txt 2>/dev/null;
           printf "  grep -n ^a (anchored):  "; ./aios-uk ./sbase-grep -n "^a" /tmp/aios_fruit.txt 2>/dev/null | tr "\n" " "; echo;
           printf "  grep -w fig (word):     "; ./aios-uk ./sbase-grep -w fig /tmp/aios_fruit.txt 2>/dev/null | tr "\n" " "; echo;
           printf "  dash pipe grep|wc -l:   "; ./aios-uk ./dash -c "./sbase-grep a /tmp/aios_fruit.txt | ./sbase-wc -l" 2>/dev/null;
           rm -f /tmp/aios_fruit.txt;
           echo "=== passwd/group db -- getpwuid/getgrgid read /etc/passwd + /etc/group (ls -l shows names) ===" &&
           ./aios-uk ./prog_pwgrp 2>/dev/null | sed "s/^/    /";
           ./aios-uk ./prog_pwgrp >/dev/null 2>&1; pwrc=$?; echo "  [prog_pwgrp exit $pwrc (expect 0)]";
           rm -rf /tmp/aios_pw && mkdir -p /tmp/aios_pw && echo hi >/tmp/aios_pw/f;
           printf "  ls -l owner:group now NAMED: "; ./aios-uk ./sbase-ls -l /tmp/aios_pw 2>/dev/null | tr -s " " | cut -d" " -f3-4 | head -1;
           rm -rf /tmp/aios_pw;
           echo "=== job-control FOUNDATION -- process groups + tty foreground group + kill-to-a-group ===" &&
           ./aios-uk ./prog_jobctl </dev/null 2>/dev/null | sed "s/^/    /";
           ./aios-uk ./prog_jobctl </dev/null >/dev/null 2>&1; jcrc=$?; echo "  [prog_jobctl exit $jcrc (expect 0)]";
           echo "=== M3i: dash (Debian Almquist shell) compiled UNMODIFIED -- the operational shell ===" &&
           printf "  echo arith: "; ./aios-uk ./dash -c "echo \$((2 + 3 * 4))" 2>/dev/null;
           printf "  control:    "; ./aios-uk ./dash -c "true && echo yes || echo no" 2>/dev/null;
           printf "  for loop:   "; ./aios-uk ./dash -c "for i in 1 2 3; do printf \"\$i \"; done; echo" 2>/dev/null;
           printf "  pipeline:   "; ./aios-uk ./dash -c "./sbase-echo a b c d e | ./sbase-wc -w" 2>/dev/null;
           printf "  cmd subst:  "; ./aios-uk ./dash -c "echo got=\$(./sbase-echo hi)" 2>/dev/null;
           echo "=== a real clock -- AIOS_SYS_CLOCK_GETTIME (time/clock_gettime/gettimeofday are live) ===" &&
           ./aios-uk ./prog_clock 2>/dev/null | sed "s/^/    /";
           ./aios-uk ./prog_clock >/dev/null 2>&1; clkrc=$?; echo "  [prog_clock exit $clkrc (expect 0)]";
           printf "  host date -u:        "; date -u "+%Y-%m-%d %H:%M:%S UTC";
           echo "=== per-process cwd -- a child chdir does NOT leak into the parent ===" &&
           ./aios-uk ./prog_pcwd 2>/dev/null | sed "s/^/    /";
           ./aios-uk ./prog_pcwd >/dev/null 2>&1; pcwdrc=$?; echo "  [prog_pcwd exit $pcwdrc (expect 0)]";
           printf "  dash subshell (cd /tmp; (cd /); pwd -> /tmp): "; ./aios-uk ./dash -c "cd /tmp; (cd /); pwd" 2>/dev/null;
           echo "=== per-process umask -- applied on create, inherited across fork + exec ===" &&
           ./aios-uk ./prog_umask 2>/dev/null | sed "s/^/    /";
           ./aios-uk ./prog_umask >/dev/null 2>&1; umrc=$?; echo "  [prog_umask exit $umrc (expect 0)]";
           rm -rf /tmp/aios_umx; printf "  dash umask 077 survives exec into mkdir -> "; ./aios-uk ./dash -c "umask 077; ./sbase-mkdir /tmp/aios_umx" 2>/dev/null; stat -c %a /tmp/aios_umx; rm -rf /tmp/aios_umx;
           echo "=== M5: real signal delivery -- the kernel runs a guest signal handler ===" &&
           echo "--- prog_signal: install handler + raise + SIG_IGN ---" &&
           ./aios-uk ./prog_signal 2>/dev/null | sed "s/^/    /";
           printf "  dash trap+kill: "; ./aios-uk ./dash -c "trap \"echo caught-it\" USR1; kill -USR1 \$\$; echo then-continued" 2>/dev/null | tr "\n" " "; echo;
           echo "--- interactive dash + ^C over a pty (the kernel delivers SIGINT to dash) ---" &&
           cc -O2 -o /tmp/ctrlc_pty test/ctrlc_pty.c -lutil 2>/dev/null &&
           timeout 25 /tmp/ctrlc_pty ./aios-uk ./dash 2>/dev/null;
           echo "=== M4: boundary ENFORCED -- a guest CANNOT bypass the kernel (escape is blocked) ===" &&
           echo "  guest_escape attempts a raw Linux write(64); the [2] LINUX line must NOT appear:" &&
           ./aios-uk ./guest_escape 2>&1 | sed "s/^/    /"; ./aios-uk ./guest_escape >/dev/null 2>&1; echo "  [escape guest killed, exit $? (expect 159)]";
           echo "=== M4.2: filesystem confinement -- a serviced open() reaches ONLY the AIOS root ===" &&
           JR=/tmp/aios_jail; SECRET=/tmp/aios_jail_secret.txt;
           rm -rf "$JR" "$SECRET";
           mkdir -p "$JR/sub" && echo visible > "$JR/inside.txt" && echo deep > "$JR/sub/deep.txt";
           echo SECRET-host-data > "$SECRET";
           ln -s /tmp/aios_jail_secret.txt "$JR/abs_link"; ln -s ../aios_jail_secret.txt "$JR/dotdot_link";
           echo "  (a) UNCONFINED: real sbase cat reads a host file OUTSIDE any root (baseline):";
           ./aios-uk ./sbase-cat "$SECRET" 2>/dev/null | sed "s/^/      /";
           echo "  (b) CONFINED (AIOS_ROOT set): the SAME cat of that host file is DENIED:";
           AIOS_ROOT="$JR" ./aios-uk ./sbase-cat "$SECRET" 2>&1 | grep -v aios-uk | sed "s/^/      /";
           echo "  (c) CONFINED: cat of an in-root path still works:";
           AIOS_ROOT="$JR" ./aios-uk ./sbase-cat /inside.txt 2>/dev/null | sed "s/^/      /";
           echo "  (d) CONFINED red-team proof (prog_jail): every escape vector denied, in-root access works:";
           AIOS_ROOT="$JR" ./aios-uk ./prog_jail 2>/dev/null | sed "s/^/      /";
           AIOS_ROOT="$JR" ./aios-uk ./prog_jail >/dev/null 2>&1; jrc=$?; echo "  [prog_jail exit $jrc (expect 0)]";
           echo "=== M4.3: exec confinement -- a guest can EXEC only binaries inside the AIOS root ===" &&
           cp sbase-true "$JR/jailtrue"; cp sbase-false "$JR/jailfalse"; chmod +x "$JR/jailtrue" "$JR/jailfalse";
           echo "  (init = the operator-named entry, exempt; only a guests OWN exec() is confined):";
           AIOS_ROOT="$JR" ./aios-uk ./prog_execjail 2>/dev/null | sed "s/^/      /";
           AIOS_ROOT="$JR" ./aios-uk ./prog_execjail >/dev/null 2>&1; ejrc=$?; echo "  [prog_execjail exit $ejrc (expect 0)]";
           echo "  confined dash drives an in-root binary, but an out-of-root one is denied:";
           AIOS_ROOT="$JR" ./aios-uk ./dash -c "/jailtrue && echo in-root-exec-ok; /bin/echo SHOULD-NOT-PRINT" 2>&1 | grep -v aios-uk | sed "s/^/      /";
           rm -rf "$JR" "$SECRET";
           echo "=== gate: pipebig, jail, execjail, clock, pcwd, umask, regex, pwgrp, jobctl must exit 0 ===" &&
           ./aios-uk ./prog_pipebig >/dev/null 2>&1; rc=$?; echo "  [pipebig $rc, jail $jrc, execjail $ejrc, clock $clkrc, pcwd $pcwdrc, umask $umrc, regex $rxrc, pwgrp $pwrc, jobctl $jcrc]";
           test "$rc" = 0 && test "$jrc" = 0 && test "$ejrc" = 0 && test "$clkrc" = 0 && test "$pcwdrc" = 0 && test "$umrc" = 0 && test "$rxrc" = 0 && test "$pwrc" = 0 && test "$jcrc" = 0'
