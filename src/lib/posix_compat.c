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
#include <poll.h>

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

/* ---- ppoll: real implementation for stdin + pipe fds (v0.4.99) ----
 * ZLE and other interactive programs use poll() to check stdin.
 * We query tty_server (TTY_POLL) for stdin and pipe_server for pipes.
 */
long aios_sys_ppoll(va_list ap) {
    struct pollfd *fds = va_arg(ap, struct pollfd *);
    unsigned int nfds = va_arg(ap, unsigned int);
    const struct timespec *tmo = va_arg(ap, const struct timespec *);
    /* sigmask arg ignored */

    if (!fds || nfds == 0) return 0;

    int ready = 0;
    for (unsigned int i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        int fd = fds[i].fd;
        short events = fds[i].events;

        if (fd < 0) continue;

        /* stdin (fd 0): ask tty_server via TTY_POLL */
        if (fd == 0 && (events & POLLIN)) {
            if (ser_ep) {
                seL4_MessageInfo_t reply = seL4_Call(ser_ep,
                    seL4_MessageInfo_new(76 /* TTY_POLL */, 0, 0, 0));
                int avail = (int)seL4_GetMR(0);
                if (avail > 0) {
                    fds[i].revents |= POLLIN;
                    ready++;
                }
            }
            continue;
        }

        /* stdout/stderr (fd 1,2): always writable */
        if ((fd == 1 || fd == 2) && (events & POLLOUT)) {
            fds[i].revents |= POLLOUT;
            ready++;
            continue;
        }

        /* AIOS fd: check tty, pipe readability */
        if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
            aios_fd_t *af = &aios_fds[fd - AIOS_FD_BASE];
            if (!af->active) {
                fds[i].revents |= POLLNVAL;
                ready++;
                continue;
            }
            /* v0.4.99: is_tty fd (zsh SHTTY): poll tty_server */
            if (af->is_tty && (events & POLLIN) && ser_ep) {
                seL4_MessageInfo_t reply = seL4_Call(ser_ep,
                    seL4_MessageInfo_new(76 /* TTY_POLL */, 0, 0, 0));
                int avail = (int)seL4_GetMR(0);
                if (avail > 0) {
                    fds[i].revents |= POLLIN;
                    ready++;
                }
                continue;
            }
            /* Pipe read end: non-blocking PIPE_READ with 0 bytes to probe */
            if (af->is_pipe && af->pipe_read && (events & POLLIN) && pipe_ep) {
                seL4_SetMR(0, (seL4_Word)af->pipe_id);
                seL4_SetMR(1, 0);  /* 0 bytes = probe */
                seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
                    seL4_MessageInfo_new(62 /* PIPE_READ */, 0, 0, 2));
                int result = (int)(long)seL4_GetMR(0);
                /* result >= 0 means data available or EOF; -11 = EAGAIN (empty) */
                if (result != -11) {
                    fds[i].revents |= POLLIN;
                    ready++;
                }
                continue;
            }
            /* Pipe write end or file: assume writable */
            if (events & POLLOUT) {
                fds[i].revents |= POLLOUT;
                ready++;
            }
            continue;
        }
    }

    /* If nothing ready and timeout is non-zero, sleep briefly and retry once */
    if (ready == 0 && tmo) {
        long ms = tmo->tv_sec * 1000 + tmo->tv_nsec / 1000000;
        if (ms > 0) {
            /* Sleep for min(ms, 50) then re-check stdin */
            long sleep_ms = ms < 50 ? ms : 50;
            struct timespec ts = { .tv_sec = 0, .tv_nsec = sleep_ms * 1000000 };
            /* Use nanosleep syscall directly */
            long __aios_nanosleep(const struct timespec *, struct timespec *);
            __aios_nanosleep(&ts, (void *)0);
            /* Re-check stdin only */
            for (unsigned int i = 0; i < nfds; i++) {
                if (fds[i].fd == 0 && (fds[i].events & POLLIN) && ser_ep) {
                    seL4_MessageInfo_t reply = seL4_Call(ser_ep,
                        seL4_MessageInfo_new(76 /* TTY_POLL */, 0, 0, 0));
                    int avail = (int)seL4_GetMR(0);
                    if (avail > 0) {
                        fds[i].revents |= POLLIN;
                        ready++;
                    }
                }
            }
        }
    }

    return ready;
}

/* ---- pselect6: translate to ppoll semantics (v0.4.99) ---- */
long aios_sys_pselect6(va_list ap) {
    int nfds_max = va_arg(ap, int);
    void *readfds  = va_arg(ap, void *);
    void *writefds = va_arg(ap, void *);
    void *exceptfds = va_arg(ap, void *);
    const struct timespec *tmo = va_arg(ap, const struct timespec *);
    (void)exceptfds;

    /* v0.4.99: scan fd set for tty fds (fd 0 or is_tty AIOS fds) */
    int ready = 0;
    unsigned long *rfds = (unsigned long *)readfds;
    unsigned long *wfds = (unsigned long *)writefds;

    if (rfds && nfds_max > 0) {
        for (int fd = 0; fd < nfds_max && fd < 64; fd++) {
            int word = fd / (8 * (int)sizeof(unsigned long));
            unsigned long bit = 1UL << (fd % (8 * sizeof(unsigned long)));
            if (!(rfds[word] & bit)) continue;

            int is_tty = 0;
            if (fd == 0) is_tty = 1;
            else if (fd >= AIOS_FD_BASE && fd < AIOS_FD_BASE + AIOS_MAX_FDS) {
                aios_fd_t *af = &aios_fds[fd - AIOS_FD_BASE];
                if (af->active && af->is_tty) is_tty = 1;
            }

            if (is_tty && ser_ep) {
                seL4_MessageInfo_t reply = seL4_Call(ser_ep,
                    seL4_MessageInfo_new(76 /* TTY_POLL */, 0, 0, 0));
                int avail = (int)seL4_GetMR(0);
                if (avail > 0) {
                    ready++;
                } else {
                    rfds[word] &= ~bit;
                }
            } else if (!is_tty) {
                rfds[word] &= ~bit;
            }
        }
    }

    if (wfds && nfds_max > 1) {
        if (*wfds & 2) ready++;
        if (*wfds & 4) ready++;
    }

    return ready;
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

    /* Use crypto_server CSPRNG if available */
    if (crypto_ep) {
        size_t got = 0;
        while (got < count) {
            size_t want = count - got;
            if (want > seL4_MsgMaxLength * sizeof(seL4_Word))
                want = seL4_MsgMaxLength * sizeof(seL4_Word);
            seL4_SetMR(0, 1);  /* CRYPTO_OP_RANDOM */
            seL4_SetMR(1, (seL4_Word)want);
            seL4_MessageInfo_t reply = seL4_Call(crypto_ep,
                seL4_MessageInfo_new(0, 0, 0, 2));
            size_t nw = seL4_MessageInfo_get_length(reply);
            for (size_t wi = 0; wi < nw && got < count; wi++) {
                seL4_Word w = seL4_GetMR(wi);
                size_t rem = count - got;
                size_t chunk = sizeof(seL4_Word);
                if (chunk > rem) chunk = rem;
                __builtin_memcpy(buf + got, &w, chunk);
                got += chunk;
            }
        }
        return (long)count;
    }
    /* Fallback: splitmix64 if crypto_server not available */
    uint64_t state;
    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(state));
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

/* ---- mremap: return ENOMEM so musl falls back to mmap+copy ---- */
long aios_sys_mremap(va_list ap) {
    (void)ap;
    return -12;  /* -ENOMEM: force musl realloc fallback */
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

/* v0.4.99: rt_sigsuspend -- suspend until signal delivery
 * ZLE uses pselect which internally calls sigsuspend.
 * Return -EINTR after a brief yield to simulate signal delivery. */
long aios_sys_rt_sigsuspend(va_list ap) {
    (void)ap;
    /* Yield to let other threads run (simulates waiting for signal) */
    seL4_Yield();
    return -4;  /* EINTR -- pretend a signal arrived */
}

/* v0.4.99: setitimer -- ZLE uses for input timeouts
 * Return success, store nothing. ZLE falls back gracefully. */
long aios_sys_setitimer(va_list ap) {
    int which = va_arg(ap, int);
    void *new_val = va_arg(ap, void *);
    void *old_val = va_arg(ap, void *);
    (void)which; (void)new_val;
    /* If old_val requested, zero it out */
    if (old_val) {
        long *p = (long *)old_val;
        for (int i = 0; i < 4; i++) p[i] = 0;
    }
    return 0;
}

/* v0.4.99: getitimer -- return zeroed timer */
long aios_sys_getitimer(va_list ap) {
    int which = va_arg(ap, int);
    void *val = va_arg(ap, void *);
    (void)which;
    if (val) {
        long *p = (long *)val;
        for (int i = 0; i < 4; i++) p[i] = 0;
    }
    return 0;
}



