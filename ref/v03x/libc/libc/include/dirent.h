#ifndef _DIRENT_H
#define _DIRENT_H
#include <stddef.h>
#include <sys/types.h>

#define NAME_MAX 255

struct dirent {
    ino_t  d_ino;
    char   d_name[NAME_MAX + 1];
};

typedef struct {
    int _fd;
    int _pos;
} DIR;

DIR           *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int            closedir(DIR *dirp);
#endif
