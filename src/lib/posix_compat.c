/*
 * AIOS POSIX Shim -- Linux compatibility syscalls
 * v0.4.78: select/poll stubs, prlimit64, getrandom, prctl, sysinfo
 *
 * These are commonly probed by musl libc and standard programs.
 * Providing stubs (even returning ENOSYS for unimplemented features)
 * prevents hard crashes from unhandled syscall traps.
 */
#include "posix_internal.h"
#include <string.h>

/* AArch64 syscall numbers for compat layer */
#ifndef __NR_ppoll
#define __NR_ppoll 73
#endif
#ifndef __NR_pselect6
#define __NR_pselect6 72
#endif
#ifndef __NR_getrandom
#define __NR_getrandom 278
#endif
#ifndef __NR_prlimit64
#define __NR_prlimit64 261
#endif
#ifndef __NR_prctl
#define __NR_prctl 167
#endif
#ifndef __NR_getrlimit
#define __NR_getrlimit 163
#endif
#ifndef __NR_setrlimit
#define __NR_setrlimit 164
#endif
#ifndef __NR_sysinfo
#define __NR_sysinfo 179
#endif
#ifndef __NR_getrusage
#define __NR_getrusage 165
#endif
#ifndef __NR_membarrier
#define __NR_membarrier 283
#endif
#ifndef __NR_futex
#define __NR_futex 98
#endif

/* ---- ppoll: stub that returns immediately with 0 ready fds ----
 * Many programs use poll() to check if stdin is readable.
 * Returning 0 (timeout, nothing ready) is safe -- caller retries or
 * falls back to blocking read.
 */
long aios_sys_ppoll(va_list ap) {
    (void)ap;
    return 0;  /* no fds ready -- timeout */
}

/* ---- pselect6: same approach as ppoll ---- */
long aios_sys_pselect6(va_list ap) {
    (void)ap;
    return 0;  /* no fds ready */
}

/* ---- getrandom: fill buffer from ARM counter-based PRNG ----
 * Not cryptographically strong, but sufficient for seeding
 * userspace PRNGs (arc4random init, stack canaries, etc.)
 */
long aios_sys_getrandom(va_list ap) {
    char *buf = va_arg(ap, char *);
    size_t count = va_arg(ap, size_t);
    /* flags ignored -- we always return data */

    if (!buf) return -14;  /* EFAULT */
    if (count > 256) count = 256;

    /* Use AArch64 CNTPCT as entropy source, mixed with XOR */
    uint64_t state;
    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(state));
    /* Simple splitmix64 PRNG seeded from counter */
    for (size_t i = 0; i < count; i++) {
        state += 0x9E3779B97F4A7C15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        buf[i] = (char)(z & 0xFF);
    }
    return (long)count;
}

/* ---- prlimit64: resource limits ----
 * Returns generous defaults. Programs probe this to check
 * stack size, open file limits, etc.
 */
long aios_sys_prlimit64(va_list ap) {
    int pid = va_arg(ap, int);
    int resource = va_arg(ap, int);
    void *new_rlim = va_arg(ap, void *);
    void *old_rlim = va_arg(ap, void *);
    (void)pid;
    (void)resource;
    (void)new_rlim;

    if (old_rlim) {
        /* struct rlimit64 { uint64_t rlim_cur, rlim_max; } */
        uint64_t *r = (uint64_t *)old_rlim;
        /* RLIMIT_NOFILE=7: report 32 (matches AIOS_MAX_FDS) */
        /* Everything else: report 8MB / unlimited */
        if (resource == 7) {
            r[0] = 32;             /* rlim_cur */
            r[1] = 32;             /* rlim_max */
        } else {
            r[0] = 8 * 1024 * 1024;   /* 8 MB soft */
            r[1] = 0xFFFFFFFFFFFFFFFFULL;  /* unlimited hard */
        }
    }
    return 0;
}

/* ---- prctl: process control ----
 * Stub: PR_SET_NAME (15) accepted silently, others return 0.
 */
long aios_sys_prctl(va_list ap) {
    (void)ap;
    return 0;
}

/* ---- getrlimit/setrlimit: legacy rlimit ---- */
long aios_sys_getrlimit(va_list ap) {
    int resource = va_arg(ap, int);
    void *rlim = va_arg(ap, void *);
    if (rlim) {
        uint64_t *r = (uint64_t *)rlim;
        if (resource == 7) {
            r[0] = 32; r[1] = 32;
        } else {
            r[0] = 8 * 1024 * 1024;
            r[1] = 0xFFFFFFFFFFFFFFFFULL;
        }
    }
    return 0;
}

long aios_sys_setrlimit(va_list ap) {
    (void)ap;
    return 0;  /* accept silently */
}

/* ---- sysinfo: system information ----
 * struct sysinfo { long uptime; unsigned long loads[3]; ... }
 * Fills minimal fields from ARM counter.
 */
long aios_sys_sysinfo(va_list ap) {
    void *info = va_arg(ap, void *);
    if (!info) return -14;

    /* Zero the struct (at least 64 bytes on AArch64) */
    uint8_t *p = (uint8_t *)info;
    for (int i = 0; i < 128; i++) p[i] = 0;

    /* uptime in seconds from ARM counter */
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(cnt));
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    long uptime = (long)(cnt / freq);

    /* struct sysinfo: first field is long uptime */
    long *lp = (long *)info;
    lp[0] = uptime;
    /* totalram (offset 32 on AArch64) */
    unsigned long *ulp = (unsigned long *)info;
    ulp[4] = 2UL * 1024 * 1024 * 1024;  /* 2 GB */
    ulp[5] = 1UL * 1024 * 1024 * 1024;  /* 1 GB free (estimate) */
    /* mem_unit (offset 104 on AArch64) */
    unsigned int *uip = (unsigned int *)((char *)info + 104);
    uip[0] = 1;

    return 0;
}

/* ---- getrusage: resource usage (stub) ---- */
long aios_sys_getrusage(va_list ap) {
    int who = va_arg(ap, int);
    void *usage = va_arg(ap, void *);
    (void)who;
    if (usage) {
        uint8_t *p = (uint8_t *)usage;
        for (int i = 0; i < 144; i++) p[i] = 0;
    }
    return 0;
}

/* ---- membarrier: memory barrier (stub) ----
 * cmd=0 (MEMBARRIER_CMD_QUERY): return supported commands bitmask
 * Others: return 0 (success)
 */
long aios_sys_membarrier(va_list ap) {
    int cmd = va_arg(ap, int);
    (void)cmd;
    return 0;
}
/* ---- futex: fast userspace mutex (stub) ----
 * FUTEX_WAIT (0): compare *uaddr with val; if equal return -ETIMEDOUT
 * FUTEX_WAKE (1): return 0 (no waiters to wake)
 * Others: return -ENOSYS
 * Required by musl threading internals and pthreads.
 */
#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#endif

long aios_sys_futex(va_list ap) {
    int *uaddr = va_arg(ap, int *);
    int futex_op = va_arg(ap, int);
    int val = va_arg(ap, int);
    int op = futex_op & 0x7F;  /* mask out FUTEX_PRIVATE_FLAG */
    if (op == FUTEX_WAKE) {
        (void)uaddr; (void)val;
        return 0;  /* no waiters woken */
    }
    if (op == FUTEX_WAIT) {
        if (uaddr && *uaddr != val) return -EAGAIN;
        /* Value matches -- would block; return timeout */
        return -110;  /* ETIMEDOUT */
    }
    return -ENOSYS;
}

