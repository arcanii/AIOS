/* sys/times.h -- AIOS shadow header. dash's `times` builtin. No clock yet -> all fields zero.
 * struct tms MUST match the libaios times() signature (four longs). */
#ifndef _SYS_TIMES_H
#define _SYS_TIMES_H
#include <sys/types.h>

typedef long clock_t;

struct tms {
    clock_t tms_utime;
    clock_t tms_stime;
    clock_t tms_cutime;
    clock_t tms_cstime;
};

clock_t times(struct tms *buf);

#endif /* _SYS_TIMES_H */
