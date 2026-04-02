#ifndef _SYS_STAT_H
#define _SYS_STAT_H
#include <sys/types.h>
#include <stdint.h>

struct stat {
    ino_t     st_ino;
    mode_t    st_mode;
    uint32_t  st_nlink;
    uid_t     st_uid;
    gid_t     st_gid;
    off_t     st_size;
    time_t    st_atime;
    time_t    st_mtime;
    time_t    st_ctime;
    uint32_t  st_blksize;
    uint32_t  st_blocks;
};

#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int mkdir(const char *path, mode_t mode);
#endif
