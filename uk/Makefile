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

# The guest is freestanding: no libc, no startfiles, its own _start.
GUEST_CFLAGS = -O2 $(WARN) $(INCLUDE) -ffreestanding -nostdlib -static \
               -fno-stack-protector -fno-asynchronous-unwind-tables

.PHONY: all run clean
all: aios-uk guest_hello

aios-uk: $(KERNEL_SRC) include/pal.h include/aios_abi.h
	$(CC) $(KERNEL_CFLAGS) -o $@ $(KERNEL_SRC)

guest_hello: guest/guest_hello.c include/aios_abi.h
	$(CC) $(GUEST_CFLAGS) -o $@ guest/guest_hello.c

run: all
	./aios-uk ./guest_hello

clean:
	rm -f aios-uk guest_hello
