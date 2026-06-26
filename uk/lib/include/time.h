/* time.h -- AIOS shadow header (see sys/types.h). UTC-only, no timezone, no RTC: localtime ==
 * gmtime, and time() returns a fixed 0 "now" (a real clock syscall is future work). struct tm +
 * struct timespec MUST match the copies libaios fills. Enough for ls -l timestamps. */
#ifndef _TIME_H
#define _TIME_H
#include <stddef.h>
#include <sys/types.h>

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};

time_t     time(time_t *t);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
size_t     strftime(char *s, size_t max, const char *fmt, const struct tm *tm);

#endif /* _TIME_H */
