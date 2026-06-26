/*
 * prog_clock.c -- proves the real time source (AIOS_SYS_CLOCK_GETTIME): time() / clock_gettime() /
 * gettimeofday() now read a live clock through the PAL (they used to return a fixed 0). Checks the
 * wall clock is a real modern epoch, formats it through the gmtime+strftime layer (so the whole
 * stack -- syscall -> civil-from-days -> strftime -- is exercised), and confirms the MONOTONIC clock
 * advances across a busy loop. Plain C via the shadow headers. Exits 0 iff all checks pass.
 */
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#define MODERN 1700000000L   /* 2023-11-14 -- any real clock is well past this */

int main(void) {
    int fails = 0;

    time_t t = time(NULL);
    printf("time() epoch = %ld\n", (long)t);
    if (t < MODERN) { printf("  FAIL: time() is not a real modern wall-clock value\n"); fails++; }
    else            printf("  ok: a real, modern wall-clock time\n");

    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC", gmtime(&t));
    printf("formatted (clock -> gmtime -> strftime) = %s\n", buf);

    struct timespec r, m1, m2;
    clock_gettime(CLOCK_REALTIME, &r);
    if (r.tv_sec < MODERN) { printf("  FAIL: CLOCK_REALTIME not modern\n"); fails++; }
    else                   printf("  ok: CLOCK_REALTIME = %ld.%09ld\n", (long)r.tv_sec, (long)r.tv_nsec);

    clock_gettime(CLOCK_MONOTONIC, &m1);
    volatile long spin = 0;
    for (long i = 0; i < 30000000L; i++) spin += i;     /* burn a little wall time */
    clock_gettime(CLOCK_MONOTONIC, &m2);
    long us = (long)((m2.tv_sec - m1.tv_sec) * 1000000L + (m2.tv_nsec - m1.tv_nsec) / 1000);
    printf("CLOCK_MONOTONIC advanced %ld us over a busy loop\n", us);
    if (us <= 0) { printf("  FAIL: monotonic clock did not advance\n"); fails++; }
    else         printf("  ok: monotonic clock advances\n");

    struct timeval tv;
    gettimeofday(&tv, 0);
    printf("gettimeofday = %ld.%06ld\n", (long)tv.tv_sec, (long)tv.tv_usec);
    if (tv.tv_sec < MODERN) { printf("  FAIL: gettimeofday not modern\n"); fails++; }

    printf("prog_clock: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
