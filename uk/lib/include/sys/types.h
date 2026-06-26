/* sys/types.h -- AIOS shadow header (compiled with -nostdinc so real sources pick this up instead
 * of the host libc's). The POSIX-ish C surface AIOS programs see is implemented by libaios on the
 * AIOS ABI; these headers just declare it. Keep them minimal -- grow as sbase/dash need more. */
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H
#include <stddef.h>          /* size_t (from the compiler's freestanding header) */

typedef long               ssize_t;
typedef int                pid_t;
typedef long               off_t;
typedef unsigned int       mode_t;
typedef unsigned int       uid_t;
typedef unsigned int       gid_t;
typedef long               time_t;
typedef unsigned long long dev_t;   /* matches struct stat st_dev */
typedef unsigned long long ino_t;   /* matches struct stat st_ino */
typedef long long          blkcnt_t;
typedef long               blksize_t;
typedef unsigned int       nlink_t;

#endif /* _SYS_TYPES_H */
