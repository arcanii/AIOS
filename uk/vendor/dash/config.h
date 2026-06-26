/* config.h -- AIOS cross-compile configuration for dash 0.5.11 (force-included via -include).
 *
 * AIOS build input, not upstream dash. Tuned to libaios (the AIOS-ABI libc): HAVE_* is set 1 only
 * for what libaios provides, and 0/undef for what dash should self-provide via system.c (so libaios
 * stays small). JOBS 0 (no terminal process groups yet), SMALL, GLOB_BROKEN (dash's internal glob).
 */
#ifndef DASH_CONFIG_H
#define DASH_CONFIG_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* Shell identity / paths */
#define EXECSHELL    "/bin/sh"
#define _PATH_BSHELL "/bin/sh"
#define _PATH_DEVNULL "/dev/null"
#define _PATH_TTY    "/dev/tty"
#define BSD 1

/* No terminal job control yet; minimize; use dash's internal glob */
#define JOBS 0
#define SMALL 1
#define GLOB_BROKEN 1

/* libaios provides these */
#define HAVE_ISALPHA 1
#define HAVE_BSEARCH 1
#define HAVE_SYSCONF 1
#define HAVE_GETPWNAM 1
#define HAVE_GETRLIMIT 1
#define HAVE_FACCESSAT 1
#define HAVE_DECL_ISBLANK 1
#define HAVE_ST_MTIM 1
#define HAVE_SIGSETJMP 1
#define HAVE_ALIAS_ATTRIBUTE 1
#define HAVE_STRTOIMAX 1
#define HAVE_STRTOUMAX 1
#define HAVE_STRSIGNAL 1   /* libaios provides strsignal (dash's would need sys_siglist) */
#define HAVE_STRTOD 1      /* libaios now has a real strtod -- use it, not dash's no-op fallback */

/* dash self-provides these via system.c (libaios lacks them): leave HAVE_* undefined --
 * HAVE_MEMPCPY, HAVE_STPCPY, HAVE_STRCHRNUL, HAVE_KILLPG, HAVE_STRTOD, HAVE_PATHS_H, HAVE_SIGSETMASK. */

/* 64-bit native: no _64 variants */
#define fstat64 fstat
#define lstat64 lstat
#define stat64 stat
#define open64 open
#define readdir64 readdir
#define dirent64 dirent

#endif /* DASH_CONFIG_H */
