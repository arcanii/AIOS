/* dirent.h -- AIOS shadow header (see sys/types.h). Directory streams, implemented in libaios over
 * AIOS_SYS_GETDENTS. `struct dirent` + `struct _AIOS_DIR` MUST stay byte-identical to the copies in
 * libaios.c -- readdir fills the bytes the program reads (same rule as struct stat <-> aios_stat). */
#ifndef _DIRENT_H
#define _DIRENT_H
#include <sys/types.h>

struct dirent {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[256];
};
typedef struct _AIOS_DIR DIR;       /* opaque -- the concrete struct lives in libaios.c */

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *d);
int            closedir(DIR *d);

/* d_type values (BSD/Linux DT_*). */
#define DT_UNKNOWN   0
#define DT_FIFO      1
#define DT_CHR       2
#define DT_DIR       4
#define DT_BLK       6
#define DT_REG       8
#define DT_LNK      10
#define DT_SOCK     12

#endif /* _DIRENT_H */
