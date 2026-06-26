/* sys/time.h -- AIOS shadow header. struct timeval + gettimeofday (no clock yet -> zero). struct
 * timeval MUST match the libaios gettimeofday signature (two longs). */
#ifndef _SYS_TIME_H
#define _SYS_TIME_H
#include <sys/types.h>
#include <time.h>

struct timeval {
    time_t tv_sec;
    long   tv_usec;
};
struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval *tv, void *tz);

#endif /* _SYS_TIME_H */
