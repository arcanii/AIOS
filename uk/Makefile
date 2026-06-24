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

.PHONY: all run clean
all: aios-uk guest_hello guest_fileio guest_cat prog_args

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

run: all
	./aios-uk ./prog_args first second

clean:
	rm -f aios-uk guest_hello guest_fileio guest_cat prog_args
