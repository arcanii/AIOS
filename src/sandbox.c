/* sandbox_kernel.c -- AIOS Sandbox Kernel  SANDBOX_KERNEL_V1
 *
 * User-space kernel running inside a single seL4 protection domain.
 * Manages processes, threads, memory, and scheduling.
 * Forwards I/O and filesystem calls to orchestrator via PPC.
 */
#include <microkit.h>
#include "aios/ipc.h"
#include "aios/channels.h"
#include "sys/syscall.h"
#include "arch/aarch64/timer.h"

/* ---- Memory regions (set by Microkit loader) ---- */
uintptr_t sandbox_io;    /* 4 KB shared with orchestrator */
uintptr_t sandbox_mem;   /* 128 MB memory pool */

#define CH_ORCH  7  /* PPC channel to orchestrator */

#define SANDBOX_MEM_SIZE  0x8000000   /* 128 MB */
#define KERNEL_RESERVE    0x100000    /* 1 MB for kernel data */
#define MAX_PROCS         256
#define MAX_THREADS       1024
#define DEFAULT_STACK_SZ  (64 * 1024)
#define DEFAULT_HEAP_SZ   (256 * 1024)
#define DEFAULT_CODE_SZ   (256 * 1024)
#define PROC_NAME_MAX     32

/* ---- Forward declarations ---- */
static void sched_yield(void);
static int load_program(const char *path, int parent_pid);

/* ---- Simple bump allocator for kernel memory pool ---- */
static uint32_t pool_offset = KERNEL_RESERVE;  /* first free byte in sandbox_mem */

static uintptr_t pool_alloc(uint32_t size) {
    size = (size + 63) & ~63;  /* 64-byte align */
    if (pool_offset + size > SANDBOX_MEM_SIZE) return 0;
    uintptr_t addr = sandbox_mem + pool_offset;
    pool_offset += size;
    return addr;
}

/* ---- Debug output via orchestrator ---- */
static void kputc(char c) {
    seL4_SetMR(0, SYS_PUTC);
    seL4_SetMR(1, (seL4_Word)(unsigned char)c);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 2));
}
static void kputs(const char *s) {
    while (*s) kputc(*s++);
}
static void kput_dec(unsigned int n) {
    char buf[12]; int i = 0;
    if (n == 0) { kputc('0'); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) kputc(buf[--i]);
}
static void kput_hex(uint64_t v) {
    kputs("0x");
    for (int i = 60; i >= 0; i -= 4)
        kputc("0123456789abcdef"[(v >> i) & 0xf]);
}

/* ---- Thread state ---- */
typedef struct {
    int state;        /* 0=free, 1=ready, 2=running, 3=blocked, 4=finished */
    int proc_idx;     /* owning process index */
    int tid;          /* thread id within process */
    uintptr_t stack_base;
    uint32_t stack_size;
    uint64_t ctx[96]; /* setjmp buffer: integer + FP/SIMD + FPCR/FPSR */
    void *retval;
    int join_waiting; /* thread waiting on join, -1 if none */
    int fresh;        /* 1 = never run, needs direct jump */
    int priority;     /* scheduling priority (higher = more important, default 0) */
    int wait_channel; /* -1 = not waiting, >= 0 = blocked on resource ID */
} thread_t;

#define TH_FREE     0
#define TH_READY    1
#define TH_RUNNING  2
#define TH_BLOCKED  3
#define TH_FINISHED 4
#define TH_BLOCKED_MTX 5   /* blocked waiting for mutex */
#define TH_BLOCKED_COND 6  /* blocked waiting for condvar */

/* ---- Process state ---- */
#define MAX_FDS 16
typedef struct { int in_use; uint32_t offset; } sbx_fd_t;

typedef struct {
    int state;         /* 0=free, 1=alive, 2=zombie */
    int pid;
    int parent_pid;
    int uid;
    uintptr_t code_base;
    uint32_t code_size;
    uintptr_t heap_base;
    uint32_t heap_size;
    uint32_t heap_used;
    int main_thread;   /* index into thread table */
    int exit_code;
    int foreground;
    char name[PROC_NAME_MAX];
    sbx_fd_t fds[MAX_FDS];
} proc_t;

#define PROC_FREE   0
#define PROC_ALIVE  1
#define PROC_ZOMBIE 2

/* ---- Kernel globals ---- */
static proc_t procs[MAX_PROCS];
static thread_t threads[MAX_THREADS];
static int next_pid = 1;
static int current_thread = -1;  /* index of running thread */
static int current_uid = 0;
static int current_gid = 0;

/* setjmp/longjmp from setjmp.S */
extern int setjmp(uint64_t buf[24]);
extern void longjmp(uint64_t buf[24], int val);

/* ---- Process management ---- */
static int proc_alloc(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == PROC_FREE) return i;
    }
    return -1;
}

static int thread_alloc(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == TH_FREE) return i;
    }
    return -1;
}


/* ---- Preemptive timer ---- */
#define TIMER_CH 0          /* notification channel (self) */
#define TICK_MS  10         /* preemption tick interval in ms */

/* Use ARM generic timer to self-notify after TICK_MS */
static void __attribute__((unused)) timer_arm(void) {
    /* Set timer to fire after TICK_MS, notification will arrive via notified() */
    uint64_t freq, now;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    /* We can't use IRQ, so use polling in sched_yield instead */
    (void)freq; (void)now;
}

/* ---- Scheduler ---- */
static uint64_t __attribute__((unused)) sched_ctx[24];  /* kernel scheduler context */

static uint64_t slice_start = 0;  /* when current thread started running */
static uint64_t slice_ticks = 0;  /* ticks per time slice */

static void sched_yield(void) {
    /* Save current thread, pick next ready thread */
    if (current_thread >= 0) {
        thread_t *cur = &threads[current_thread];
        if (cur->state == TH_RUNNING)
            cur->state = TH_READY;
        if (setjmp(cur->ctx) != 0) {
            /* Resumed — record new slice start */
            uint64_t now;
            __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
            slice_start = now;
            return;
        }
    }

    /* Priority-aware round-robin: pick highest priority READY thread
     * On tie, round-robin among same priority (fairness) */
    int best_idx = -1;
    int best_priority = -1;
    int start = (current_thread + 1) % MAX_THREADS;
    for (int i = 0; i < MAX_THREADS; i++) {
        int idx = (start + i) % MAX_THREADS;
        if (threads[idx].state == TH_READY) {
            if (threads[idx].priority > best_priority) {
                best_priority = threads[idx].priority;
                best_idx = idx;
            }
        }
    }
    if (best_idx >= 0) {
        {
            int idx = best_idx;
            current_thread = idx;
            threads[idx].state = TH_RUNNING;
                if (threads[idx].fresh) {
                    threads[idx].fresh = 0;
                    uintptr_t trampoline = threads[idx].ctx[11];
                    uintptr_t x19_val = threads[idx].ctx[0];
                    uintptr_t x20_val = threads[idx].ctx[1];
                    uintptr_t sp_val = threads[idx].ctx[12];
                    __asm__ volatile(
                        "mov sp, %[sp]   \n"
                        "mov x19, %[x19] \n"
                        "mov x20, %[x20] \n"
                        "br  %[tramp]    \n"
                        :: [sp]"r"(sp_val),
                           [x19]"r"(x19_val),
                           [x20]"r"(x20_val),
                           [tramp]"r"(trampoline)
                        : "memory"
                    );
                    __builtin_unreachable();
                }
                longjmp(threads[idx].ctx, 1);
        }
    }

    /* No ready threads -- yield to platform to process I/O */
    seL4_Yield();
    current_thread = -1;
}

/* ---- Syscall table for user programs ---- */
#include "aios/aios.h"
static aios_syscalls_t *sys __attribute__((unused));

/* Global sys pointer used by program macros */
static aios_syscalls_t kern_syscalls;

/* ---- Syscall implementations (forwarded to orchestrator) ---- */
static void ks_putc(char c) {
    seL4_SetMR(0, SYS_PUTC);
    seL4_SetMR(1, (seL4_Word)(unsigned char)c);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 2));
}

static void ks_puts(const char *s) {
    while (*s) ks_putc(*s++);
}

static void ks_puts_direct(const char *s) {
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (s[i] && i < 255) { dst[i] = s[i]; i++; }
    dst[i] = '\0';
    seL4_SetMR(0, 32);  /* SYS_PUTS_DIRECT */
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
}


/* Check if current thread's time slice expired, yield if so */
static void check_preempt(void) {
    if (current_thread >= 0 && slice_ticks > 0) {
        uint64_t now;
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
        if (now - slice_start >= slice_ticks) {
            sched_yield();
        }
    }
}

static int ks_getc(void) {
    for (;;) {
        seL4_SetMR(0, SYS_GETC);
        microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
        int64_t r = (int64_t)seL4_GetMR(0);
        if (r >= 0) return (int)(unsigned char)r;
        if (r == -2) {
            /* EAGAIN -- yield to other threads then retry */
            check_preempt();
            sched_yield();
            continue;
        }
        return -1;
    }
}

static int ks_open(const char *path) {
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (path[i] && i < 255) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    seL4_SetMR(0, SYS_OPEN);
    seL4_SetMR(1, 0);  /* flags=readonly */
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 2));
    int fd = (int)seL4_GetMR(0);
    if (fd >= 0 && fd < MAX_FDS && current_thread >= 0) {
        int pi = threads[current_thread].proc_idx;
        procs[pi].fds[fd].in_use = 1;
        procs[pi].fds[fd].offset = 0;
    }
    return fd;
}

static int ks_open_flags(const char *path, int flags) {
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (path[i] && i < 255) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    seL4_SetMR(0, SYS_OPEN);
    seL4_SetMR(1, (seL4_Word)flags);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 2));
    return (int)seL4_GetMR(0);
}

static int ks_read(int fd, void *buf, unsigned long len) {
    if (len > 3584) len = 3584;
    uint32_t offset = 0;
    if (current_thread >= 0 && fd >= 0 && fd < MAX_FDS) {
        int pi = threads[current_thread].proc_idx;
        offset = procs[pi].fds[fd].offset;
    }
    seL4_SetMR(0, SYS_READ);
    seL4_SetMR(1, (seL4_Word)fd);
    seL4_SetMR(2, (seL4_Word)offset);
    seL4_SetMR(3, (seL4_Word)len);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 4));
    int64_t got = (int64_t)seL4_GetMR(0);
    if (got > 0) {
        volatile uint8_t *src = (volatile uint8_t *)(sandbox_io + 0x200);
        uint8_t *dst = (uint8_t *)buf;
        for (int64_t j = 0; j < got; j++) dst[j] = src[j];
        if (current_thread >= 0 && fd >= 0 && fd < MAX_FDS) {
            int pi = threads[current_thread].proc_idx;
            procs[pi].fds[fd].offset += (uint32_t)got;
        }
    }
    return (int)got;
}

static int ks_write_file(int fd, const void *buf, unsigned long len) {
    if (len > 3584) len = 3584;
    volatile uint8_t *dst = (volatile uint8_t *)(sandbox_io + 0x200);
    const uint8_t *src = (const uint8_t *)buf;
    for (unsigned long j = 0; j < len; j++) dst[j] = src[j];
    seL4_SetMR(0, SYS_WRITE);
    seL4_SetMR(1, (seL4_Word)fd);
    seL4_SetMR(2, (seL4_Word)len);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 3));
    return (int)seL4_GetMR(0);
}

static int ks_close(int fd) {
    if (current_thread >= 0 && fd >= 0 && fd < MAX_FDS) {
        int pi = threads[current_thread].proc_idx;
        procs[pi].fds[fd].in_use = 0;
        procs[pi].fds[fd].offset = 0;
    }
    seL4_SetMR(0, SYS_CLOSE);
    seL4_SetMR(1, (seL4_Word)fd);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 2));
    return (int)(int64_t)seL4_GetMR(0);
}

static int ks_exec(const char *path, const char *args) {
    (void)args;
    int caller_pi = -1;
    if (current_thread >= 0)
        caller_pi = threads[current_thread].proc_idx;
    int child_pid = load_program(path, caller_pi >= 0 ? procs[caller_pi].pid : 0);
    if (child_pid < 0) return child_pid;
    /* Find child process index */
    int child_pi = -1;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != PROC_FREE && procs[i].pid == child_pid) {
            child_pi = i;
            break;
        }
    }
    if (child_pi < 0) return child_pid;
    /* Wait for child to finish by yielding until it exits */
    while (procs[child_pi].state == PROC_ALIVE) {
        sched_yield();
    }
    int exit_code = procs[child_pi].exit_code;
    /* Clean up zombie */
    procs[child_pi].state = PROC_FREE;
    return exit_code;
}

static int ks_getpid(void) {
    if (current_thread < 0) return 0;
    int pi = threads[current_thread].proc_idx;
    return procs[pi].pid;
}

static int ks_getppid(void) {
    if (current_thread < 0) return 0;
    int pi = threads[current_thread].proc_idx;
    return procs[pi].parent_pid;
}

static int ks_sleep(unsigned int seconds) {
    if (seconds == 0) return 0;
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    uint64_t now, target;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    target = now + (uint64_t)seconds * freq;
    while (1) {
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
        if (now >= target) break;
        sched_yield();
    }
    return 0;
}

void ks_exit(int code) {
    if (current_thread < 0) return;
    thread_t *th = &threads[current_thread];
    proc_t *p = &procs[th->proc_idx];
    p->state = PROC_ZOMBIE;
    p->exit_code = code;
    th->state = TH_FINISHED;
    /* process exited silently */
    sched_yield();
    for (;;) ;  /* should never return */
}

static int ks_getuid(void) { return 0; }
static int ks_getgid(void) { return 0; }
static unsigned long long ks_timer_freq(void) {
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? f : 62500000;
}

/* ---- Stub syscalls (return error for now) ---- */
static int __attribute__((unused)) ks_stub_int(void) { return -1; }
static int __attribute__((unused)) ks_stub_int1(int a) { (void)a; return -1; }
static int __attribute__((unused)) ks_stub_int2(int a, int b) { (void)a; (void)b; return -1; }
static void *ks_malloc(unsigned long n) {
    if (current_thread < 0) return (void *)0;
    int pi = threads[current_thread].proc_idx;
    proc_t *p = &procs[pi];
    /* Align to 16 bytes */
    uint32_t alloc = (p->heap_used + 15) & ~15U;
    uint32_t end = alloc + (uint32_t)n;
    if (end > p->heap_size) return (void *)0;
    p->heap_used = end;
    return (void *)(p->heap_base + alloc);
}
static void ks_free(void *ptr) { (void)ptr; /* bump allocator: no-op */ }

/* ---- Login ---- */
static int ks_login(const char *username, const char *password) {
    volatile char *uname = (volatile char *)(sandbox_io + 0x200);
    volatile char *passwd = (volatile char *)(sandbox_io + 0x300);
    int i = 0;
    while (username[i] && i < 31) { uname[i] = username[i]; i++; }
    uname[i] = '\0';
    i = 0;
    while (password[i] && i < 63) { passwd[i] = password[i]; i++; }
    passwd[i] = '\0';
    seL4_SetMR(0, SYS_LOGIN);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
    int r = (int)(int64_t)seL4_GetMR(0);
    if (r == 0) {
        /* Update current uid/gid */
        uint32_t uid = (uint32_t)seL4_GetMR(1);
        uint32_t gid = (uint32_t)seL4_GetMR(2);
        if (current_thread >= 0) {
            int pi = threads[current_thread].proc_idx;
            procs[pi].uid = (int)uid;
        }
        current_uid = (int)uid;
        current_gid = (int)gid;
    }
    /* Clear password from shared memory */
    for (int j = 0; j < 64; j++) passwd[j] = 0;
    return r;
}

/* ---- Initialize syscall table ---- */

/* ---- Stub implementations for missing syscalls ---- */
static void ks_put_dec(unsigned int n) {
    char buf[12]; int i = 0;
    if (n == 0) { ks_putc('0'); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) ks_putc(buf[--i]);
}
static void ks_put_hex(unsigned int v) {
    ks_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        ks_putc("0123456789abcdef"[(v >> i) & 0xf]);
}
static void *ks_memcpy(void *dst, const void *s, unsigned long n) {
    unsigned char *d = dst; const unsigned char *p = s;
    while (n--) *d++ = *p++;
    return dst;
}
static void *ks_memset(void *dst, int c, unsigned long n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}
static int ks_strlen(const char *s) {
    int n = 0; while (*s++) n++; return n;
}
static int ks_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char *)a - *(unsigned char *)b;
}
static char *ks_strcpy(char *dst, const char *s) {
    char *d = dst; while ((*d++ = *s++)); return dst;
}
static char *ks_strncpy(char *dst, const char *s, unsigned long n) {
    char *d = dst;
    while (n-- && (*d++ = *s++));
    while (n-- > 0) *d++ = 0;
    return dst;
}
static int ks_unlink(const char *path) {
    volatile char *fn = (volatile char *)(sandbox_io + 0x200);
    int i = 0; while (path[i] && i < 255) { fn[i] = path[i]; i++; } fn[i] = 0;
    seL4_SetMR(0, SYS_UNLINK);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
    return (int)(int64_t)seL4_GetMR(0);
}
static int ks_mkdir(const char *path) {
    volatile char *fn = (volatile char *)(sandbox_io + 0x200);
    int i = 0; while (path[i] && i < 255) { fn[i] = path[i]; i++; } fn[i] = 0;
    seL4_SetMR(0, SYS_MKDIR);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
    return (int)(int64_t)seL4_GetMR(0);
}
static int ks_rmdir(const char *path) {
    volatile char *fn = (volatile char *)(sandbox_io + 0x200);
    int i = 0; while (path[i] && i < 255) { fn[i] = path[i]; i++; } fn[i] = 0;
    seL4_SetMR(0, SYS_RMDIR);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
    return (int)(int64_t)seL4_GetMR(0);
}
static int ks_rename(const char *oldp, const char *newp) {
    volatile char *fn = (volatile char *)(sandbox_io + 0x200);
    int i = 0; while (oldp[i] && i < 127) { fn[i] = oldp[i]; i++; } fn[i] = 0;
    volatile char *fn2 = (volatile char *)(sandbox_io + 0x280);
    int j = 0; while (newp[j] && j < 127) { fn2[j] = newp[j]; j++; } fn2[j] = 0;
    seL4_SetMR(0, SYS_RENAME);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
    return (int)(int64_t)seL4_GetMR(0);
}
static int ks_readdir(void *buf, unsigned long max) {
    /* Write "." to path field so orchestrator uses cwd */
    volatile char *fn = (volatile char *)(sandbox_io + 0x200);
    fn[0] = '.'; fn[1] = 0;
    seL4_SetMR(0, SYS_READDIR);
    seL4_SetMR(1, (seL4_Word)max);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 2));
    int count = (int)(int64_t)seL4_GetMR(0);
    uint32_t total_bytes = (uint32_t)seL4_GetMR(1);

    if (count > 0 && total_bytes > 0) {
        if (total_bytes > max) total_bytes = max;
        volatile uint8_t *src_p = (volatile uint8_t *)(sandbox_io + 0x200);
        uint8_t *dst = (uint8_t *)buf;
        for (uint32_t i = 0; i < total_bytes; i++) dst[i] = src_p[i];
    }
    return count;
}
static int ks_filesize(void) {
    seL4_SetMR(0, SYS_FSTAT);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
    return (int)seL4_GetMR(1);
}
/* Cached stat_ex fields from last stat_file call */
static unsigned int _cached_uid, _cached_gid, _cached_mode, _cached_mtime;

static int ks_stat_file(const char *path, unsigned long *size_out) {
    volatile char *fn = (volatile char *)(sandbox_io + 0x200);
    int i = 0; while (path[i] && i < 255) { fn[i] = path[i]; i++; } fn[i] = 0;
    seL4_SetMR(0, SYS_STAT);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
    int r = (int)(int64_t)seL4_GetMR(0);
    if (r == 0) {
        if (size_out) *size_out = (unsigned long)seL4_GetMR(1);
        _cached_uid = (unsigned int)seL4_GetMR(2);
        _cached_gid = (unsigned int)seL4_GetMR(3);
        _cached_mode = (unsigned int)seL4_GetMR(4);
        _cached_mtime = (unsigned int)seL4_GetMR(5);
    }
    return r;
}
static int ks_stat_ex(unsigned int *uid, unsigned int *gid, unsigned int *mode, unsigned int *mtime) {
    /* Return cached values from last ks_stat_file call */
    if (uid) *uid = _cached_uid;
    if (gid) *gid = _cached_gid;
    if (mode) *mode = _cached_mode;
    if (mtime) *mtime = _cached_mtime;
    return 0;
}
static int ks_lseek(int fd, long offset, int whence) {
    (void)fd; (void)offset; (void)whence;
    return 0;
}
static int ks_getcwd(char *buf, unsigned long size) {
    seL4_SetMR(0, SYS_GETCWD);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
    volatile char *src = (volatile char *)(sandbox_io + 0x200);
    unsigned long i = 0;
    while (src[i] && i < size - 1) { buf[i] = src[i]; i++; }
    buf[i] = 0;
    return 0;
}
static int ks_chdir(const char *path) {
    volatile char *fn = (volatile char *)(sandbox_io + 0x200);
    int i = 0; while (path[i] && i < 255) { fn[i] = path[i]; i++; } fn[i] = 0;
    seL4_SetMR(0, SYS_CHDIR);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
    int r = (int)(int64_t)seL4_GetMR(0);

    return r;
}
static int ks_access(const char *path, int amode) {
    (void)path; (void)amode; return 0;
}
static int ks_umask(int mask) { (void)mask; return 022; }
static int ks_dup(int fd) { (void)fd; return -1; }
static int ks_dup2(int oldfd, int newfd) { (void)oldfd; (void)newfd; return -1; }
static int ks_pipe(int pipefd[2]) { (void)pipefd; return -1; }
static long ks_time(void) {
    seL4_SetMR(0, SYS_TIME);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
    return (long)seL4_GetMR(1);
}
static int ks_spawn(const char *path, const char *args) { return ks_exec(path, args); }
static int ks_waitpid(int pid, int *status) { (void)pid; if (status) *status = 0; return pid; }
static int ks_fork(void) { return -1; }
static int ks_chmod(const char *path, unsigned int mode) {
    volatile char *fn = (volatile char *)(sandbox_io + 0x200);
    int i = 0; while (path[i] && i < 255) { fn[i] = path[i]; i++; } fn[i] = 0;
    seL4_SetMR(0, SYS_CHMOD);
    seL4_SetMR(1, mode);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 2));
    return (int)(int64_t)seL4_GetMR(0);
}
static int ks_chown(const char *path, unsigned int uid, unsigned int gid) {
    volatile char *fn = (volatile char *)(sandbox_io + 0x200);
    int i = 0; while (path[i] && i < 255) { fn[i] = path[i]; i++; } fn[i] = 0;
    seL4_SetMR(0, SYS_CHOWN);
    seL4_SetMR(1, uid);
    seL4_SetMR(2, gid);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 3));
    return (int)(int64_t)seL4_GetMR(0);
}
static int ks_ftruncate(int fd, unsigned long length) { (void)fd; (void)length; return -1; }
static void ks_exit_proc(int status) { ks_exit(status); }
static int ks_fcntl(int fd, int cmd, int arg) { (void)fd; (void)cmd; (void)arg; return -1; }
static int ks_kill_proc(int pid, int sig) { (void)pid; (void)sig; return -1; }
static int ks_getprocs(void *buf, int max) {
    proc_info_t *out = (proc_info_t *)buf;
    int count = 0;
    for (int i = 0; i < MAX_PROCS && count < max; i++) {
        if (procs[i].state == PROC_FREE) continue;
        out[count].pid = procs[i].pid;
        out[count].parent_pid = procs[i].parent_pid;
        /* Map internal state to POSIX states */
        int th_idx = procs[i].main_thread;
        int st = 0;
        if (th_idx >= 0 && th_idx < MAX_THREADS) {
            switch (threads[th_idx].state) {
                case TH_READY:    st = 2; break;  /* READY */
                case TH_RUNNING:  st = 3; break;  /* RUNNING */
                case TH_BLOCKED:  st = 4; break;  /* BLOCKED */
                case TH_FINISHED: st = 5; break;  /* ZOMBIE */
                default:          st = 1; break;  /* QUEUED */
            }
        }
        out[count].state = st;
        out[count].uid = procs[i].uid;
        out[count].slot = 0;
        out[count].foreground = (unsigned char)procs[i].foreground;
        out[count]._reserved[0] = 0;
        out[count]._reserved[1] = 0;
        out[count]._reserved[2] = 0;
        /* Copy name */
        int ni = 0;
        while (procs[i].name[ni] && ni < 31) { out[count].name[ni] = procs[i].name[ni]; ni++; }
        out[count].name[ni] = '\0';
        out[count].cpu_time = 0;
        count++;
    }
    return count;
}
static int ks_socket(int d, int t, int p) { (void)d; (void)t; (void)p; return -1; }
static int ks_connect(int s, const void *a, int l) { (void)s; (void)a; (void)l; return -1; }
static int ks_bind(int s, const void *a, int l) { (void)s; (void)a; (void)l; return -1; }
static int ks_listen(int s, int b) { (void)s; (void)b; return -1; }
static int ks_accept(int s, void *a, int *l) { (void)s; (void)a; (void)l; return -1; }
static int ks_send(int s, const void *b, unsigned long l, int f) { (void)s; (void)b; (void)l; (void)f; return -1; }
static int ks_recv(int s, void *b, unsigned long l, int f) { (void)s; (void)b; (void)l; (void)f; return -1; }
static int ks_geteuid(void) { return 0; }
static int ks_getegid(void) { return 0; }


extern void _thread_start(void);
extern void _pthread_start(void);

/* ================================================================
 * POSIX Threads Implementation (sandbox kernel)
 *
 * All threading runs inside the sandbox PD. No PPC needed.
 * Mutex/cond use yield-spin (safe on single-core with preemption).
 * ================================================================ */

/* ---- Thread management ---- */

static int ks_pthread_create(unsigned long *thread_out, const void *attr,
                              void *(*start_routine)(void *), void *arg) {
    (void)attr;
    if (!start_routine) return 22; /* EINVAL */
    if (current_thread < 0) return 38; /* ENOSYS */

    int pi = threads[current_thread].proc_idx;

    int ti = thread_alloc();
    if (ti < 0) return 11; /* EAGAIN - no thread slots */

    uintptr_t stack = pool_alloc(DEFAULT_STACK_SZ);
    if (!stack) {
        threads[ti].state = TH_FREE;
        return 12; /* ENOMEM */
    }

    thread_t *th = &threads[ti];
    th->state = TH_READY;
    th->proc_idx = pi;
    th->tid = ti;
    th->stack_base = stack;
    th->stack_size = DEFAULT_STACK_SZ;
    th->retval = (void *)0;
    th->join_waiting = -1;
    th->fresh = 1;
    th->priority = 0;
    th->wait_channel = -1;

    /* Setup context: x19 = arg, x20 = start_routine */
    /* _thread_start trampoline moves x19->x0 then blr x20 */
    for (int k = 0; k < 96; k++) th->ctx[k] = 0;
    uintptr_t sp_top = (stack + DEFAULT_STACK_SZ) & ~15UL;
    th->ctx[12] = sp_top;                        /* sp */
    th->ctx[11] = (uint64_t)_pthread_start;      /* x30/lr = pthread trampoline */
    th->ctx[0]  = (uint64_t)arg;                 /* x19 = arg */
    th->ctx[1]  = (uint64_t)start_routine;       /* x20 = entry */

    if (thread_out) *thread_out = (unsigned long)ti;
    return 0;
}

static int ks_pthread_join(unsigned long thread_id, void **retval) {
    int ti = (int)thread_id;
    if (ti < 0 || ti >= MAX_THREADS) return 22; /* EINVAL */
    thread_t *target = &threads[ti];
    if (target->state == TH_FREE) return 3; /* ESRCH */

    /* Busy-wait with yield until target finishes */
    while (target->state != TH_FINISHED) {
        sched_yield();
    }

    if (retval) *retval = target->retval;

    /* Clean up thread */
    target->state = TH_FREE;
    return 0;
}

static int ks_pthread_detach(unsigned long thread_id) {
    int ti = (int)thread_id;
    if (ti < 0 || ti >= MAX_THREADS) return 22;
    if (threads[ti].state == TH_FREE) return 3;
    threads[ti].join_waiting = -2; /* -2 = detached, auto-cleanup */
    return 0;
}

static void ks_pthread_exit(void *retval) {
    if (current_thread < 0) return;
    thread_t *th = &threads[current_thread];
    th->retval = retval;
    th->state = TH_FINISHED;

    /* If detached, auto-free */
    if (th->join_waiting == -2) {
        th->state = TH_FREE;
    }

    sched_yield();
    for (;;) ; /* should never return */
}

/* ---- Mutex (yield-spin, single-core safe) ---- */

/* Mutex layout: int[0] = lock owner thread (-1 = unlocked), int[1] = initialized */
#define MTX_UNLOCKED (-1)

static int ks_pthread_mutex_init(void *mutex, const void *attr) {
    (void)attr;
    if (!mutex) return 22;
    int *m = (int *)mutex;
    m[0] = MTX_UNLOCKED;
    m[1] = 1; /* initialized */
    return 0;
}

static int ks_pthread_mutex_lock(void *mutex) {
    if (!mutex) return 22;
    int *m = (int *)mutex;
    while (1) {
        if (m[0] == MTX_UNLOCKED) {
            m[0] = current_thread;
            return 0;
        }
        /* Block this thread on the mutex instead of spinning */
        if (current_thread >= 0) {
            threads[current_thread].state = TH_BLOCKED_MTX;
            threads[current_thread].wait_channel = (int)(uintptr_t)mutex;
            sched_yield();
            /* Resumed -- re-check lock */
            continue;
        }
        sched_yield();
    }
}

static int ks_pthread_mutex_unlock(void *mutex) {
    if (!mutex) return 22;
    int *m = (int *)mutex;
    if (m[0] != current_thread) return 1; /* EPERM - not owner */
    m[0] = MTX_UNLOCKED;
    /* Wake highest-priority thread blocked on this mutex */
    int wake_idx = -1;
    int wake_pri = -1;
    int chan = (int)(uintptr_t)mutex;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == TH_BLOCKED_MTX &&
            threads[i].wait_channel == chan &&
            threads[i].priority > wake_pri) {
            wake_pri = threads[i].priority;
            wake_idx = i;
        }
    }
    if (wake_idx >= 0) {
        threads[wake_idx].state = TH_READY;
        threads[wake_idx].wait_channel = -1;
    }
    return 0;
}

static int ks_pthread_mutex_destroy(void *mutex) {
    if (!mutex) return 22;
    int *m = (int *)mutex;
    m[0] = MTX_UNLOCKED;
    m[1] = 0;
    return 0;
}

/* ---- Condition variables (yield-spin) ---- */

/* Cond layout: int[0] = signal counter, int[1] = initialized */

static int ks_pthread_cond_init(void *cond, const void *attr) {
    (void)attr;
    if (!cond) return 22;
    int *c = (int *)cond;
    c[0] = 0; /* signal counter */
    c[1] = 1; /* initialized */
    return 0;
}

static int ks_pthread_cond_wait(void *cond, void *mutex) {
    if (!cond || !mutex) return 22;
    int *c = (int *)cond;
    int snapshot = c[0];

    /* Release mutex, block on condvar, re-acquire when signaled */
    ks_pthread_mutex_unlock(mutex);

    while (c[0] == snapshot) {
        if (current_thread >= 0) {
            threads[current_thread].state = TH_BLOCKED_COND;
            threads[current_thread].wait_channel = (int)(uintptr_t)cond;
        }
        sched_yield();
    }

    ks_pthread_mutex_lock(mutex);
    return 0;
}

static int ks_pthread_cond_signal(void *cond) {
    if (!cond) return 22;
    int *c = (int *)cond;
    c[0]++;
    /* Wake one highest-priority thread blocked on this condvar */
    int wake_idx = -1;
    int wake_pri = -1;
    int chan = (int)(uintptr_t)cond;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == TH_BLOCKED_COND &&
            threads[i].wait_channel == chan &&
            threads[i].priority > wake_pri) {
            wake_pri = threads[i].priority;
            wake_idx = i;
        }
    }
    if (wake_idx >= 0) {
        threads[wake_idx].state = TH_READY;
        threads[wake_idx].wait_channel = -1;
    }
    return 0;
}

static int ks_pthread_cond_broadcast(void *cond) {
    if (!cond) return 22;
    int *c = (int *)cond;
    c[0]++;
    /* Wake ALL threads blocked on this condvar */
    int chan = (int)(uintptr_t)cond;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == TH_BLOCKED_COND &&
            threads[i].wait_channel == chan) {
            threads[i].state = TH_READY;
            threads[i].wait_channel = -1;
        }
    }
    return 0;
}

/* ---- Read-write locks (yield-spin) ---- */

/* RWLock layout: int[0] = reader count, int[1] = writer (thread id or -1), int[2] = init */

static int ks_pthread_rwlock_init(void *rwlock, const void *attr) {
    (void)attr;
    if (!rwlock) return 22;
    int *rw = (int *)rwlock;
    rw[0] = 0;  /* readers */
    rw[1] = -1; /* no writer */
    rw[2] = 1;  /* initialized */
    return 0;
}

static int ks_pthread_rwlock_rdlock(void *rwlock) {
    if (!rwlock) return 22;
    int *rw = (int *)rwlock;
    while (rw[1] >= 0) {
        if (current_thread >= 0) {
            threads[current_thread].state = TH_BLOCKED_MTX;
            threads[current_thread].wait_channel = (int)(uintptr_t)rwlock;
        }
        sched_yield();
    }
    rw[0]++;
    return 0;
}

static int ks_pthread_rwlock_wrlock(void *rwlock) {
    if (!rwlock) return 22;
    int *rw = (int *)rwlock;
    while (rw[1] >= 0 || rw[0] > 0) {
        if (current_thread >= 0) {
            threads[current_thread].state = TH_BLOCKED_MTX;
            threads[current_thread].wait_channel = (int)(uintptr_t)rwlock;
        }
        sched_yield();
    }
    rw[1] = current_thread;
    return 0;
}

static int ks_pthread_rwlock_unlock(void *rwlock) {
    if (!rwlock) return 22;
    int *rw = (int *)rwlock;
    if (rw[1] == current_thread) {
        rw[1] = -1;
    } else if (rw[0] > 0) {
        rw[0]--;
    }
    /* Wake all threads blocked on this rwlock */
    int chan = (int)(uintptr_t)rwlock;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == TH_BLOCKED_MTX &&
            threads[i].wait_channel == chan) {
            threads[i].state = TH_READY;
            threads[i].wait_channel = -1;
        }
    }
    return 0;
}

/* ---- Thread-local storage ---- */

#define KS_TLS_MAX 64
static const void *_ks_tls[MAX_THREADS][KS_TLS_MAX];
static int _ks_tls_next_key = 0;

static int ks_pthread_key_create(unsigned int *key, void (*destructor)(void *)) {
    (void)destructor;
    if (!key) return 22;
    if (_ks_tls_next_key >= KS_TLS_MAX) return 12;
    *key = (unsigned int)_ks_tls_next_key++;
    return 0;
}

static int ks_pthread_setspecific(unsigned int key, const void *value) {
    if (key >= KS_TLS_MAX || current_thread < 0) return 22;
    _ks_tls[current_thread][key] = value;
    return 0;
}

static void *ks_pthread_getspecific(unsigned int key) {
    if (key >= KS_TLS_MAX || current_thread < 0) return (void *)0;
    return (void *)_ks_tls[current_thread][key];
}

static void ks_sched_yield_user(void) {
    sched_yield();
}

/* ================================================================ */

/* ---- Shutdown (root-only) ---- */
static int ks_shutdown(int flags) {
    /* Only uid 0 (root) can shutdown */
    if (current_thread >= 0) {
        int pi = threads[current_thread].proc_idx;
        if (procs[pi].uid != 0 && current_uid != 0) {
            kputs("sbxk: shutdown denied (not root)\n");
            return -1;  /* EPERM */
        }
    }
    kputs("sbxk: system shutdown requested\n");

    /* Terminate all running threads */
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state != TH_FREE && threads[i].state != TH_FINISHED) {
            threads[i].state = TH_FINISHED;
        }
    }

    /* Forward to orchestrator for FS sync + halt */
    seL4_SetMR(0, SYS_SHUTDOWN);
    seL4_SetMR(1, (seL4_Word)flags);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 2));

    /* Should not return, but if it does: */
    kputs("sbxk: halt\n");
    for (;;) { __asm__ volatile("wfi"); }
    return 0;
}

static int ks_sync(void) {
    seL4_SetMR(0, SYS_SYNC);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
    return (int)(int64_t)seL4_GetMR(0);
}

static void init_syscall_table(void) {
    /* Console I/O */
    kern_syscalls.puts = ks_puts;
    kern_syscalls.putc = ks_putc;
    kern_syscalls.put_dec = ks_put_dec;
    kern_syscalls.put_hex = ks_put_hex;
    /* Memory */
    kern_syscalls.malloc = ks_malloc;
    kern_syscalls.free = ks_free;
    kern_syscalls.memcpy = ks_memcpy;
    kern_syscalls.memset = ks_memset;
    /* Strings */
    kern_syscalls.strlen = ks_strlen;
    kern_syscalls.strcmp = ks_strcmp;
    kern_syscalls.strcpy = ks_strcpy;
    kern_syscalls.strncpy = ks_strncpy;
    /* File I/O */
    kern_syscalls.open = ks_open;
    kern_syscalls.open_flags = ks_open_flags;
    kern_syscalls.read = ks_read;
    kern_syscalls.write_file = ks_write_file;
    kern_syscalls.close = ks_close;
    kern_syscalls.unlink = ks_unlink;
    kern_syscalls.mkdir = ks_mkdir;
    kern_syscalls.rmdir = ks_rmdir;
    kern_syscalls.rename = ks_rename;
    kern_syscalls.exec = ks_exec;
    kern_syscalls.readdir = ks_readdir;
    kern_syscalls.filesize = ks_filesize;
    /* Extended POSIX */
    kern_syscalls.stat_file = ks_stat_file;
    kern_syscalls.stat_ex = ks_stat_ex;
    kern_syscalls.lseek = ks_lseek;
    kern_syscalls.getcwd = ks_getcwd;
    kern_syscalls.chdir = ks_chdir;
    kern_syscalls.getpid = ks_getpid;
    /* Args */
    kern_syscalls.args = "";
    /* Interactive I/O */
    kern_syscalls.getc = ks_getc;
    kern_syscalls.puts_direct = ks_puts_direct;
    kern_syscalls.putc_direct = ks_putc;
    kern_syscalls.sleep = ks_sleep;
    /* POSIX extensions */
    kern_syscalls.getuid = ks_getuid;
    kern_syscalls.getgid = ks_getgid;
    kern_syscalls.geteuid = ks_geteuid;
    kern_syscalls.getegid = ks_getegid;
    kern_syscalls.getppid = ks_getppid;
    kern_syscalls.access = ks_access;
    kern_syscalls.umask = ks_umask;
    kern_syscalls.dup = ks_dup;
    kern_syscalls.dup2 = ks_dup2;
    kern_syscalls.pipe = ks_pipe;
    kern_syscalls.time = ks_time;
    /* Process management */
    kern_syscalls.spawn = ks_spawn;
    kern_syscalls.waitpid = ks_waitpid;
    kern_syscalls.fork = ks_fork;
    kern_syscalls.chmod = ks_chmod;
    kern_syscalls.chown = ks_chown;
    kern_syscalls.ftruncate = ks_ftruncate;
    kern_syscalls.exit_proc = ks_exit_proc;
    kern_syscalls.fcntl = ks_fcntl;
    kern_syscalls.kill_proc = ks_kill_proc;
    kern_syscalls.getprocs = ks_getprocs;
    kern_syscalls.timer_freq = ks_timer_freq;
    /* Sockets */
    kern_syscalls.socket = ks_socket;
    kern_syscalls.connect = ks_connect;
    kern_syscalls.bind = ks_bind;
    kern_syscalls.listen = ks_listen;
    kern_syscalls.accept = ks_accept;
    kern_syscalls.send = ks_send;
    kern_syscalls.recv = ks_recv;

    /* Privileged operations */
    kern_syscalls.shutdown = ks_shutdown;
    kern_syscalls.sync = ks_sync;

    /* POSIX Threads */
    kern_syscalls.pthread_create = ks_pthread_create;
    kern_syscalls.pthread_join = ks_pthread_join;
    kern_syscalls.pthread_detach = ks_pthread_detach;
    kern_syscalls.pthread_exit = ks_pthread_exit;
    kern_syscalls.pthread_mutex_init = ks_pthread_mutex_init;
    kern_syscalls.pthread_mutex_lock = ks_pthread_mutex_lock;
    kern_syscalls.pthread_mutex_unlock = ks_pthread_mutex_unlock;
    kern_syscalls.pthread_mutex_destroy = ks_pthread_mutex_destroy;
    kern_syscalls.pthread_cond_init = ks_pthread_cond_init;
    kern_syscalls.pthread_cond_wait = ks_pthread_cond_wait;
    kern_syscalls.pthread_cond_signal = ks_pthread_cond_signal;
    kern_syscalls.pthread_cond_broadcast = ks_pthread_cond_broadcast;
    kern_syscalls.pthread_rwlock_init = ks_pthread_rwlock_init;
    kern_syscalls.pthread_rwlock_rdlock = ks_pthread_rwlock_rdlock;
    kern_syscalls.pthread_rwlock_wrlock = ks_pthread_rwlock_wrlock;
    kern_syscalls.pthread_rwlock_unlock = ks_pthread_rwlock_unlock;
    kern_syscalls.pthread_key_create = ks_pthread_key_create;
    kern_syscalls.pthread_setspecific = ks_pthread_setspecific;
    kern_syscalls.pthread_getspecific = ks_pthread_getspecific;
    kern_syscalls.sched_yield = ks_sched_yield_user;
}

/* Thread entry trampoline: moves x19->x0, jumps to x20 */
__asm__(
    ".global _thread_start\n"
    ".type _thread_start, %function\n"
    "_thread_start:\n"
    "    mov x0, x19\n"
    "    blr x20\n"
    "    /* program returned in w0 -- call ks_exit */\n"
    "    bl  ks_exit\n"
    "    b   .\n"
);
extern void _thread_start(void);

/* Pthread trampoline: same as _thread_start but calls ks_pthread_exit
 * so return value is captured as void* retval, not int exit code,
 * and only the thread is terminated (not the whole process). */
__asm__(
    ".global _pthread_start\n"
    ".type _pthread_start, %function\n"
    "_pthread_start:\n"
    "    mov x0, x19\n"
    "    blr x20\n"
    "    /* x0 = void* retval from thread function */\n"
    "    bl  ks_pthread_exit\n"
    "    b   .\n"
);
extern void _pthread_start(void);

/* ---- Program loader ---- */
static int load_program(const char *path, int parent_pid) {
    int pi = proc_alloc();
    if (pi < 0) {
        kputs("sbxk: no free process slot\n");
        return -1;
    }
    int ti = thread_alloc();
    if (ti < 0) {
        kputs("sbxk: no free thread slot\n");
        return -1;
    }

    /* Allocate memory for process */
    uintptr_t code_base = pool_alloc(DEFAULT_CODE_SZ);
    uintptr_t heap_base = pool_alloc(DEFAULT_HEAP_SZ);
    uintptr_t stack_base = pool_alloc(DEFAULT_STACK_SZ);
    if (!code_base || !heap_base || !stack_base) {
        kputs("sbxk: out of memory\n");
        return -1;
    }

    /* Ask orchestrator to load binary into code_base */
    volatile char *fn = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (path[i] && i < 255) { fn[i] = path[i]; i++; }
    fn[i] = '\0';
    uint64_t mem_offset = code_base - sandbox_mem;
    seL4_SetMR(0, SYS_EXEC);
    seL4_SetMR(1, (seL4_Word)mem_offset);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 2));
    int64_t r = (int64_t)seL4_GetMR(0);
    if (r != 0) {
        /* load failed silently — shell handles error */
        return -1;
    }
    uint32_t loaded = (uint32_t)seL4_GetMR(1);

    /* Flush D-cache and invalidate I-cache for loaded code region */
    {
        uintptr_t start = code_base & ~63UL;
        uintptr_t end = (code_base + loaded + 63) & ~63UL;
        for (uintptr_t a = start; a < end; a += 64) {
            __asm__ volatile("dc cvau, %0" :: "r"(a));
        }
        __asm__ volatile("dsb ish");
        for (uintptr_t a = start; a < end; a += 64) {
            __asm__ volatile("ic ivau, %0" :: "r"(a));
        }
        __asm__ volatile("dsb ish");
        __asm__ volatile("isb");
    }

    /* Setup process */
    proc_t *p = &procs[pi];
    p->state = PROC_ALIVE;
    p->pid = next_pid++;
    p->parent_pid = parent_pid;
    p->uid = 0;
    p->code_base = code_base;
    p->code_size = loaded;
    p->heap_base = heap_base;
    p->heap_size = DEFAULT_HEAP_SZ;
    p->heap_used = 0;
    p->main_thread = ti;
    p->exit_code = 0;
    p->foreground = 1;
    /* Copy name */
    int ni = 0;
    while (path[ni] && ni < PROC_NAME_MAX - 1) { p->name[ni] = path[ni]; ni++; }
    p->name[ni] = '\0';

    /* Setup main thread */
    thread_t *th = &threads[ti];
    th->state = TH_READY;
    th->proc_idx = pi;
    th->tid = 0;
    th->stack_base = stack_base;
    th->stack_size = DEFAULT_STACK_SZ;
    th->join_waiting = -1;
    th->fresh = 1;

    /* Initialize thread context to jump to program entry */
    /* Entry point: int _start(aios_syscalls_t *sys) */
    /* x30 (lr) = entry point, sp = top of stack, x19 = arg (sys ptr) */
    /* TRAMPOLINE_FIXED */
    for (int k = 0; k < 96; k++) th->ctx[k] = 0;
    uintptr_t entry = code_base;
    uintptr_t sp_top = (stack_base + DEFAULT_STACK_SZ) & ~15UL;
    th->ctx[12] = sp_top;                        /* sp */
    th->ctx[11] = (uint64_t)_thread_start;       /* x30/lr = trampoline */
    th->ctx[0]  = (uint64_t)&kern_syscalls;      /* x19 = syscalls ptr */
    th->ctx[1]  = (uint64_t)entry;               /* x20 = real entry point */

    
    
    
    


    /* loaded silently */

    return p->pid;
}

/* ---- Thread entry trampoline ---- */
/* When longjmp restores a new thread, x19 has the sys pointer.
   But the program entry expects it in x0. The setjmp/longjmp
   restores x19 and then ret jumps to x30 (entry point).
   We need a small trampoline to move x19 -> x0. */





/* ---- Microkit entry points ---- */
void init(void) {
    /* Clear all state on (re)start */
    for (int i = 0; i < MAX_PROCS; i++) procs[i].state = PROC_FREE;
    for (int i = 0; i < MAX_THREADS; i++) {
        threads[i].state = TH_FREE;
        threads[i].fresh = 0;
        for (int k = 0; k < 96; k++) threads[i].ctx[k] = 0;
    }
    current_thread = -1;
    next_pid = 1;
    pool_offset = KERNEL_RESERVE;

    init_syscall_table();

    kputs("sbxk: sandbox kernel starting\n");
    kputs("sbxk: memory pool ");
    kput_dec(SANDBOX_MEM_SIZE / (1024 * 1024));
    kputs(" MB at ");
    kput_hex(sandbox_mem);
    kputs("\n");

    /* Load shell as first process */
    /* Login prompt */
    {
        int logged_in = 0;
        int attempts = 0;
        while (!logged_in && attempts < 3) {
            char username[32], password[64];
            kputs("login: ");
            /* Read username */
            int ui = 0;
            while (ui < 31) {
                int64_t gc;
                do {
                    seL4_SetMR(0, SYS_GETC);
                    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
                    gc = (int64_t)seL4_GetMR(0);
                } while (gc < 0);
                char c = (char)(unsigned char)gc;
                if (c == '\n' || c == '\r') break;
                if (c == 127 || c == 8) { if (ui > 0) { ui--; kputs("\b \b"); } continue; }
                if (c >= ' ') { username[ui++] = c; kputc(c); }
            }
            username[ui] = '\0';
            kputc('\n');
            if (ui == 0) { attempts++; continue; }

            kputs("password: ");
            int pi = 0;
            while (pi < 63) {
                int64_t gc;
                do {
                    seL4_SetMR(0, SYS_GETC);
                    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
                    gc = (int64_t)seL4_GetMR(0);
                } while (gc < 0);
                char c = (char)(unsigned char)gc;
                if (c == '\n' || c == '\r') break;
                if (c == 127 || c == 8) { if (pi > 0) pi--; continue; }
                if (c >= ' ') { password[pi++] = c; kputc('*'); }
            }
            password[pi] = '\0';
            kputc('\n');

            int r = ks_login(username, password);
            if (r == 0) {
                kputs("\n");
                logged_in = 1;
            } else {
                kputs("Login incorrect\n\n");
                attempts++;
            }
            /* Clear password */
            for (int j = 0; j < 64; j++) password[j] = 0;
        }
        if (!logged_in) {
            kputs("Too many failed attempts\n");
            return;
        }
    }

    int pid = load_program("/bin/shell.bin", 0);
    if (pid > 0) {
        /* Init preemptive time slicing */
        {
            uint64_t freq;
            __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
            slice_ticks = (freq * TICK_MS) / 1000;
        }
        kputs("sbxk: starting scheduler (preemptive, ");
        kput_dec(TICK_MS);
        kputs("ms quantum)\n");
        sched_yield();
        kputs("sbxk: all processes exited\n");
    } else {
        kputs("sbxk: FATAL - could not load shell\n");
    }
}

void notified(microkit_channel ch) {
    (void)ch;
    /* Preemption tick from orchestrator */
    if (current_thread >= 0 && threads[current_thread].state == TH_RUNNING) {
        /* Check if there are other ready threads */
        int start = (current_thread + 1) % MAX_THREADS;
        int found = 0;
        for (int i = 0; i < MAX_THREADS; i++) {
            int idx = (start + i) % MAX_THREADS;
            if (threads[idx].state == TH_READY) { found = 1; break; }
        }
        if (!found) return;  /* no point switching if we're the only thread */

        /* Save current thread and switch */
        threads[current_thread].state = TH_READY;
        if (setjmp(threads[current_thread].ctx) != 0) {
            return;  /* resumed */
        }
        /* Pick highest priority ready thread */
        int best = -1;
        int bpri = -1;
        for (int i = 0; i < MAX_THREADS; i++) {
            int idx = (start + i) % MAX_THREADS;
            if (threads[idx].state == TH_READY && threads[idx].priority > bpri) {
                bpri = threads[idx].priority;
                best = idx;
            }
        }
        if (best >= 0) {
            current_thread = best;
            threads[best].state = TH_RUNNING;
            longjmp(threads[best].ctx, 1);
        }
        /* Shouldn't reach here, but restore current if so */
        threads[current_thread].state = TH_RUNNING;
    }
}
