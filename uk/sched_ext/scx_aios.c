// SPDX-License-Identifier: GPL-2.0
/*
 * scx_aios.c -- the userspace loader for the AIOS sched_ext scheduler (scx_aios.bpf.c).
 *
 * It opens + loads the BPF scheduler, attaches its `struct sched_ext_ops` (which switches the kernel to
 * AIOS's scheduling policy), then holds it attached until interrupted -- on exit the kernel reverts to
 * its default scheduler. This is HOST (Linux) code, not an AIOS-ABI program -- it is how the AIOS system
 * installs its scheduling policy on the substrate. Requires root + CONFIG_SCHED_CLASS_EXT.
 *
 * Build: `make` (needs clang + bpftool + libbpf-dev + the target kernel's vmlinux.h) -- see the Makefile.
 */
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include "scx_aios.bpf.skel.h"

static volatile sig_atomic_t exit_req;
static void on_sig(int sig) { (void)sig; exit_req = 1; }

static int print_fn(enum libbpf_print_level level, const char *fmt, va_list ap)
{
	if (level == LIBBPF_DEBUG)
		return 0;
	return vfprintf(stderr, fmt, ap);
}

int main(void)
{
	struct scx_aios_bpf *skel;
	struct bpf_link *link;

	libbpf_set_print(print_fn);
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	skel = scx_aios_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "scx_aios: open_and_load failed (need root + CONFIG_SCHED_CLASS_EXT)\n");
		return 1;
	}

	link = bpf_map__attach_struct_ops(skel->maps.aios_ops);
	if (!link) {
		fprintf(stderr, "scx_aios: attach_struct_ops failed (need root; another SCX scheduler loaded?)\n");
		scx_aios_bpf__destroy(skel);
		return 1;
	}

	printf("scx_aios: the AIOS sched_ext scheduler is ATTACHED -- the kernel now schedules by AIOS policy.\n");
	printf("scx_aios: (check: cat /sys/kernel/sched_ext/root/ops -> \"aios\"). Ctrl-C to detach.\n");
	fflush(stdout);

	while (!exit_req)
		sleep(1);

	printf("\nscx_aios: detaching -- the kernel reverts to its default scheduler.\n");
	bpf_link__destroy(link);
	scx_aios_bpf__destroy(skel);
	return 0;
}
