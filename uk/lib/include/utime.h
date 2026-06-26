/* utime.h -- AIOS shadow header (see sys/types.h). Legacy file-time setting; sbase's libutil/cp.c
 * includes it. utime() is a libaios no-op today (file times are not yet modeled -- see utimensat). */
#ifndef _UTIME_H
#define _UTIME_H
#include <sys/types.h>

struct utimbuf {
    time_t actime;     /* access time */
    time_t modtime;    /* modification time */
};

int utime(const char *path, const struct utimbuf *times);

#endif /* _UTIME_H */
