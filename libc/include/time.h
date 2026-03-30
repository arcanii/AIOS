#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>
#include <sys/types.h>

typedef long time_t;
typedef long clock_t;
typedef long suseconds_t;

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

#define CLOCKS_PER_SEC 1000000

time_t time(time_t *t);
clock_t clock(void);
struct tm *localtime(const time_t *timep);
struct tm *gmtime(const time_t *timep);
time_t mktime(struct tm *tm);
char *ctime(const time_t *timep);
char *asctime(const struct tm *tm);
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif
