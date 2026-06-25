/*
 * aios_version.h -- the AIOS userspace-kernel version.
 *
 * 0.5.x is a NEW design line: the gVisor-style userspace kernel on a commodity host (the 2026-06-24
 * pivot, docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md). It deliberately departs from the
 * 0.4.x seL4/RPi4 bare-metal line (preserved on `main` as the record/fallback), so it gets its own
 * major.minor. Patch advances per milestone. 0.5.0 covers M0..M3e: Linux substrate, the trap/VFS
 * foundation, the full process model (exec/fork/wait/pipe/dup2 -- a multi-process kernel + an AIOS
 * shell), and the start of the libc retarget (-nostdinc shadow headers + FILE* stdio).
 *
 * Host-agnostic by construction (pure version macros), so the kernel may include it without taking
 * on any host dependency.
 */
#ifndef AIOS_VERSION_H
#define AIOS_VERSION_H

#define AIOS_VERSION_MAJOR 0
#define AIOS_VERSION_MINOR 5
#define AIOS_VERSION_PATCH 0

#define _AIOS_STR(x)  #x
#define _AIOS_XSTR(x) _AIOS_STR(x)
#define AIOS_VERSION_STR \
    _AIOS_XSTR(AIOS_VERSION_MAJOR) "." _AIOS_XSTR(AIOS_VERSION_MINOR) "." _AIOS_XSTR(AIOS_VERSION_PATCH)

#define AIOS_VERSION_LINE "userspace kernel"   /* the 0.5.x design line */

#endif /* AIOS_VERSION_H */
