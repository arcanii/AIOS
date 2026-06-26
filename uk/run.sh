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
           echo "=== M3i: dash (Debian Almquist shell) compiled UNMODIFIED -- the operational shell ===" &&
           printf "  echo arith: "; ./aios-uk ./dash -c "echo \$((2 + 3 * 4))" 2>/dev/null;
           printf "  control:    "; ./aios-uk ./dash -c "true && echo yes || echo no" 2>/dev/null;
           printf "  for loop:   "; ./aios-uk ./dash -c "for i in 1 2 3; do printf \"\$i \"; done; echo" 2>/dev/null;
           printf "  pipeline:   "; ./aios-uk ./dash -c "./sbase-echo a b c d e | ./sbase-wc -w" 2>/dev/null;
           printf "  cmd subst:  "; ./aios-uk ./dash -c "echo got=\$(./sbase-echo hi)" 2>/dev/null;
           echo "=== M4: boundary ENFORCED -- a guest CANNOT bypass the kernel (escape is blocked) ===" &&
           echo "  guest_escape attempts a raw Linux write(64); the [2] LINUX line must NOT appear:" &&
           ./aios-uk ./guest_escape 2>&1 | sed "s/^/    /"; ./aios-uk ./guest_escape >/dev/null 2>&1; echo "  [escape guest killed, exit $? (expect 159)]";
           echo "=== M3d gate: prog_pipebig exit must be 0 ===" &&
           ./aios-uk ./prog_pipebig >/dev/null 2>&1; rc=$?; echo "  [exit $rc]";
           test "$rc" = 0'
