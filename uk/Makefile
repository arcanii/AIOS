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

.PHONY: all run clean
all: aios-uk guest_hello guest_fileio guest_cat prog_args prog_wc prog_bigalloc prog_tail prog_exec \
     prog_fork prog_spawn prog_pipe prog_pipeline prog_pipebig prog_sh prog_libc

aios-uk: $(KERNEL_SRC) include/pal.h include/aios_abi.h
	$(CC) $(KERNEL_CFLAGS) -o $@ $(KERNEL_SRC)

guest_hello: guest/guest_hello.c include/aios_abi.h
	$(CC) $(GUEST_CFLAGS) -o $@ guest/guest_hello.c

guest_fileio: guest/guest_fileio.c include/aios_abi.h
	$(CC) $(GUEST_CFLAGS) -o $@ guest/guest_fileio.c

guest_cat: guest/guest_cat.c include/aios_abi.h
	$(CC) $(GUEST_CFLAGS) -o $@ guest/guest_cat.c

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

run: all
	./aios-uk ./prog_tail -n 3 /etc/os-release

clean:
	rm -f aios-uk guest_hello guest_fileio guest_cat prog_args prog_wc prog_bigalloc prog_tail \
	      prog_exec prog_fork prog_spawn prog_pipe prog_pipeline prog_pipebig prog_sh prog_libc
