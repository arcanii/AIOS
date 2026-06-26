/* errno.h -- AIOS shadow header (see sys/types.h). `errno` is set by the POSIX wrappers in libaios
 * from a failing syscall's negated return (-errno). The E* values are the AIOS error numbers
 * (aios_abi.h), which match Linux's. */
#ifndef _ERRNO_H
#define _ERRNO_H
#include "aios_abi.h"

extern int errno;

#define EPERM        AIOS_EPERM
#define ENOENT       AIOS_ENOENT
#define ESRCH        AIOS_ESRCH
#define EINTR        AIOS_EINTR
#define EIO          AIOS_EIO
#define EBADF        AIOS_EBADF
#define ECHILD       AIOS_ECHILD
#define EAGAIN       AIOS_EAGAIN
#define EWOULDBLOCK  AIOS_EAGAIN
#define ENOMEM       AIOS_ENOMEM
#define EACCES       AIOS_EACCES
#define EFAULT       AIOS_EFAULT
#define EBUSY        AIOS_EBUSY
#define EEXIST       AIOS_EEXIST
#define ENOTDIR      AIOS_ENOTDIR
#define EISDIR       AIOS_EISDIR
#define EINVAL       AIOS_EINVAL
#define EMFILE       AIOS_EMFILE
#define ESPIPE       AIOS_ESPIPE
#define EPIPE        AIOS_EPIPE
#define ERANGE       AIOS_ERANGE
#define ENAMETOOLONG AIOS_ENAMETOOLONG
#define ENOSYS       AIOS_ENOSYS
#define ENXIO        AIOS_ENXIO
#define E2BIG        AIOS_E2BIG
#define ENOEXEC      AIOS_ENOEXEC
#define EXDEV        AIOS_EXDEV
#define ENOTTY       AIOS_ENOTTY
#define ETXTBSY      AIOS_ETXTBSY
#define ENOSPC       AIOS_ENOSPC
#define EDOM         AIOS_EDOM
#define ENOTEMPTY    AIOS_ENOTEMPTY
#define ELOOP        AIOS_ELOOP

#endif /* _ERRNO_H */
