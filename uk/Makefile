# AIOS userspace kernel -- M1 first-light build.
#
# Two artifacts:
#   aios-uk       the AIOS userspace kernel (host-agnostic core + the Linux PAL backend)
#   guest_hello   a freestanding AIOS-ABI program
#
# Build + run inside an aarch64 Linux container (see run.sh) -- the Mac host is darwin and
# cannot ptrace Linux. Targets aarch64; the kernel needs CAP_SYS_PTRACE at runtime.

CC      ?= cc
WARN     = -Wall -Wextra
INCLUDE  = -Iinclude

# The kernel is an ordinary hosted program (its PAL backend uses libc + ptrace).
KERNEL_SRC = kernel/aios_kernel.c pal/pal_linux.c
KERNEL_CFLAGS = -O2 $(WARN) $(INCLUDE)

# Freestanding guests bring their own _start (no libc).
GUEST_CFLAGS = -O2 $(WARN) $(INCLUDE) -ffreestanding -nostdlib -static \
               -fno-stack-protector -fno-asynchronous-unwind-tables

# libaios programs: ordinary C linked against the AIOS-ABI C runtime (lib/libaios.c brings _start).
PROG_CFLAGS = $(GUEST_CFLAGS) -Ilib
LIBAIOS = lib/libaios.c

# libc programs: REAL C sources compiled with -nostdinc so they pick up AIOS's shadow standard
# headers (lib/include) instead of the host libc's -- the path to recompiling sbase/dash unmodified.
# The compiler's own freestanding headers (stddef/stdint/stdarg) are kept via -isystem.
GCCINC   := $(shell $(CC) -print-file-name=include)
LIBC_CFLAGS = $(GUEST_CFLAGS) -nostdinc -isystem $(GCCINC) -Ilib -Ilib/include
LIBC_HDRS   = $(wildcard lib/include/*.h lib/include/sys/*.h)

# Vendored sbase (real third-party POSIX utilities, compiled UNMODIFIED against libaios). Variables
# up here so $(SBASE_BINS) is defined before `all` references it; the build rules are below.
SBASE        = vendor/sbase
# -w: we do NOT lint vendored sources (sbase trips various -Wall/-Wextra warnings upstream);
# patching sbase is off-limits, so silence warnings rather than touch it. Errors still surface, and
# our own libaios is linted in the prog_* builds. -I$(SBASE) resolves util.h/arg.h/fs.h/utf.h.
SBASE_CFLAGS = $(LIBC_CFLAGS) -I$(SBASE) -w
SBASE_DEPS   = $(LIBAIOS) $(LIBC_HDRS) $(SBASE)/util.h $(SBASE)/arg.h $(SBASE)/compat.h
SBASE_BINS   = sbase-true sbase-false sbase-echo sbase-cat sbase-wc sbase-mkdir sbase-rm sbase-ls \
               sbase-head sbase-tail sbase-cp sbase-mv sbase-ln sbase-chmod sbase-sort sbase-grep

# Vendored dash (the Debian Almquist shell, BSD) -- compiled UNMODIFIED against libaios. config.h +
# the generated sources (syntax/nodes/builtins/init/signames/token.h) are AIOS build inputs (see
# vendor/README.md). -include config.h, -DSHELL etc. per the dash build; -Dalloca=__builtin_alloca
# (alloca must hit the caller's stack); -w (never lint vendored code).
DASH        = vendor/dash
DASH_SRC    = $(DASH)/src
DASH_CFLAGS = $(LIBC_CFLAGS) -I$(DASH_SRC) -include $(DASH)/config.h \
              -DSHELL -DSMALL -DGLOB_BROKEN -D_GNU_SOURCE -Dalloca=__builtin_alloca -w
DASH_CFILES = $(addprefix $(DASH_SRC)/, alias.c arith_yacc.c arith_yylex.c cd.c error.c eval.c \
              exec.c expand.c histedit.c input.c jobs.c mail.c main.c memalloc.c miscbltin.c \
              mystring.c options.c parser.c redir.c show.c trap.c output.c bltin/printf.c system.c \
              bltin/test.c bltin/times.c var.c syntax.c nodes.c builtins.c init.c signames.c)

.PHONY: all run clean sbase
all: aios-uk guest_hello guest_fileio guest_cat guest_escape prog_args prog_wc prog_bigalloc prog_tail prog_exec \
     prog_fork prog_spawn prog_pipe prog_pipeline prog_pipebig prog_sh prog_libc prog_stdio prog_errno \
     prog_fs prog_getopt prog_dir prog_signal prog_jail prog_execjail prog_clock prog_pcwd prog_umask prog_regex prog_pwgrp prog_jobctl prog_stop $(SBASE_BINS) dash

aios-uk: $(KERNEL_SRC) include/pal.h include/aios_abi.h include/aios_version.h
	$(CC) $(KERNEL_CFLAGS) -o $@ $(KERNEL_SRC)

guest_hello: guest/guest_hello.c include/aios_abi.h
	$(CC) $(GUEST_CFLAGS) -o $@ guest/guest_hello.c

guest_fileio: guest/guest_fileio.c include/aios_abi.h
	$(CC) $(GUEST_CFLAGS) -o $@ guest/guest_fileio.c

guest_cat: guest/guest_cat.c include/aios_abi.h
	$(CC) $(GUEST_CFLAGS) -o $@ guest/guest_cat.c

guest_escape: guest/guest_escape.c include/aios_abi.h
	$(CC) $(GUEST_CFLAGS) -o $@ guest/guest_escape.c

prog_args: guest/prog_args.c $(LIBAIOS) lib/libaios.h include/aios_abi.h
	$(CC) $(PROG_CFLAGS) -o $@ guest/prog_args.c $(LIBAIOS)

prog_wc: guest/prog_wc.c $(LIBAIOS) lib/libaios.h include/aios_abi.h
	$(CC) $(PROG_CFLAGS) -o $@ guest/prog_wc.c $(LIBAIOS)

prog_bigalloc: guest/prog_bigalloc.c $(LIBAIOS) lib/libaios.h include/aios_abi.h
	$(CC) $(PROG_CFLAGS) -o $@ guest/prog_bigalloc.c $(LIBAIOS)

prog_tail: guest/prog_tail.c $(LIBAIOS) lib/libaios.h include/aios_abi.h
	$(CC) $(PROG_CFLAGS) -o $@ guest/prog_tail.c $(LIBAIOS)

prog_exec: guest/prog_exec.c $(LIBAIOS) lib/libaios.h include/aios_abi.h
	$(CC) $(PROG_CFLAGS) -o $@ guest/prog_exec.c $(LIBAIOS)

prog_fork: guest/prog_fork.c $(LIBAIOS) lib/libaios.h include/aios_abi.h
	$(CC) $(PROG_CFLAGS) -o $@ guest/prog_fork.c $(LIBAIOS)

prog_spawn: guest/prog_spawn.c $(LIBAIOS) lib/libaios.h include/aios_abi.h
	$(CC) $(PROG_CFLAGS) -o $@ guest/prog_spawn.c $(LIBAIOS)

prog_pipe: guest/prog_pipe.c $(LIBAIOS) lib/libaios.h include/aios_abi.h
	$(CC) $(PROG_CFLAGS) -o $@ guest/prog_pipe.c $(LIBAIOS)

prog_pipeline: guest/prog_pipeline.c $(LIBAIOS) lib/libaios.h include/aios_abi.h
	$(CC) $(PROG_CFLAGS) -o $@ guest/prog_pipeline.c $(LIBAIOS)

prog_pipebig: guest/prog_pipebig.c $(LIBAIOS) lib/libaios.h include/aios_abi.h
	$(CC) $(PROG_CFLAGS) -o $@ guest/prog_pipebig.c $(LIBAIOS)

prog_sh: guest/prog_sh.c $(LIBAIOS) lib/libaios.h include/aios_abi.h
	$(CC) $(PROG_CFLAGS) -o $@ guest/prog_sh.c $(LIBAIOS)

# A real-libc-style C program: standard #includes only, compiled -nostdinc against the shadow
# headers. Proves the path that recompiles sbase/dash unmodified.
prog_libc: guest/prog_libc.c $(LIBAIOS) $(LIBC_HDRS) include/aios_abi.h
	$(CC) $(LIBC_CFLAGS) -o $@ guest/prog_libc.c $(LIBAIOS)

prog_stdio: guest/prog_stdio.c $(LIBAIOS) $(LIBC_HDRS) include/aios_abi.h
	$(CC) $(LIBC_CFLAGS) -o $@ guest/prog_stdio.c $(LIBAIOS)

prog_errno: guest/prog_errno.c $(LIBAIOS) $(LIBC_HDRS) include/aios_abi.h
	$(CC) $(LIBC_CFLAGS) -o $@ guest/prog_errno.c $(LIBAIOS)

prog_fs: guest/prog_fs.c $(LIBAIOS) $(LIBC_HDRS) include/aios_abi.h
	$(CC) $(LIBC_CFLAGS) -o $@ guest/prog_fs.c $(LIBAIOS)

prog_getopt: guest/prog_getopt.c $(LIBAIOS) $(LIBC_HDRS) include/aios_abi.h
	$(CC) $(LIBC_CFLAGS) -o $@ guest/prog_getopt.c $(LIBAIOS)

prog_dir: guest/prog_dir.c $(LIBAIOS) $(LIBC_HDRS) include/aios_abi.h
	$(CC) $(LIBC_CFLAGS) -o $@ guest/prog_dir.c $(LIBAIOS)

prog_signal: guest/prog_signal.c $(LIBAIOS) $(LIBC_HDRS) include/aios_abi.h
	$(CC) $(LIBC_CFLAGS) -o $@ guest/prog_signal.c $(LIBAIOS)

# M4.2 red-team: proves the guest is confined to its AIOS root (run under AIOS_ROOT by run.sh).
prog_jail: guest/prog_jail.c $(LIBAIOS) $(LIBC_HDRS) include/aios_abi.h
	$(CC) $(LIBC_CFLAGS) -o $@ guest/prog_jail.c $(LIBAIOS)

# M4.3 red-team: proves a guest can only EXEC binaries inside its AIOS root (run under AIOS_ROOT).
prog_execjail: guest/prog_execjail.c $(LIBAIOS) lib/libaios.h include/aios_abi.h
	$(CC) $(PROG_CFLAGS) -o $@ guest/prog_execjail.c $(LIBAIOS)

# Proves the real time source (AIOS_SYS_CLOCK_GETTIME): time/clock_gettime/gettimeofday are live.
prog_clock: guest/prog_clock.c $(LIBAIOS) $(LIBC_HDRS) include/aios_abi.h
	$(CC) $(LIBC_CFLAGS) -o $@ guest/prog_clock.c $(LIBAIOS)

# Proves the cwd is PER-PROCESS: a child's chdir does not leak into the parent.
prog_pcwd: guest/prog_pcwd.c $(LIBAIOS) $(LIBC_HDRS) include/aios_abi.h
	$(CC) $(LIBC_CFLAGS) -o $@ guest/prog_pcwd.c $(LIBAIOS)

# Proves the umask is a real per-process mask applied on create (inherited across fork).
prog_umask: guest/prog_umask.c $(LIBAIOS) $(LIBC_HDRS) include/aios_abi.h
	$(CC) $(LIBC_CFLAGS) -o $@ guest/prog_umask.c $(LIBAIOS)

# Direct correctness gate for the libaios BRE/ERE engine (regcomp/regexec) -- the way grep uses it.
prog_regex: guest/prog_regex.c $(LIBAIOS) $(LIBC_HDRS) include/aios_abi.h
	$(CC) $(LIBC_CFLAGS) -o $@ guest/prog_regex.c $(LIBAIOS)

# Gate for the passwd/group database (getpwuid/getpwnam/getgrgid/getgrnam over /etc/passwd,/etc/group).
prog_pwgrp: guest/prog_pwgrp.c $(LIBAIOS) $(LIBC_HDRS) include/aios_abi.h
	$(CC) $(LIBC_CFLAGS) -o $@ guest/prog_pwgrp.c $(LIBAIOS)

# Gate for the job-control foundation: process groups + tty foreground group + kill-to-a-group.
prog_jobctl: guest/prog_jobctl.c $(LIBAIOS) $(LIBC_HDRS) include/aios_abi.h
	$(CC) $(LIBC_CFLAGS) -o $@ guest/prog_jobctl.c $(LIBAIOS)

# Gate for stop/continue: SIGSTOP/SIGCONT + wait(WUNTRACED)/wait(WCONTINUED).
prog_stop: guest/prog_stop.c $(LIBAIOS) $(LIBC_HDRS) include/aios_abi.h
	$(CC) $(LIBC_CFLAGS) -o $@ guest/prog_stop.c $(LIBAIOS)

# ---- vendored sbase build rules (vendor/sbase, suckless MIT, pinned -- see vendor/README.md) ----
# REAL third-party POSIX utilities compiled UNMODIFIED against AIOS's libc (libaios), via the
# libc-program class + the sbase include dir. Each utility links its source + the libutil objects
# it needs + libaios. Missing libc features are added to libaios, NEVER patched into sbase. Outputs
# are named sbase-<name> (gitignored via /sbase-*).
sbase: $(SBASE_BINS)

sbase-true: $(SBASE)/true.c $(LIBAIOS)
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/true.c $(LIBAIOS)
sbase-false: $(SBASE)/false.c $(LIBAIOS)
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/false.c $(LIBAIOS)
sbase-echo: $(SBASE)/echo.c $(SBASE_DEPS)
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/echo.c \
	      $(SBASE)/libutil/eprintf.c $(SBASE)/libutil/putword.c $(SBASE)/libutil/fshut.c $(LIBAIOS)
sbase-cat: $(SBASE)/cat.c $(SBASE_DEPS)
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/cat.c \
	      $(SBASE)/libutil/eprintf.c $(SBASE)/libutil/concat.c $(SBASE)/libutil/writeall.c $(LIBAIOS)
sbase-wc: $(SBASE)/wc.c $(SBASE_DEPS) $(SBASE)/utf.h
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/wc.c \
	      $(SBASE)/libutil/eprintf.c $(SBASE)/libutil/fshut.c \
	      $(SBASE)/libutf/fgetrune.c $(SBASE)/libutf/rune.c $(SBASE)/libutf/runetype.c \
	      $(SBASE)/libutf/isspacerune.c $(LIBAIOS)
sbase-mkdir: $(SBASE)/mkdir.c $(SBASE_DEPS)
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/mkdir.c \
	      $(SBASE)/libutil/eprintf.c $(SBASE)/libutil/mkdirp.c $(SBASE)/libutil/mode.c \
	      $(SBASE)/libutil/strlcpy.c $(LIBAIOS)
sbase-rm: $(SBASE)/rm.c $(SBASE_DEPS) $(SBASE)/fs.h
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/rm.c \
	      $(SBASE)/libutil/eprintf.c $(SBASE)/libutil/ealloc.c $(SBASE)/libutil/strlcpy.c \
	      $(SBASE)/libutil/confirm.c $(SBASE)/libutil/recurse.c $(SBASE)/libutil/rm.c $(LIBAIOS)
sbase-ls: $(SBASE)/ls.c $(SBASE_DEPS) $(SBASE)/fs.h $(SBASE)/utf.h
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/ls.c \
	      $(SBASE)/libutil/eprintf.c $(SBASE)/libutil/ealloc.c $(SBASE)/libutil/reallocarray.c \
	      $(SBASE)/libutil/human.c $(SBASE)/libutil/fshut.c \
	      $(SBASE)/libutf/rune.c $(SBASE)/libutf/runetype.c $(SBASE)/libutf/isprintrune.c \
	      $(SBASE)/libutf/iscntrlrune.c $(LIBAIOS)
sbase-head: $(SBASE)/head.c $(SBASE_DEPS)
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/head.c \
	      $(SBASE)/libutil/eprintf.c $(SBASE)/libutil/fshut.c $(SBASE)/libutil/strtonum.c $(LIBAIOS)
sbase-tail: $(SBASE)/tail.c $(SBASE_DEPS) $(SBASE)/utf.h
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/tail.c \
	      $(SBASE)/libutil/eprintf.c $(SBASE)/libutil/ealloc.c $(SBASE)/libutil/strtonum.c \
	      $(SBASE)/libutil/concat.c $(SBASE)/libutil/writeall.c $(SBASE)/libutil/fshut.c \
	      $(SBASE)/libutf/rune.c $(SBASE)/libutf/runetype.c $(LIBAIOS)
# cp/mv share libutil/cp.c (the copy engine) + recurse; cp/mv copy content + structure (-p metadata
# is degraded: no chmod/chown/utime syscalls; symlinks/special files report ENOSYS -- see libaios).
CP_OBJS = $(SBASE)/libutil/eprintf.c $(SBASE)/libutil/ealloc.c $(SBASE)/libutil/fshut.c \
          $(SBASE)/libutil/cp.c $(SBASE)/libutil/fnck.c $(SBASE)/libutil/recurse.c \
          $(SBASE)/libutil/concat.c $(SBASE)/libutil/enmasse.c $(SBASE)/libutil/strlcpy.c \
          $(SBASE)/libutil/strlcat.c $(SBASE)/libutil/confirm.c $(SBASE)/libutil/writeall.c \
          $(SBASE)/libutil/strtonum.c
sbase-sort: $(SBASE)/sort.c $(SBASE_DEPS) $(SBASE)/text.h $(SBASE)/utf.h
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/sort.c \
	      $(SBASE)/libutil/eprintf.c $(SBASE)/libutil/ealloc.c $(SBASE)/libutil/fshut.c \
	      $(SBASE)/libutil/getlines.c $(SBASE)/libutil/reallocarray.c $(SBASE)/libutil/strnsubst.c \
	      $(SBASE)/libutil/linecmp.c $(SBASE)/libutil/concat.c $(SBASE)/libutil/parseoffset.c \
	      $(SBASE)/libutil/strtonum.c $(SBASE)/libutil/memmem.c $(SBASE)/libutil/unescape.c \
	      $(SBASE)/libutil/strlcpy.c $(SBASE)/libutil/writeall.c \
	      $(SBASE)/libutf/rune.c $(SBASE)/libutf/runetype.c $(SBASE)/libutf/isalnumrune.c \
	      $(SBASE)/libutf/isalpharune.c $(SBASE)/libutf/isdigitrune.c $(SBASE)/libutf/isblankrune.c \
	      $(SBASE)/libutf/isprintrune.c $(SBASE)/libutf/iscntrlrune.c $(SBASE)/libutf/lowerrune.c $(LIBAIOS)
sbase-cp: $(SBASE)/cp.c $(SBASE_DEPS) $(SBASE)/fs.h
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/cp.c $(CP_OBJS) $(LIBAIOS)
sbase-mv: $(SBASE)/mv.c $(SBASE_DEPS) $(SBASE)/fs.h
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/mv.c $(CP_OBJS) $(SBASE)/libutil/rm.c $(LIBAIOS)
sbase-ln: $(SBASE)/ln.c $(SBASE_DEPS)
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/ln.c $(SBASE)/libutil/eprintf.c $(LIBAIOS)
sbase-chmod: $(SBASE)/chmod.c $(SBASE_DEPS) $(SBASE)/fs.h
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/chmod.c \
	      $(SBASE)/libutil/eprintf.c $(SBASE)/libutil/ealloc.c $(SBASE)/libutil/mode.c \
	      $(SBASE)/libutil/recurse.c $(SBASE)/libutil/strlcpy.c $(SBASE)/libutil/strtonum.c $(LIBAIOS)
# grep: the first regex-using utility. eregcomp.c calls regcomp/regerror, now REAL in libaios; the
# pattern FILEs come from libaios fmemopen. The BRE/ERE engine lives in libaios -- grep is UNMODIFIED.
sbase-grep: $(SBASE)/grep.c $(SBASE_DEPS) $(SBASE)/queue.h
	$(CC) $(SBASE_CFLAGS) -o $@ $(SBASE)/grep.c \
	      $(SBASE)/libutil/eprintf.c $(SBASE)/libutil/ealloc.c \
	      $(SBASE)/libutil/eregcomp.c $(SBASE)/libutil/fshut.c $(SBASE)/libutil/strcasestr.c $(LIBAIOS)

# dash = the operational shell. One cc invocation over the dash sources + generated sources + libaios.
dash: $(DASH_CFILES) $(LIBAIOS) $(LIBC_HDRS) $(DASH)/config.h
	$(CC) $(DASH_CFLAGS) -o $@ $(DASH_CFILES) $(LIBAIOS)

run: all
	./aios-uk ./prog_tail -n 3 /etc/os-release

clean:
	rm -f aios-uk guest_hello guest_fileio guest_cat guest_escape prog_args prog_wc prog_bigalloc prog_tail \
	      prog_exec prog_fork prog_spawn prog_pipe prog_pipeline prog_pipebig prog_sh prog_libc \
	      prog_stdio prog_errno prog_fs prog_getopt prog_dir prog_signal prog_jail prog_execjail prog_clock prog_pcwd prog_umask prog_regex prog_pwgrp prog_jobctl prog_stop $(SBASE_BINS) dash
