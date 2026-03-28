/*
 * sandbox.c — AIOS userspace execution environment
 *
 * This PD provides a minimal runtime for compiled programs:
 *   - Receives compiled code via sandbox_code (RWX region)
 *   - Provides syscall-like functions via sandbox_io
 *   - Executes code and reports results back to orchestrator
 *
 * Memory layout:
 *   sandbox_io   (0x20000000) — 4 KiB IPC with orchestrator
 *   sandbox_heap (0x20100000) — 4 MiB heap for malloc/free
 *   sandbox_code (0x20500000) — 1 MiB for compiled machine code
 */

#include <microkit.h>
#include <aios/channels.h>
#include <aios/ipc.h>
#include "sys/syscall.h"

/* Shared memory regions (set by Microkit loader) */
uintptr_t sandbox_io;
uintptr_t sandbox_heap;
uintptr_t sandbox_code;


/* ── Minimal libc ──────────────────────────────────── */

#define HEAP_SIZE (4 * 1024 * 1024)  /* 4 MiB */
static uint32_t heap_used = 0;

/* Per-fd file position tracking */
#define MAX_FDS 16
static uint32_t fd_pos[MAX_FDS];

static void *sbx_malloc(unsigned long size) {
    /* Simple bump allocator — no free */
    size = (size + 15) & ~15UL;  /* align to 16 */
    if (heap_used + size > HEAP_SIZE) return (void *)0;
    void *ptr = (void *)(sandbox_heap + heap_used);
    heap_used += size;
    return ptr;
}

static void sbx_free(void *ptr) {
    /* Bump allocator: free is a no-op */
    (void)ptr;
}

static void sbx_heap_reset(void) {
    heap_used = 0;
}

static void *sbx_memcpy(void *dst, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static void *sbx_memset(void *dst, int c, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

static int sbx_strlen(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static int sbx_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char *)a - *(unsigned char *)b;
}

static char *sbx_strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

static char *sbx_strncpy(char *dst, const char *src, unsigned long n) {
    unsigned long i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

/* ── Output buffer ─────────────────────────────────── */
static uint32_t out_len = 0;

static void sbx_putc(char c) {
    if (out_len < SBX_OUTPUT_MAX - 1) {
        *(volatile char *)(sandbox_io + SBX_OUTPUT + out_len) = c;
        out_len++;
    }
}

static uint32_t puts_count = 0;
static void sbx_puts(const char *s) {
    puts_count++;
    while (*s) sbx_putc(*s++);
}

static void sbx_put_dec(unsigned int n) {
    char buf[12];
    int i = 0;
    if (n == 0) { sbx_putc('0'); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) sbx_putc(buf[--i]);
}

static void sbx_put_hex(unsigned int n) {
    const char *hex = "0123456789abcdef";
    sbx_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        sbx_putc(hex[(n >> i) & 0xf]);
}


/* ── File I/O syscalls (PPC to orchestrator) ─────────── */

static int sbx_open_flags(const char *path, int flags) {
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (path[i] && i < 255) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    seL4_SetMR(0, SYS_OPEN);
    seL4_SetMR(1, (seL4_Word)flags);
    microkit_ppcall(CH_SANDBOX, microkit_msginfo_new(0, 2));
    int fd = (int)seL4_GetMR(0);
    if (fd >= 0 && fd < MAX_FDS) fd_pos[fd] = 0;
    return fd;
}

static int sbx_open(const char *path) {
    return sbx_open_flags(path, 0);
}

static int sbx_read(int fd, void *buf, unsigned long len) {
    uint32_t offset = (fd >= 0 && fd < MAX_FDS) ? fd_pos[fd] : 0;
    seL4_SetMR(0, SYS_READ);
    seL4_SetMR(1, fd);
    seL4_SetMR(2, offset);
    seL4_SetMR(3, len);
    microkit_ppcall(CH_SANDBOX, microkit_msginfo_new(0, 4));
    int got = (int)seL4_GetMR(0);
    if (got > 0) {
        volatile uint8_t *src = (volatile uint8_t *)(sandbox_io + 0x400);
        uint8_t *d = (uint8_t *)buf;
        for (int i = 0; i < got; i++) d[i] = src[i];
        if (fd >= 0 && fd < MAX_FDS) fd_pos[fd] += (uint32_t)got;
    }
    return got;
}

static int sbx_write_file(int fd, const void *buf, unsigned long len) {
    const uint8_t *s = (const uint8_t *)buf;
    volatile uint8_t *dst = (volatile uint8_t *)(sandbox_io + 0x400);
    for (unsigned long i = 0; i < len && i < 4096; i++) dst[i] = s[i];
    seL4_SetMR(0, SYS_WRITE);
    seL4_SetMR(1, fd);
    seL4_SetMR(2, len);
    microkit_ppcall(CH_SANDBOX, microkit_msginfo_new(0, 3));
    return (int)seL4_GetMR(0);
}

static int sbx_close(int fd) {
    if (fd >= 0 && fd < MAX_FDS) fd_pos[fd] = 0;
    seL4_SetMR(0, SYS_CLOSE);
    seL4_SetMR(1, fd);
    microkit_ppcall(CH_SANDBOX, microkit_msginfo_new(0, 2));
    return (int)seL4_GetMR(0);
}

static int sbx_unlink(const char *path) {
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (path[i] && i < 255) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    seL4_SetMR(0, SYS_UNLINK);
    microkit_ppcall(CH_SANDBOX, microkit_msginfo_new(0, 1));
    return (int)seL4_GetMR(0);
}

static int sbx_readdir(void *buf, unsigned long max_entries) {
    seL4_SetMR(0, SYS_READDIR);
    microkit_ppcall(CH_SANDBOX, microkit_msginfo_new(0, 1));
    int count = (int)seL4_GetMR(0);
    if (count > 0) {
        volatile uint8_t *src = (volatile uint8_t *)(sandbox_io + 0x400);
        uint8_t *d = (uint8_t *)buf;
        unsigned long bytes = count * 16;
        if (bytes > max_entries * 16) bytes = max_entries * 16;
        for (unsigned long i = 0; i < bytes; i++) d[i] = src[i];
    }
    return count;
}

static int sbx_filesize(void) {
    return (int)seL4_GetMR(1);
}

/* ── Additional POSIX syscalls ─────────────────────── */

static int sbx_stat(const char *path, unsigned long *size_out) {
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (path[i] && i < 255) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    seL4_SetMR(0, SYS_STAT);
    microkit_ppcall(CH_SANDBOX, microkit_msginfo_new(0, 1));
    int r = (int)seL4_GetMR(0);
    if (r == 0 && size_out) *size_out = (unsigned long)seL4_GetMR(1);
    return r;
}

static int sbx_lseek(int fd, long offset, int whence) {
    if (fd >= 0 && fd < MAX_FDS) {
        if (whence == 0) fd_pos[fd] = (uint32_t)offset;        /* SEEK_SET */
        else if (whence == 1) fd_pos[fd] += (uint32_t)offset;   /* SEEK_CUR */
        /* SEEK_END not supported without knowing file size */
        return (int)fd_pos[fd];
    }
    return -1;
}

static int sbx_getcwd(char *buf, unsigned long size) {
    if (buf && size >= 2) { buf[0] = '/'; buf[1] = '\0'; }
    return 0;
}

static int sbx_getpid(void) {
    return 1;
}

/* ── Interactive I/O (PPC, immediate) ────────────────── */

static int sbx_getc(void) {
    seL4_SetMR(0, SYS_GETC);
    microkit_ppcall(CH_SANDBOX, microkit_msginfo_new(0, 1));
    return (int)seL4_GetMR(0);
}

static void sbx_puts_direct(const char *s) {
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (s[i] && i < 255) { dst[i] = s[i]; i++; }
    dst[i] = '\0';
    seL4_SetMR(0, 32);
    microkit_ppcall(CH_SANDBOX, microkit_msginfo_new(0, 1));
}

static void sbx_putc_direct(char c) {
    seL4_SetMR(0, SYS_PUTC);
    seL4_SetMR(1, c);
    microkit_ppcall(CH_SANDBOX, microkit_msginfo_new(0, 2));
}

/* ── Syscall table (passed to user programs) ───────── */
/*
 * User programs receive a pointer to this struct as their argument.
 * This gives them access to libc-like functions without linking.
 */
typedef struct {
    /* Console I/O */
    void (*puts)(const char *s);
    void (*putc)(char c);
    void (*put_dec)(unsigned int n);
    void (*put_hex)(unsigned int n);

    /* Memory */
    void *(*malloc)(unsigned long size);
    void  (*free)(void *ptr);
    void *(*memcpy)(void *dst, const void *src, unsigned long n);
    void *(*memset)(void *dst, int c, unsigned long n);

    /* Strings */
    int   (*strlen)(const char *s);
    int   (*strcmp)(const char *a, const char *b);
    char *(*strcpy)(char *dst, const char *src);
    char *(*strncpy)(char *dst, const char *src, unsigned long n);
    /* File I/O */
    int   (*open)(const char *path);
    int   (*open_flags)(const char *path, int flags);
    int   (*read)(int fd, void *buf, unsigned long len);
    int   (*write_file)(int fd, const void *buf, unsigned long len);
    int   (*close)(int fd);
    int   (*unlink)(const char *path);
    int   (*readdir)(void *buf, unsigned long max_entries);
    int   (*filesize)(void);
    /* Extended POSIX */
    int   (*stat_file)(const char *path, unsigned long *size_out);
    int   (*lseek)(int fd, long offset, int whence);
    int   (*getcwd)(char *buf, unsigned long size);
    int   (*getpid)(void);
    /* Args */
    const char *args;
    /* Interactive I/O */
    int   (*getc)(void);
    void  (*puts_direct)(const char *s);
    void  (*putc_direct)(char c);
} aios_syscalls_t;

static aios_syscalls_t syscalls;

static void init_syscalls(void) {
    syscalls.puts    = sbx_puts;
    syscalls.putc    = sbx_putc;
    syscalls.put_dec = sbx_put_dec;
    syscalls.put_hex = sbx_put_hex;
    syscalls.malloc  = sbx_malloc;
    syscalls.free    = sbx_free;
    syscalls.memcpy  = sbx_memcpy;
    syscalls.memset  = sbx_memset;
    syscalls.strlen  = sbx_strlen;
    syscalls.strcmp   = sbx_strcmp;
    syscalls.strcpy  = sbx_strcpy;
    syscalls.strncpy = sbx_strncpy;
    /* File I/O */
    syscalls.open       = sbx_open;
    syscalls.open_flags = sbx_open_flags;
    syscalls.read       = sbx_read;
    syscalls.write_file = sbx_write_file;
    syscalls.close      = sbx_close;
    syscalls.unlink     = sbx_unlink;
    syscalls.readdir    = sbx_readdir;
    syscalls.filesize   = sbx_filesize;
    /* Extended POSIX */
    syscalls.stat_file  = sbx_stat;
    syscalls.lseek      = sbx_lseek;
    syscalls.getcwd     = sbx_getcwd;
    syscalls.getpid     = sbx_getpid;
    /* Args */
    syscalls.getc        = sbx_getc;
    syscalls.puts_direct = sbx_puts_direct;
    syscalls.putc_direct = sbx_putc_direct;
}

/* ── Execute code ──────────────────────────────────── */
typedef int (*program_entry_t)(aios_syscalls_t *sys);

static void run_program(void) {
    uint32_t code_size = RD32(sandbox_io, SBX_CODE_SIZE);
    if (code_size == 0 || code_size > (1024 * 1024)) {
        WR32(sandbox_io, SBX_STATUS, SBX_ST_ERROR);
        WR32(sandbox_io, SBX_EXIT_CODE, (uint32_t)-1);
        return;
    }

    /* Reset output buffer and heap */
    out_len = 0;
    sbx_heap_reset();

    WR32(sandbox_io, SBX_STATUS, SBX_ST_RUNNING);

    /* Data/instruction cache sync */
    /* Clean data cache and invalidate instruction cache for code region */
    uintptr_t addr = sandbox_code;
    uintptr_t end = sandbox_code + code_size;
    for (; addr < end; addr += 64) {
        __asm__ volatile("dc cvau, %0" :: "r"(addr));
    }
    __asm__ volatile("dsb ish" ::: "memory");
    addr = sandbox_code;
    for (; addr < end; addr += 64) {
        __asm__ volatile("ic ivau, %0" :: "r"(addr));
    }
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    /* Set args pointer */
    syscalls.args = (const char *)(sandbox_io + SBX_ARGS);

    /* Jump to code: entry point is the start of sandbox_code */
    program_entry_t entry = (program_entry_t)sandbox_code;
    microkit_dbg_puts("SBX: jumping to code at 0x20500000\n");
    microkit_dbg_puts("SBX: entry=0x");
    for (int _i=60;_i>=0;_i-=4) microkit_dbg_putc("0123456789abcdef"[(((uintptr_t)entry)>>_i)&0xf]);
    microkit_dbg_puts(" sys=0x");
    for (int _i=60;_i>=0;_i-=4) microkit_dbg_putc("0123456789abcdef"[(((uintptr_t)&syscalls)>>_i)&0xf]);
    microkit_dbg_puts("\n");
    int exit_code = entry(&syscalls);

    /* Store results */
    WR32(sandbox_io, SBX_OUTPUT_LEN, out_len);
    WR32(sandbox_io, SBX_EXIT_CODE, (uint32_t)exit_code);
    WR32(sandbox_io, SBX_STATUS, SBX_ST_DONE);
}

/* ── Microkit entry points ─────────────────────────── */
void init(void) {
    init_syscalls();
    microkit_dbg_puts("SBX: sandbox PD ready\n");
    WR32(sandbox_io, SBX_STATUS, SBX_ST_IDLE);
}

void notified(microkit_channel ch) {
    if (ch == CH_SANDBOX) {
        uint32_t cmd = RD32(sandbox_io, SBX_CMD);
        switch (cmd) {
        case SBX_CMD_RUN:
            run_program();
            microkit_notify(CH_SANDBOX);
            break;
        case SBX_CMD_HALT:
            microkit_dbg_puts("SBX: halt\n");
            break;
        default:
            break;
        }
        /* Clear command */
        WR32(sandbox_io, SBX_CMD, SBX_CMD_NOP);
    }
}
