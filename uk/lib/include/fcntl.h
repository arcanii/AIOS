/* fcntl.h -- AIOS shadow header (see sys/types.h). open() + the O_* flags AIOS programs use; the
 * values are AIOS-owned (aios_abi.h, via libaios) and the kernel/PAL translate to the host's. */
#ifndef _FCNTL_H
#define _FCNTL_H
#include "aios_abi.h"

#define O_RDONLY    AIOS_O_RDONLY
#define O_WRONLY    AIOS_O_WRONLY
#define O_RDWR      AIOS_O_RDWR
#define O_CREAT     AIOS_O_CREAT
#define O_TRUNC     AIOS_O_TRUNC
#define O_APPEND    AIOS_O_APPEND
#define O_CLOEXEC   AIOS_O_CLOEXEC
#define O_DIRECTORY AIOS_O_DIRECTORY

/* *at directory-fd resolution + flags */
#define AT_FDCWD            AIOS_AT_FDCWD
#define AT_SYMLINK_NOFOLLOW AIOS_AT_SYMLINK_NOFOLLOW
#define AT_REMOVEDIR        AIOS_AT_REMOVEDIR

int open(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);

#endif /* _FCNTL_H */
