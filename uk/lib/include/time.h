/* time.h -- AIOS shadow header (see sys/types.h). UTC-only, no timezone: localtime == gmtime. time()
 * and clock_gettime() now read a REAL clock (AIOS_SYS_CLOCK_GETTIME via the PAL). struct tm +
 * struct timespec MUST match the copies libaios fills. */
#ifndef _TIME_H
#define _TIME_H
#include <stddef.h>
#include <sys/types.h>
#include "aios_abi.h"   /* CLOCK_* ids */

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};

typedef int clockid_t;
#define CLOCK_REALTIME  AIOS_CLOCK_REALTIME
#define CLOCK_MONOTONIC AIOS_CLOCK_MONOTONIC

time_t     time(time_t *t);
int        clock_gettime(clockid_t clk_id, struct timespec *ts);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
size_t     strftime(char *s, size_t max, const char *fmt, const struct tm *tm);

#endif /* _TIME_H */
