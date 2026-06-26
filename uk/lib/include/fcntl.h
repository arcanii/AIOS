/* fcntl.h -- AIOS shadow header (see sys/types.h). open() + the O_* flags AIOS programs use; the
 * values are AIOS-owned (aios_abi.h, via libaios) and the kernel/PAL translate to the host's. */
#ifndef _FCNTL_H
#define _FCNTL_H
#include <sys/types.h>   /* mode_t (creat) */
#include "aios_abi.h"

#define O_RDONLY    AIOS_O_RDONLY
#define O_WRONLY    AIOS_O_WRONLY
#define O_RDWR      AIOS_O_RDWR
#define O_CREAT     AIOS_O_CREAT
#define O_TRUNC     AIOS_O_TRUNC
#define O_APPEND    AIOS_O_APPEND
#define O_CLOEXEC   AIOS_O_CLOEXEC
#define O_DIRECTORY AIOS_O_DIRECTORY
#define O_EXCL      AIOS_O_EXCL
#define O_NONBLOCK  AIOS_O_NONBLOCK
#define O_NDELAY    AIOS_O_NONBLOCK

/* *at directory-fd resolution + flags */
#define AT_FDCWD            AIOS_AT_FDCWD
#define AT_SYMLINK_NOFOLLOW AIOS_AT_SYMLINK_NOFOLLOW
#define AT_REMOVEDIR        AIOS_AT_REMOVEDIR
#define AT_EACCESS          0x200

/* fcntl commands + fd flags */
#define F_DUPFD         AIOS_F_DUPFD
#define F_GETFD         AIOS_F_GETFD
#define F_SETFD         AIOS_F_SETFD
#define F_GETFL         AIOS_F_GETFL
#define F_SETFL         AIOS_F_SETFL
#define F_DUPFD_CLOEXEC AIOS_F_DUPFD_CLOEXEC
#define FD_CLOEXEC      AIOS_FD_CLOEXEC

int open(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int cmd, ...);

#endif /* _FCNTL_H */
