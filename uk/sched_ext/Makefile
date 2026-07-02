# scx_aios -- AIOS's own scheduling policy as a sched_ext (SCX) BPF scheduler.
#
# BUILD needs: clang (BPF target) + bpftool (skeleton gen) + libbpf-dev + the TARGET kernel's vmlinux.h
# (generated from its BTF). LOADING (`make load` / ./scx_aios) needs ROOT + a kernel with
# CONFIG_SCHED_CLASS_EXT=y (the RPi5's stock Ubuntu-26.04 kernel has it).
#
# On the target (native): `make` generates vmlinux.h from /sys/kernel/btf/vmlinux automatically.
# Cross-building (e.g. in a container) against another kernel's BTF: `make BTF=vmlinux.btf`.

CLANG   ?= clang
BPFTOOL ?= bpftool
CC      ?= cc
ARCH    ?= arm64
BTF     ?= /sys/kernel/btf/vmlinux

BPF_CFLAGS = -g -O2 -Wall -target bpf -D__TARGET_ARCH_$(ARCH) -I.

.PHONY: all load clean
all: scx_aios

# vmlinux.h -- the target kernel's type definitions, dumped from its BTF (CO-RE).
vmlinux.h:
	$(BPFTOOL) btf dump file $(BTF) format c > $@

scx_aios.bpf.o: scx_aios.bpf.c vmlinux.h
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

scx_aios.bpf.skel.h: scx_aios.bpf.o
	$(BPFTOOL) gen skeleton $< name scx_aios_bpf > $@

scx_aios: scx_aios.c scx_aios.bpf.skel.h
	$(CC) -O2 -Wall -o $@ scx_aios.c -lbpf -lelf

# Load the scheduler (root). It stays attached until Ctrl-C.
load: scx_aios
	sudo ./scx_aios

clean:
	rm -f scx_aios scx_aios.bpf.o scx_aios.bpf.skel.h vmlinux.h
