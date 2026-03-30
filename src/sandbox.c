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

static microkit_channel my_channel;
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
static uint32_t fd_size[MAX_FDS];

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
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));
    int fd = (int)seL4_GetMR(0);
    if (fd >= 0 && fd < MAX_FDS) {
        fd_pos[fd] = 0;
        fd_size[fd] = (uint32_t)seL4_GetMR(1);
    }
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
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 4));
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
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 3));
    return (int)seL4_GetMR(0);
}

static int sbx_close(int fd) {
    if (fd >= 0 && fd < MAX_FDS) { fd_pos[fd] = 0; fd_size[fd] = 0; }
    seL4_SetMR(0, SYS_CLOSE);
    seL4_SetMR(1, fd);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));
    return (int)seL4_GetMR(0);
}

static int sbx_unlink(const char *path) {
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (path[i] && i < 255) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    seL4_SetMR(0, SYS_UNLINK);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    return (int)seL4_GetMR(0);
}

static int sbx_mkdir(const char *path) {
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (path[i] && i < 255) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    seL4_SetMR(0, SYS_MKDIR);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    return (int)seL4_GetMR(0);
}

static int sbx_rmdir(const char *path) {
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (path[i] && i < 255) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    seL4_SetMR(0, SYS_RMDIR);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    return (int)seL4_GetMR(0);
}

static int sbx_rename(const char *oldpath, const char *newpath) {
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (oldpath[i] && i < 127) { dst[i] = oldpath[i]; i++; }
    dst[i] = '\0';
    i++;
    int j = 0;
    while (newpath[j] && i < 255) { dst[i] = newpath[j]; i++; j++; }
    dst[i] = '\0';
    seL4_SetMR(0, SYS_RENAME); /* SYS_RENAME placeholder */
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    return (int)seL4_GetMR(0);
}



static int sbx_readdir(void *buf, unsigned long max_entries) {
    /* No path needed - orchestrator uses current cwd */
    seL4_SetMR(0, SYS_READDIR);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    int count = (int)seL4_GetMR(0);
    unsigned long total_bytes = (unsigned long)seL4_GetMR(1);
    if (count > 0 && total_bytes > 0) {
        volatile uint8_t *src = (volatile uint8_t *)(sandbox_io + 0x400);
        uint8_t *d = (uint8_t *)buf;
        if (total_bytes > max_entries) total_bytes = max_entries;
        for (unsigned long i = 0; i < total_bytes; i++) d[i] = src[i];
    }
    return count;
}

static int sbx_filesize(void) {
    return (int)seL4_GetMR(1);
}

/* ── Additional POSIX syscalls ─────────────────────── */

static uint32_t last_stat_uid, last_stat_gid, last_stat_mode;

static int sbx_stat(const char *path, unsigned long *size_out) {
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (path[i] && i < 255) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    seL4_SetMR(0, SYS_STAT);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    int r = (int)seL4_GetMR(0);
    if (r == 0 && size_out) *size_out = (unsigned long)seL4_GetMR(1);
    if (r == 0) {
        last_stat_uid  = (uint32_t)seL4_GetMR(2);
        last_stat_gid  = (uint32_t)seL4_GetMR(3);
        last_stat_mode = (uint32_t)seL4_GetMR(4);
    }
    return r;
}

static int sbx_stat_ex(unsigned int *uid, unsigned int *gid, unsigned int *mode) {
    if (uid)  *uid  = last_stat_uid;
    if (gid)  *gid  = last_stat_gid;
    if (mode) *mode = last_stat_mode;
    return 0;
}

static int sbx_lseek(int fd, long offset, int whence) {
    if (fd >= 0 && fd < MAX_FDS) {
        if (whence == 0) fd_pos[fd] = (uint32_t)offset;              /* SEEK_SET */
        else if (whence == 1) fd_pos[fd] += (uint32_t)offset;         /* SEEK_CUR */
        else if (whence == 2) fd_pos[fd] = fd_size[fd] + (uint32_t)offset; /* SEEK_END */
        return (int)fd_pos[fd];
    }
    return -1;
}

static int sbx_getcwd(char *buf, unsigned long size) {
    seL4_SetMR(0, SYS_GETCWD);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    if (buf && size > 0) {
        volatile char *sio = (volatile char *)sandbox_io;
        unsigned long i = 0;
        while (sio[0x200 + i] && i < size - 1) { buf[i] = sio[0x200 + i]; i++; }
        buf[i] = '\0';
    }
    return (int)seL4_GetMR(0);
}

static int sbx_chdir(const char *path) {
    volatile char *sio = (volatile char *)sandbox_io;
    /* Copy path to sandbox_io+0x200 */
    int i = 0;
    while (path[i] && i < 255) { sio[0x200 + i] = path[i]; i++; }
    sio[0x200 + i] = 0;
    seL4_SetMR(0, SYS_CHDIR);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    return (int)seL4_GetMR(0);
}

static int sbx_getpid(void) {
    return 1;
}

/* ── Interactive I/O (PPC, immediate) ────────────────── */

static int sbx_getc(void) {
    seL4_SetMR(0, SYS_GETC);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    return (int)seL4_GetMR(0);
}

static void sbx_puts_direct(const char *s) {
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (s[i] && i < 255) { dst[i] = s[i]; i++; }
    dst[i] = '\0';
    seL4_SetMR(0, 32);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
}

static void sbx_putc_direct(char c) {
    seL4_SetMR(0, SYS_PUTC);
    seL4_SetMR(1, c);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));
}

/* ── Syscall table (passed to user programs) ───────── */
/*
 * User programs receive a pointer to this struct as their argument.
 * This gives them access to libc-like functions without linking.
 */


/* ── Process spawn/wait syscalls ─────────────────────── */
static int sbx_spawn(const char *path, const char *args) {
    /* Copy path to sio+0x200 */
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (path[i] && i < 255) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    /* Copy args to sio+0x600 */
    volatile char *adst = (volatile char *)(sandbox_io + 0x600);
    i = 0;
    if (args) {
        while (args[i] && i < 255) { adst[i] = args[i]; i++; }
    }
    adst[i] = '\0';
    seL4_SetMR(0, SYS_SPAWN);
    seL4_SetMR(1, 0);  /* flags: 0 = default */
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));
    return (int)seL4_GetMR(0);
}

static int sbx_waitpid(int pid, int *status) {
    /* Poll with sleep — orchestrator returns -2 (EAGAIN) if child not done */
    for (int attempt = 0; attempt < 600; attempt++) {  /* ~60s timeout */
        seL4_SetMR(0, SYS_WAITPID);
        seL4_SetMR(1, (seL4_Word)pid);
        microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));
        int rpid = (int)seL4_GetMR(0);
        if (rpid != -2) {
            if (status) *status = (int)seL4_GetMR(1);
            return rpid;
        }
        /* Child not done yet — sleep 100ms then retry */
        seL4_SetMR(0, SYS_SLEEP);
        seL4_SetMR(1, 100);
        microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));
    }
    if (status) *status = -1;
    return -1;  /* timeout */
}


static int sbx_chmod(const char *path, unsigned int mode) {
    volatile char *sio = (volatile char *)sandbox_io;
    int i = 0;
    while (path[i] && i < 255) { sio[0x200 + i] = path[i]; i++; }
    sio[0x200 + i] = 0;
    seL4_SetMR(0, SYS_CHMOD);
    seL4_SetMR(1, mode);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));
    return (int)seL4_GetMR(0);
}

static int sbx_chown(const char *path, unsigned int uid, unsigned int gid) {
    volatile char *sio = (volatile char *)sandbox_io;
    int i = 0;
    while (path[i] && i < 255) { sio[0x200 + i] = path[i]; i++; }
    sio[0x200 + i] = 0;
    seL4_SetMR(0, SYS_CHOWN);
    seL4_SetMR(1, uid);
    seL4_SetMR(2, gid);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 3));
    return (int)seL4_GetMR(0);
}

static int sbx_ftruncate(int fd, unsigned long length) {
    seL4_SetMR(0, SYS_FTRUNCATE);
    seL4_SetMR(1, (seL4_Word)fd);
    seL4_SetMR(2, (seL4_Word)length);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 3));
    return (int)seL4_GetMR(0);
}

static int sbx_fcntl(int fd, int cmd, int arg) {
    seL4_SetMR(0, SYS_FCNTL);
    seL4_SetMR(1, (seL4_Word)fd);
    seL4_SetMR(2, (seL4_Word)cmd);
    seL4_SetMR(3, (seL4_Word)arg);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 4));
    return (int)seL4_GetMR(0);
}

static int sbx_kill_proc(int pid, int sig) {
    seL4_SetMR(0, SYS_KILL_PROC);
    seL4_SetMR(1, (seL4_Word)pid);
    seL4_SetMR(2, (seL4_Word)sig);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));
    return (int)seL4_GetMR(0);
}

/* ── POSIX identity syscalls ─────────────────────────── */
static int sbx_getuid(void) {
    seL4_SetMR(0, SYS_GETUID);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    return (int)seL4_GetMR(0);
}

static int sbx_getgid(void) {
    seL4_SetMR(0, SYS_GETGID);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    return (int)seL4_GetMR(0);
}

static int sbx_geteuid(void) {
    seL4_SetMR(0, SYS_GETEUID);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    return (int)seL4_GetMR(0);
}

static int sbx_getegid(void) {
    seL4_SetMR(0, SYS_GETEGID);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    return (int)seL4_GetMR(0);
}

static int sbx_getppid(void) {
    seL4_SetMR(0, SYS_GETPPID);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    return (int)seL4_GetMR(0);
}

static int sbx_access(const char *path, int amode) {
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (path[i] && i < 255) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    seL4_SetMR(0, SYS_ACCESS);
    seL4_SetMR(1, (seL4_Word)amode);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));
    return (int)seL4_GetMR(0);
}

static int sbx_umask(int mask) {
    seL4_SetMR(0, SYS_UMASK);
    seL4_SetMR(1, (seL4_Word)mask);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));
    return (int)seL4_GetMR(0);
}

static int sbx_dup(int oldfd) {
    seL4_SetMR(0, SYS_DUP);
    seL4_SetMR(1, (seL4_Word)oldfd);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));
    return (int)seL4_GetMR(0);
}

static int sbx_dup2(int oldfd, int newfd) {
    seL4_SetMR(0, SYS_DUP2);
    seL4_SetMR(1, (seL4_Word)oldfd);
    seL4_SetMR(2, (seL4_Word)newfd);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 3));
    return (int)seL4_GetMR(0);
}

static int sbx_pipe(int pipefd[2]) {
    seL4_SetMR(0, SYS_PIPE);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    int rc = (int)seL4_GetMR(0);
    if (rc == 0) {
        pipefd[0] = (int)seL4_GetMR(1);
        pipefd[1] = (int)seL4_GetMR(2);
    }
    return rc;
}

static long sbx_time(void) {
    seL4_SetMR(0, SYS_TIME);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    return (long)seL4_GetMR(0);
}

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
    int   (*mkdir)(const char *path);
    int   (*rmdir)(const char *path);
    int   (*rename)(const char *oldpath, const char *newpath);
    int   (*exec)(const char *path, const char *args);
    int   (*readdir)(void *buf, unsigned long max_entries);
    int   (*filesize)(void);
    /* Extended POSIX */
    int   (*stat_file)(const char *path, unsigned long *size_out);
    int   (*stat_ex)(unsigned int *uid, unsigned int *gid, unsigned int *mode);
    int   (*lseek)(int fd, long offset, int whence);
    int   (*getcwd)(char *buf, unsigned long size);
    int   (*chdir)(const char *path);
    int   (*getpid)(void);
    /* Args */
    const char *args;
    /* Interactive I/O */
    int   (*getc)(void);
    void  (*puts_direct)(const char *s);
    void  (*putc_direct)(char c);
    int   (*sleep)(unsigned int seconds);
    /* POSIX extensions */
    int   (*getuid)(void);
    int   (*getgid)(void);
    int   (*geteuid)(void);
    int   (*getegid)(void);
    int   (*getppid)(void);
    int   (*access)(const char *path, int amode);
    int   (*umask)(int mask);
    int   (*dup)(int oldfd);
    int   (*dup2)(int oldfd, int newfd);
    int   (*pipe)(int pipefd[2]);
    long  (*time)(void);
    /* Process management */
    int   (*spawn)(const char *path, const char *args);
    int   (*waitpid)(int pid, int *status);
    int   (*chmod)(const char *path, unsigned int mode);
    int   (*chown)(const char *path, unsigned int uid, unsigned int gid);
    int   (*ftruncate)(int fd, unsigned long length);
    int   (*fcntl)(int fd, int cmd, int arg);
    int   (*kill_proc)(int pid, int sig);
} aios_syscalls_t;
typedef int (*program_entry_t)(aios_syscalls_t *sys);

static aios_syscalls_t syscalls;

static int sbx_exec(const char *path, const char *args) {
    /* Save parent code to top of heap before orchestrator overwrites it */
    uint32_t parent_size = RD32(sandbox_io, SBX_CODE_SIZE);
    uint8_t *backup = (uint8_t *)(sandbox_heap + HEAP_SIZE - parent_size);
    volatile uint8_t *code = (volatile uint8_t *)sandbox_code;
    for (uint32_t i = 0; i < parent_size; i++) backup[i] = code[i];

    /* Write filename + args to path buffer */
    volatile char *dst = (volatile char *)(sandbox_io + 0x200);
    int i = 0;
    while (path[i] && i < 127) { dst[i] = path[i]; i++; }
    dst[i] = '\0';

    /* Write args to SBX_ARGS */
    volatile char *adst = (volatile char *)(sandbox_io + SBX_ARGS);
    int ai = 0;
    if (args) {
        while (args[ai] && ai < SBX_ARGS_MAX - 1) { adst[ai] = args[ai]; ai++; }
    }
    adst[ai] = '\0';

    /* PPC to orchestrator: load child into sandbox_code */
    seL4_SetMR(0, SYS_EXEC);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    int64_t magic = (int64_t)seL4_GetMR(0);

    if (magic != (int64_t)SBX_EXEC_MAGIC) {
        /* Failed to load — restore parent */
        for (uint32_t j = 0; j < parent_size; j++) code[j] = backup[j];
        return -1;
    }

    /* Child is now in sandbox_code — flush caches and run */
    uint32_t child_size = RD32(sandbox_io, SBX_EXEC_CHILD_SIZE);
    uintptr_t addr = sandbox_code;
    uintptr_t end = sandbox_code + child_size;
    for (; addr < end; addr += 64)
        __asm__ volatile("dc cvau, %0" :: "r"(addr));
    __asm__ volatile("dsb ish" ::: "memory");
    addr = sandbox_code;
    for (; addr < end; addr += 64)
        __asm__ volatile("ic ivau, %0" :: "r"(addr));
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    /* Reset output buffer before running child */
    out_len = 0;
    WR32(sandbox_io, SBX_OUTPUT_LEN, 0);

    /* Set args pointer for child */
    syscalls.args = (const char *)(sandbox_io + SBX_ARGS);

    /* Run child */
    program_entry_t entry = (program_entry_t)sandbox_code;
    int child_exit = entry(&syscalls);

    /* Flush child output via direct serial before restoring parent */
    if (out_len > 0) {
        volatile char *out = (volatile char *)(sandbox_io + SBX_OUTPUT);
        for (uint32_t oi = 0; oi < out_len; oi++)
            sbx_putc_direct(out[oi]);
    }
    /* Reset output buffer for parent */
    out_len = 0;
    WR32(sandbox_io, SBX_OUTPUT_LEN, 0);

    /* Restore parent code from heap backup */
    for (uint32_t j = 0; j < parent_size; j++) code[j] = backup[j];

    /* Flush caches for restored parent */
    addr = sandbox_code;
    end = sandbox_code + parent_size;
    for (; addr < end; addr += 64)
        __asm__ volatile("dc cvau, %0" :: "r"(addr));
    __asm__ volatile("dsb ish" ::: "memory");
    addr = sandbox_code;
    for (; addr < end; addr += 64)
        __asm__ volatile("ic ivau, %0" :: "r"(addr));
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    /* Tell orchestrator we are done, pass child exit code */
    seL4_SetMR(0, SYS_EXEC_DONE);
    seL4_SetMR(1, (uint64_t)(uint32_t)child_exit);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));

    /* Restore args pointer for parent */
    syscalls.args = (const char *)(sandbox_io + SBX_ARGS);

    return child_exit;
}


static int sbx_sleep(unsigned int seconds) {
    /* Busy-wait locally — don't block orchestrator */
    for (volatile unsigned int s = 0; s < seconds; s++)
        for (volatile unsigned int i = 0; i < 50000000; i++);
    return 0;
}

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
    syscalls.mkdir      = sbx_mkdir;
    syscalls.rmdir      = sbx_rmdir;
    syscalls.rename     = sbx_rename;
    syscalls.exec       = sbx_exec;
    syscalls.readdir    = sbx_readdir;
    syscalls.filesize   = sbx_filesize;
    /* Extended POSIX */
    syscalls.stat_file  = sbx_stat;
    syscalls.stat_ex    = sbx_stat_ex;
    syscalls.lseek      = sbx_lseek;
    syscalls.getcwd     = sbx_getcwd;
    syscalls.chdir      = sbx_chdir;
    syscalls.getpid     = sbx_getpid;
    /* Args */
    syscalls.getc        = sbx_getc;
    syscalls.puts_direct = sbx_puts_direct;
    syscalls.putc_direct = sbx_putc_direct;
    syscalls.sleep       = sbx_sleep;
    /* POSIX extensions */
    syscalls.getuid     = sbx_getuid;
    syscalls.getgid     = sbx_getgid;
    syscalls.geteuid    = sbx_geteuid;
    syscalls.getegid    = sbx_getegid;
    syscalls.getppid    = sbx_getppid;
    syscalls.access     = sbx_access;
    syscalls.umask      = sbx_umask;
    syscalls.dup        = sbx_dup;
    syscalls.dup2       = sbx_dup2;
    syscalls.pipe       = sbx_pipe;
    syscalls.time       = sbx_time;
    /* Process management */
    syscalls.spawn      = sbx_spawn;
    syscalls.waitpid    = sbx_waitpid;
    syscalls.chmod      = sbx_chmod;
    syscalls.chown      = sbx_chown;
    syscalls.ftruncate  = sbx_ftruncate;
    syscalls.fcntl      = sbx_fcntl;
    syscalls.kill_proc  = sbx_kill_proc;
}

/* ── Execute code ──────────────────────────────────── */

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

    /* Zero BSS: clear memory from end of code to code_size + 64KB */
    {
        volatile uint8_t *bss_start = (volatile uint8_t *)(sandbox_code + code_size);
        uint32_t bss_clear = 64 * 1024;  /* zero 64KB beyond code for BSS */
        if (code_size + bss_clear > 1024 * 1024) bss_clear = (1024 * 1024) - code_size;
        for (uint32_t i = 0; i < bss_clear; i++) bss_start[i] = 0;
    }

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
    /* Derive our channel ID from PD name: sbx0->7, sbx1->8, etc. */
    /* Channel mapping: sbx0-3 use channels 7-10, sbx4-7 use channels 14-17 */
    int sbx_id = microkit_name[3] - '0';
    if (sbx_id < 4)
        my_channel = CH_SBX_BASE + sbx_id;       /* 7, 8, 9, 10 */
    else
        my_channel = CH_SBX4 + (sbx_id - 4);     /* 14, 15, 16, 17 */
    init_syscalls();
    /* boot message silenced to avoid UART interleave */
    WR32(sandbox_io, SBX_STATUS, SBX_ST_IDLE);
}

void notified(microkit_channel ch) {
    if (ch == my_channel) {
        uint32_t cmd = RD32(sandbox_io, SBX_CMD);
        switch (cmd) {
        case SBX_CMD_RUN:
            run_program();
            microkit_notify(my_channel);
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
