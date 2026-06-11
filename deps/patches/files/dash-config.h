/* config.h -- AIOS cross-compile configuration for dash
 * Generated for musl-libc on AArch64 (seL4 microkernel).
 * v0.4.64: dash port Phase 1
 */
#ifndef DASH_CONFIG_H
#define DASH_CONFIG_H

/* Expose GNU extensions in musl: mempcpy, strchrnul, stpcpy */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* Shell identity */
#define EXECSHELL "/bin/sh"
#define _PATH_BSHELL "/bin/sh"
#define _PATH_DEVNULL "/dev/null"
#define _PATH_TTY "/dev/tty"
#define BSD 1

/* Disable job control -- AIOS has no terminal process groups yet */
#define JOBS 0

/* Minimize footprint */
#define SMALL 1

/* Use internal glob (avoid libc glob dependencies) */
#define GLOB_BROKEN 1

/* musl libc on AArch64 provides all of these */
#define HAVE_ISALPHA 1
#define HAVE_MEMPCPY 1
#define HAVE_STPCPY 1
#define HAVE_STRCHRNUL 1
#define HAVE_BSEARCH 1
#define HAVE_SYSCONF 1
#define HAVE_GETPWNAM 1
#define HAVE_GETRLIMIT 1
#define HAVE_FACCESSAT 1
#define HAVE_KILLPG 1
#define HAVE_STRSIGNAL 1
#define HAVE_PATHS_H 1

/* musl AArch64 is natively 64-bit, no _64 variants needed */
#define fstat64 fstat
#define lstat64 lstat
#define stat64 stat
#define open64 open
#define readdir64 readdir
#define dirent64 dirent

/* musl provides these declarations */
#define HAVE_DECL_ISBLANK 1

/* Signal handling */
#define HAVE_SIGSETJMP 1

/* Compiler attributes musl/GCC support */
#define HAVE_ALIAS_ATTRIBUTE 1

/* intmax_t format string */
/* PRIdMAX -- provided by musl inttypes.h, do not redefine */

/* stat struct has st_mtim (not st_mtimespec) */
#define HAVE_ST_MTIM 1

/* musl provides strtod -- suppress system.h inline version */
#define HAVE_STRTOD 1

#endif /* DASH_CONFIG_H */
