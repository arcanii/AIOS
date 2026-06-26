/*
 * aios_version.h -- the AIOS userspace-kernel version.
 *
 * 0.5.x is a NEW design line: the gVisor-style userspace kernel on a commodity host (the 2026-06-24
 * pivot, docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md). It deliberately departs from the
 * 0.4.x seL4/RPi4 bare-metal line (preserved on `main` as the record/fallback), so it gets its own
 * major.minor. Patch advances per milestone. 0.5.0 covered M0..M3e (Linux substrate, the trap/VFS
 * foundation, the full process model, and the start of the libc retarget). 0.5.1 adds the rest of
 * M3e (errno, sys/stat, getopt/qsort, directory streams) + the *at family + readlink + a real
 * printf and time layer, and VENDORED sbase whose true/false/echo/cat/wc/mkdir/rm/ls compile
 * UNMODIFIED against AIOS's libc and run on the kernel. 0.5.2 reaches "operational": VENDORED dash
 * (the Debian Almquist Shell) compiles UNMODIFIED and runs as a real shell -- builtins, arithmetic,
 * control flow, loops, pipelines, command substitution, redirection (+ setjmp/signals/fcntl libc).
 * 30-syscall ABI. 0.5.3 = M4 boundary enforcement: the trap model is now SOUND -- the PAL
 * neutralizes every trapped syscall so a guest-chosen syscall NEVER executes on the host, and the
 * kernel kills any guest that emits a non-AIOS (real Linux) syscall (an escape attempt); only the
 * kernel's own injections reach Linux. 0.5.4 = M5 real signal delivery: the kernel runs a guest's
 * handler (sigaction/kill/sigreturn + a frame dance in the PAL), so dash trap/kill work AND
 * INTERACTIVE dash + ^C work -- ^C interrupts the prompt and dash survives (a do_read single-read
 * fix made interactive mode function). 0.5.5 = M4.2 the OTHER half of the boundary: filesystem
 * confinement. When the PAL is launched with AIOS_ROOT set, every guest file path is resolved INSIDE
 * that root via openat2(RESOLVE_IN_ROOT) -- absolute paths, ".." traversal, and symlinks (absolute
 * or "..") are clamped to the root -- so a serviced open()/stat()/... can reach ONLY an AIOS root,
 * never arbitrary host paths. It is an UNPRIVILEGED primitive (no chroot/CAP), purely a PAL policy:
 * the kernel + ABI are UNCHANGED (zero new syscalls). Proof: guest/prog_jail.c (every escape vector
 * denied; in-root access works). 0.5.6 = M4.3 exec confinement: a guest-issued exec (AIOS_SYS_EXEC)
 * is resolved INSIDE the root too (openat2 + canonical /proc/self/fd path), so a guest can only
 * launch binaries in its root; the INIT program the operator names is the trusted entry, exempt.
 * Proof: guest/prog_execjail.c (in-root binaries run; out-of-root host paths denied). 0.5.7 = four
 * more vendored sbase utilities run UNMODIFIED -- head / tail / cp / mv (+ libaios grew getline/
 * getdelim, creat, basename/dirname, llabs/sleep, and honest no-op/ENOSYS stubs for the file-metadata
 * ops cp/mv reach for; a real openat2-strict mode mask + a long-standing fopen-errno fix came with it).
 * 0.5.8 = a REAL clock: AIOS_SYS_CLOCK_GETTIME (ABI -> 35) reads the host clock via the PAL, so
 * time()/clock_gettime()/gettimeofday() are live (ls dates, dash timing); time() no longer returns 0.
 *
 * Host-agnostic by construction (pure version macros), so the kernel may include it without taking
 * on any host dependency.
 */
#ifndef AIOS_VERSION_H
#define AIOS_VERSION_H

#define AIOS_VERSION_MAJOR 0
#define AIOS_VERSION_MINOR 5
#define AIOS_VERSION_PATCH 8

#define _AIOS_STR(x)  #x
#define _AIOS_XSTR(x) _AIOS_STR(x)
#define AIOS_VERSION_STR \
    _AIOS_XSTR(AIOS_VERSION_MAJOR) "." _AIOS_XSTR(AIOS_VERSION_MINOR) "." _AIOS_XSTR(AIOS_VERSION_PATCH)

#define AIOS_VERSION_LINE "userspace kernel"   /* the 0.5.x design line */

#endif /* AIOS_VERSION_H */
