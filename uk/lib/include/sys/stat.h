/* sys/stat.h -- AIOS shadow header (see sys/types.h). `struct stat` MUST match `struct aios_stat`
 * (aios_abi.h) byte for byte -- the kernel fills those bytes and the program reads them here. */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H
#include <sys/types.h>
#include "aios_abi.h"

struct stat {
    unsigned long long st_dev;
    unsigned long long st_ino;
    unsigned int       st_mode;
    unsigned int       st_nlink;
    unsigned int       st_uid;
    unsigned int       st_gid;
    unsigned int       _pad;
    long long          st_size;
    long long          st_blksize;
    long long          st_blocks;
    long long          st_atime;
    long long          st_mtime;
    long long          st_ctime;
};

#define S_IFMT   AIOS_S_IFMT
#define S_IFREG  AIOS_S_IFREG
#define S_IFDIR  AIOS_S_IFDIR
#define S_IFLNK  AIOS_S_IFLNK
#define S_IFCHR  AIOS_S_IFCHR
#define S_IFBLK  AIOS_S_IFBLK
#define S_IFIFO  AIOS_S_IFIFO
#define S_IFSOCK AIOS_S_IFSOCK
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* permission + special bits (standard octal values) */
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

int    stat (const char *path, struct stat *st);
int    lstat(const char *path, struct stat *st);
int    fstat(int fd, struct stat *st);
int    mkdir(const char *path, mode_t mode);
mode_t umask(mode_t mask);

#endif /* _SYS_STAT_H */
