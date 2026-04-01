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
    uint64_t ctx[24]; /* setjmp buffer: callee-saved regs */
    void *retval;
    int join_waiting; /* thread waiting on join, -1 if none */
} thread_t;

#define TH_FREE     0
#define TH_READY    1
#define TH_RUNNING  2
#define TH_BLOCKED  3
#define TH_FINISHED 4

/* ---- Process state ---- */
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
} proc_t;

#define PROC_FREE   0
#define PROC_ALIVE  1
#define PROC_ZOMBIE 2

/* ---- Kernel globals ---- */
static proc_t procs[MAX_PROCS];
static thread_t threads[MAX_THREADS];
static int next_pid = 1;
static int current_thread = -1;  /* index of running thread */

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

/* ---- Scheduler ---- */
static uint64_t sched_ctx[24];  /* kernel scheduler context */

static void sched_yield(void) {
    /* Save current thread, pick next ready thread */
    if (current_thread >= 0) {
        thread_t *cur = &threads[current_thread];
        if (cur->state == TH_RUNNING)
            cur->state = TH_READY;
        if (setjmp(cur->ctx) != 0) {
            return;  /* resumed */
        }
    }

    /* Round-robin: find next READY thread */
    int start = (current_thread + 1) % MAX_THREADS;
    for (int i = 0; i < MAX_THREADS; i++) {
        int idx = (start + i) % MAX_THREADS;
        if (threads[idx].state == TH_READY) {
            current_thread = idx;
            threads[idx].state = TH_RUNNING;
            longjmp(threads[idx].ctx, 1);
        }
    }

    /* No ready threads -- return to kernel idle loop */
    current_thread = -1;
}

/* ---- Syscall table for user programs ---- */
#include "aios/aios.h"

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

static int ks_getc(void) {
    for (;;) {
        seL4_SetMR(0, SYS_GETC);
        microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 1));
        int64_t r = (int64_t)seL4_GetMR(0);
        if (r >= 0) return (int)(unsigned char)r;
        if (r == -2) {
            /* EAGAIN -- yield to other threads then retry */
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
    return (int)seL4_GetMR(0);
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
    seL4_SetMR(0, SYS_READ);
    seL4_SetMR(1, (seL4_Word)fd);
    seL4_SetMR(2, 0);  /* offset -- managed by sandbox */
    seL4_SetMR(3, (seL4_Word)len);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 4));
    int64_t got = (int64_t)seL4_GetMR(0);
    if (got > 0) {
        volatile uint8_t *src = (volatile uint8_t *)(sandbox_io + 0x200);
        uint8_t *dst = (uint8_t *)buf;
        for (int64_t j = 0; j < got; j++) dst[j] = src[j];
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
    seL4_SetMR(0, SYS_CLOSE);
    seL4_SetMR(1, (seL4_Word)fd);
    microkit_ppcall(CH_ORCH, microkit_msginfo_new(0, 2));
    return (int)seL4_GetMR(0);
}

static int ks_exec(const char *path) {
    return load_program(path, 0);
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

static void ks_exit(int code) {
    if (current_thread < 0) return;
    thread_t *th = &threads[current_thread];
    proc_t *p = &procs[th->proc_idx];
    p->state = PROC_ZOMBIE;
    p->exit_code = code;
    th->state = TH_FINISHED;
    kputs("Process ");
    kput_dec(p->pid);
    kputs(" exited with code ");
    kput_dec((unsigned int)code);
    kputs("\n");
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
static int ks_stub_int(void) { return -1; }
static int ks_stub_int1(int a) { (void)a; return -1; }
static int ks_stub_int2(int a, int b) { (void)a; (void)b; return -1; }
static void *ks_stub_ptr(unsigned long n) { (void)n; return (void *)0; }
static void ks_stub_free(void *p) { (void)p; }

/* ---- Initialize syscall table ---- */
static void init_syscall_table(void) {
    kern_syscalls.putc = ks_putc;
    kern_syscalls.puts = ks_puts;
    kern_syscalls.puts_direct = ks_puts_direct;
    kern_syscalls.getc = ks_getc;
    kern_syscalls.open = ks_open;
    kern_syscalls.open_flags = ks_open_flags;
    kern_syscalls.read = ks_read;
    kern_syscalls.write_file = ks_write_file;
    kern_syscalls.close = ks_close;
    kern_syscalls.exec = ks_exec;
    kern_syscalls.getpid = ks_getpid;
    kern_syscalls.getppid = ks_getppid;
    kern_syscalls.sleep = ks_sleep;
    kern_syscalls.getuid = ks_getuid;
    kern_syscalls.getgid = ks_getgid;
    kern_syscalls.timer_freq = ks_timer_freq;
    kern_syscalls.malloc = ks_stub_ptr;
    kern_syscalls.free = ks_stub_free;
    kern_syscalls.args = "";
}

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
        kputs("sbxk: failed to load ");
        kputs(path);
        kputs("\n");
        return -1;
    }
    uint32_t loaded = (uint32_t)seL4_GetMR(1);

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

    /* Initialize thread context to jump to program entry */
    /* Entry point: int _start(aios_syscalls_t *sys) */
    /* x30 (lr) = entry point, sp = top of stack, x19 = arg (sys ptr) */
    /* TRAMPOLINE_FIXED */
    for (int k = 0; k < 24; k++) th->ctx[k] = 0;
    uintptr_t entry = code_base;
    uintptr_t sp_top = (stack_base + DEFAULT_STACK_SZ) & ~15UL;
    th->ctx[12] = sp_top;                        /* sp */
    th->ctx[11] = (uint64_t)_thread_start;       /* x30/lr = trampoline */
    th->ctx[0]  = (uint64_t)&kern_syscalls;      /* x19 = syscalls ptr */
    th->ctx[1]  = (uint64_t)entry;               /* x20 = real entry point */

    kputs("sbxk: loaded ");
    kputs(path);
    kputs(" (");
    kput_dec(loaded);
    kputs(" bytes) as PID ");
    kput_dec(p->pid);
    kputs("\n");

    return p->pid;
}

/* ---- Thread entry trampoline ---- */
/* When longjmp restores a new thread, x19 has the sys pointer.
   But the program entry expects it in x0. The setjmp/longjmp
   restores x19 and then ret jumps to x30 (entry point).
   We need a small trampoline to move x19 -> x0. */


__attribute__((naked, used))
static void _thread_start(void) {
    __asm__ volatile(
        "mov x0, x19\n"  /* arg0 = syscalls ptr (was in x19) */
        "br  x20\n"      /* jump to real entry point (was in x20) */
    );
}



/* ---- Microkit entry points ---- */
void init(void) {
    /* Zero process and thread tables */
    for (int i = 0; i < MAX_PROCS; i++) procs[i].state = PROC_FREE;
    for (int i = 0; i < MAX_THREADS; i++) threads[i].state = TH_FREE;

    init_syscall_table();

    kputs("sbxk: sandbox kernel starting\n");
    kputs("sbxk: memory pool ");
    kput_dec(SANDBOX_MEM_SIZE / (1024 * 1024));
    kputs(" MB at ");
    kput_hex(sandbox_mem);
    kputs("\n");

    /* Load shell as first process */
    int pid = load_program("/bin/shell.bin", 0);
    if (pid > 0) {
        kputs("sbxk: starting scheduler\n");
        sched_yield();
        kputs("sbxk: all processes exited\n");
    } else {
        kputs("sbxk: FATAL - could not load shell\n");
    }
}

void notified(microkit_channel ch) {
    (void)ch;
    /* Preemption tick from orchestrator -- save current, switch to next */
    /* For now, cooperative only */
}
