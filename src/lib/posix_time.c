/*
 * posix_time.c -- AIOS POSIX time syscall handlers
 * clock_gettime, gettimeofday, nanosleep, times
 */
#include "posix_internal.h"

long aios_sys_clock_gettime(va_list ap) {
    int clk_id = va_arg(ap, int);
    struct timespec *tp = va_arg(ap, struct timespec *);
    (void)clk_id;
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    tp->tv_sec = (long)(cnt / freq);
    tp->tv_nsec = (long)((cnt % freq) * 1000000000ULL / freq);
    return 0;
}

long aios_sys_gettimeofday(va_list ap) {
    struct timeval *tv = va_arg(ap, struct timeval *);
    void *tz = va_arg(ap, void *);
    (void)tz;
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    tv->tv_sec = (long)(cnt / freq);
    tv->tv_usec = (long)((cnt % freq) * 1000000ULL / freq);
    return 0;
}

long aios_sys_nanosleep(va_list ap) {
    const struct timespec *req = va_arg(ap, const struct timespec *);
    struct timespec *rem = va_arg(ap, struct timespec *);
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    uint64_t target;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(target));
    target += (uint64_t)req->tv_sec * freq + (uint64_t)req->tv_nsec * freq / 1000000000ULL;
    uint64_t now;
    do {
        seL4_Yield();
        /* Signal Phase 3: check for cross-process signals */
        aios_sig_check();
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    } while (now < target);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

long aios_sys_times(va_list ap)
{
    struct { long tms_utime; long tms_stime; long tms_cutime; long tms_cstime; } *buf;
    buf = va_arg(ap, void *);
    if (buf) {
        buf->tms_utime = 0;
        buf->tms_stime = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    /* Return current clock ticks (monotonic approximation) */
    struct timespec ts;
    clock_gettime(0 /* CLOCK_MONOTONIC */, &ts);
    return (long)(ts.tv_sec * 100 + ts.tv_nsec / 10000000);
}
