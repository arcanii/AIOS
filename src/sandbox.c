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
 *   sandbox_heap — 16 MiB heap for malloc/free
 *   sandbox_code — 4 MiB for compiled machine code
 */

#include <microkit.h>
#include <aios/channels.h>
#include <aios/ipc.h>
#include <aios/proc_state.h>

/* ── Logging backend for sandbox ─────────────────────── */
#define LOG_MODULE "SBX"
#define LOG_LEVEL  LOG_LEVEL_DEBUG
#include <aios/log.h>

static microkit_channel my_channel;
#include "sys/syscall.h"
#include "arch/arch.h"

/* Shared memory regions (set by Microkit loader) */
uintptr_t sandbox_io;
uintptr_t sandbox_heap;
uintptr_t sandbox_code;


/* ── Minimal libc ──────────────────────────────────── */

#define HEAP_SIZE  (16 * 1024 * 1024)  /* 128 MiB */
static uint32_t heap_used = PROC_STATE_RESERVE;  /* Reserve proc_state area */
static uintptr_t stack_top = 0;  /* SP at run_program entry */

/* Stack bounds for fork — recorded at main() entry */
#define FORK_STACK_SAVE_MAX (1024)  /* max stack to save */
/* Stack save area: stored at end of heap, before exec backup */
#define FORK_STACK_OFFSET  (HEAP_SIZE - FORK_STACK_SAVE_MAX - 128 * 1024)

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
    heap_used = PROC_STATE_RESERVE;  /* Reserve space for proc_state_t + stack snapshot */
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
static volatile uint32_t suspend_pending = 0;

/* Forward declarations for suspend/resume */
static int save_and_yield(int syscall_result);
#define SUSPEND_CHECK(result) do { \
    if (suspend_pending || RD32(sandbox_io, SBX_SUSPEND_FLAG)) { \
        suspend_pending = 0; \
        WR32(sandbox_io, SBX_SUSPEND_FLAG, 0); \
        return save_and_yield(result); \
    } \
} while(0)

static void sbx_putc_direct(char c);

static void sbx_puts(const char *s) {
    if (!s) return;
    puts_count++;
    /* Suspension checkpoint — puts is called frequently */
    if (suspend_pending || RD32(sandbox_io, SBX_SUSPEND_FLAG)) {
        suspend_pending = 0;
        WR32(sandbox_io, SBX_SUSPEND_FLAG, 0);
        save_and_yield(0);
    }
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

/* ── Logging backend implementation ─────────────────── */
void _log_puts(const char *s) {
    if (!s) return;
    while (*s) { sbx_putc_direct(*s); s++; }
}
void _log_put_dec(unsigned long n) {
    char buf[20];
    int i = 0;
    if (n == 0) { sbx_putc_direct('0'); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) sbx_putc_direct(buf[--i]);
}
void _log_flush(void) { /* sbx_putc_direct is unbuffered */ }
unsigned long _log_get_time(void) {
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    return (unsigned long)(cnt / freq);
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
    SUSPEND_CHECK(got);
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

static uint32_t last_stat_uid, last_stat_gid, last_stat_mode, last_stat_mtime;

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
        last_stat_mtime = (uint32_t)seL4_GetMR(5);
    }
    return r;
}

static int sbx_stat_ex(unsigned int *uid, unsigned int *gid, unsigned int *mode, unsigned int *mtime) {
    if (uid)  *uid  = last_stat_uid;
    if (gid)  *gid  = last_stat_gid;
    if (mode) *mode = last_stat_mode;
    if (mtime) *mtime = last_stat_mtime;
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
    seL4_SetMR(0, SYS_GETPID);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    return (int)seL4_GetMR(0);
}

/* ── Interactive I/O (PPC, immediate) ────────────────── */

static int sbx_getc(void) {
    for (;;) {
        seL4_SetMR(0, SYS_GETC);
        microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
        int result = (int)seL4_GetMR(0);
        if (result != -2) return result;  /* got a char or EOF */
        /* EAGAIN: yield and retry */
        seL4_Yield();
    }
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

static __attribute__((unused)) void sbx_debug(const char *msg) {
    while (*msg) { sbx_putc_direct(*msg); msg++; }
}

/* ── Process state save/restore for suspend/resume/fork ──── */

static void save_process_state(int syscall_result) {
    proc_state_t *ps = (proc_state_t *)sandbox_heap;

    ps->magic = PROC_STATE_MAGIC;
    ps->version = 1;

    /* Save sandbox globals */
    ps->heap_used = heap_used;
    ps->out_len = out_len;
    ps->puts_count = puts_count;
    ps->stack_top = stack_top;
    for (int i = 0; i < MAX_PROC_FDS; i++) {
        ps->fd_pos[i] = fd_pos[i];
        ps->fd_size[i] = fd_size[i];
    }

    ps->pending_result = syscall_result;
    ps->is_valid = 1;
}

static void save_stack_snapshot(proc_state_t *ps) {
    uintptr_t cur_sp = ps->saved_sp;  /* Set by caller from ctx.sp */
    uint32_t stack_size = 0;
    if (stack_top > cur_sp) {
        stack_size = (uint32_t)(stack_top - cur_sp);
    }
    if (stack_size > (uint32_t)PROC_STACK_MAX) {
        stack_size = (uint32_t)PROC_STACK_MAX;
    }
    ps->saved_stack_size = stack_size;

    /* Copy stack data right after the struct */
    uint8_t *dst = (uint8_t *)ps + sizeof(proc_state_t);
    uint8_t *src = (uint8_t *)cur_sp;
    for (uint32_t i = 0; i < stack_size; i++)
        dst[i] = src[i];
}

static void __attribute__((unused)) restore_process_state(void) {
    proc_state_t *ps = (proc_state_t *)sandbox_heap;

    if (ps->magic != PROC_STATE_MAGIC || !ps->is_valid)
        return;

    /* Restore sandbox globals */
    heap_used = ps->heap_used;
    out_len = ps->out_len;
    puts_count = ps->puts_count;
    stack_top = ps->stack_top;
    for (int i = 0; i < MAX_PROC_FDS; i++) {
        fd_pos[i] = ps->fd_pos[i];
        fd_size[i] = ps->fd_size[i];
    }

    /* Restore stack snapshot */
    if (ps->saved_stack_size > 0 && ps->saved_sp != 0) {
        uint8_t *src = (uint8_t *)ps + sizeof(proc_state_t);
        uint8_t *dst = (uint8_t *)ps->saved_sp;
        for (uint32_t i = 0; i < ps->saved_stack_size; i++)
            dst[i] = src[i];
    }
}

/* Save context + state, tell orchestrator, never returns (until resumed) */
static int save_and_yield(int syscall_result) {
    proc_state_t *ps = (proc_state_t *)sandbox_heap;

    /* Save CPU context — arch_save_context returns 0 on save, 1 on resume */
    arch_context_t *ctx = (arch_context_t *)ps->ctx;
    int resumed = arch_save_context(ctx);

    if (resumed) {
        /* We were resumed on this (or another) slot.
         * Stack was already restored by the RESUME handler in notified().
         * Restore globals only. */
        proc_state_t *rps = (proc_state_t *)sandbox_heap;
        heap_used    = rps->heap_used;
        out_len      = rps->out_len;
        puts_count   = rps->puts_count;
        stack_top    = rps->stack_top;
        for (int i = 0; i < MAX_PROC_FDS; i++) {
            fd_pos[i]  = rps->fd_pos[i];
            fd_size[i] = rps->fd_size[i];
        }
        suspend_pending = 0;
        sbx_putc_direct('P');
        sbx_putc_direct('R');
        sbx_putc_direct('=');
        int pr = rps->pending_result;
        sbx_putc_direct('0' + (pr & 0xf));
        sbx_putc_direct(10);
        return pr;
    }

    /* Save globals and stack */
    save_process_state(syscall_result);
    ps->saved_sp = ctx->sp;
    save_stack_snapshot(ps);
    ps->suspended = 1;

    /* Flush any buffered output before suspending */
    if (out_len > 0) {
        volatile char *out = (volatile char *)(sandbox_io + SBX_OUTPUT);
        for (uint32_t oi = 0; oi < out_len; oi++)
            sbx_putc_direct(out[oi]);
        out_len = 0;
        WR32(sandbox_io, SBX_OUTPUT_LEN, 0);
    }

    /* Tell orchestrator we are suspended */
    WR32(sandbox_io, SBX_STATUS, SBX_ST_SUSPENDED);
    seL4_SetMR(0, SYS_SUSPENDED);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));

    /* Orchestrator will copy our heap to swap, then later restore us.
     * When restored, arch_restore_context jumps back to arch_save_context
     * which returns 1, hitting the 'resumed' path above. */

    /* Should not reach here in suspend case, but might in fork */
    return syscall_result;
}





/* ── Syscall table (passed to user programs) ───────── */
/*
 * User programs receive a pointer to this struct as their argument.
 * This gives them access to libc-like functions without linking.
 */


/* ── Process spawn/wait syscalls ─────────────────────── */


/* Exit current process — sends SYS_EXIT to orchestrator */
static void sbx_exit_proc(int status) {
    /* Flush any buffered output */
    if (out_len > 0) {
        volatile char *out = (volatile char *)(sandbox_io + SBX_OUTPUT);
        for (uint32_t oi = 0; oi < out_len; oi++)
            sbx_putc_direct(out[oi]);
        out_len = 0;
        WR32(sandbox_io, SBX_OUTPUT_LEN, 0);
    }

    WR32(sandbox_io, SBX_EXIT_CODE, (uint32_t)status);
    WR32(sandbox_io, SBX_STATUS, SBX_ST_DONE);

    /* Tell orchestrator we are exiting */
    seL4_SetMR(0, SYS_EXIT);
    seL4_SetMR(1, (uint64_t)(uint32_t)status);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));

    /* Notify orchestrator that we are done (triggers handle_exec_done) */
    microkit_notify(my_channel);

    /* Halt — orchestrator will restart this PD when needed */
    while (1) {
        seL4_Yield();
    }
}

static int sbx_fork(void) {
    /*
     * Child-inherits-slot fork:
     * 1. Save our full process state to proc_state_t at heap[0]
     * 2. PPC to orchestrator (SYS_FORK)
     * 3. Orchestrator copies our heap+code to swap (that's the parent image)
     * 4. Orchestrator returns 0 — we are now the child
     * 5. Parent will be resumed later from swap via SBX_CMD_RESUME
     *
     * When the parent is eventually resumed, arch_restore_context
     * jumps into save_process_state's arch_save_context which returns 1,
     * and save_and_yield returns the child PID (set by orchestrator).
     */

    proc_state_t *ps = (proc_state_t *)sandbox_heap;

    /* Save CPU context */
    arch_context_t *ctx = (arch_context_t *)ps->ctx;
    int resumed = arch_save_context(ctx);

    if (resumed) {
        proc_state_t *rps = (proc_state_t *)sandbox_heap;
        heap_used    = rps->heap_used;
        out_len      = rps->out_len;
        puts_count   = rps->puts_count;
        stack_top    = rps->stack_top;
        for (int i = 0; i < MAX_PROC_FDS; i++) {
            fd_pos[i]  = rps->fd_pos[i];
            fd_size[i] = rps->fd_size[i];
        }
        suspend_pending = 0;
        out_len = 0;  /* Don't replay pre-fork buffered output */
        int pr = rps->pending_result;

        /* Dump current SP, FP, LR and stack contents */
        sbx_putc_direct(10);
        return pr;
    }

    /* First time through — save everything */
    save_process_state(0);  /* result=0 placeholder, orchestrator will set child PID */
    ps->saved_sp = ctx->sp;
    save_stack_snapshot(ps);
    ps->suspended = 0;  /* This is a fork, not a suspend */

    /* PPC to orchestrator — it will:
     * 1. Copy our heap (including proc_state) to swap
     * 2. Set parent's pending_result = child_pid in the swap copy
     * 3. Reassign this slot to the child
     * 4. Return 0 to us (we are the child now)
     */
    seL4_SetMR(0, SYS_FORK);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));

    /* We are the child — orchestrator returned 0 */
    int child_result = (int)seL4_GetMR(0);
    return child_result;
}


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

/* ── Process listing syscall ─────────────────────────── */
static unsigned long long sbx_timer_freq(void) {
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    return (unsigned long long)freq;
}

static int sbx_getprocs(void *buf, int max_entries) {
    seL4_SetMR(0, SYS_GETPROCS);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 1));
    int count = (int)seL4_GetMR(0);
    unsigned long total_bytes = (unsigned long)seL4_GetMR(1);
    if (count > 0 && total_bytes > 0) {
        volatile uint8_t *src = (volatile uint8_t *)(sandbox_io + 0x400);
        uint8_t *d = (uint8_t *)buf;
        unsigned long limit = (unsigned long)max_entries * 64;
        if (total_bytes > limit) total_bytes = limit;
        for (unsigned long i = 0; i < total_bytes; i++) d[i] = src[i];
    }
    return count;
}

/* Import shared syscall interface — single source of truth */
#define AIOS_NO_SYS_GLOBAL
#include <aios/aios.h>
typedef int (*program_entry_t)(aios_syscalls_t *sys);


static aios_syscalls_t syscalls;

static int sbx_exec(const char *path, const char *args) {
    /* Save parent code + BSS to top of heap before orchestrator overwrites it */
    uint32_t parent_code = RD32(sandbox_io, SBX_CODE_SIZE);
    uint32_t bss_extra = 64 * 1024;  /* must match run_program BSS clear */
    uint32_t parent_size = parent_code + bss_extra;
    if (parent_size > 4 * 1024 * 1024) parent_size = 4 * 1024 * 1024;
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
    arch_flush_code_region(sandbox_code, child_size);

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
    arch_flush_code_region(sandbox_code, parent_size);

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


/* ── Socket syscall wrappers ─────────────────────────── */
static int sbx_socket(int domain, int type, int protocol) {
    (void)domain; (void)protocol;
    seL4_SetMR(0, SYS_SOCKET);
    seL4_SetMR(1, 2); /* AF_INET */
    seL4_SetMR(2, (seL4_Word)type);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 3));
    return (int)seL4_GetMR(0);
}

static int sbx_connect(int sockfd, const void *addr, int addrlen) {
    (void)addrlen;
    const uint8_t *sa = (const uint8_t *)addr;
    volatile uint8_t *dst = (volatile uint8_t *)(sandbox_io + 0x200);
    dst[0] = sa[4]; dst[1] = sa[5]; dst[2] = sa[6]; dst[3] = sa[7];
    dst[4] = sa[3]; dst[5] = sa[2];
    seL4_SetMR(0, SYS_CONNECT);
    seL4_SetMR(1, (seL4_Word)sockfd);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));
    return (int)seL4_GetMR(0);
}

static int sbx_bind(int sockfd, const void *addr, int addrlen) {
    (void)addrlen;
    const uint8_t *sa = (const uint8_t *)addr;
    uint16_t port = ((uint16_t)sa[2] << 8) | sa[3];
    seL4_SetMR(0, SYS_BIND);
    seL4_SetMR(1, (seL4_Word)sockfd);
    seL4_SetMR(2, (seL4_Word)port);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 3));
    return (int)seL4_GetMR(0);
}

static int sbx_listen(int sockfd, int backlog) {
    (void)backlog;
    seL4_SetMR(0, SYS_LISTEN);
    seL4_SetMR(1, (seL4_Word)sockfd);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));
    return (int)seL4_GetMR(0);
}

static int sbx_accept(int sockfd, void *addr, int *addrlen) {
    (void)addr; (void)addrlen;
    seL4_SetMR(0, SYS_ACCEPT);
    seL4_SetMR(1, (seL4_Word)sockfd);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 2));
    return (int)seL4_GetMR(0);
}

static int sbx_send(int sockfd, const void *buf, unsigned long len, int flags) {
    (void)flags;
    volatile uint8_t *dst = (volatile uint8_t *)(sandbox_io + 0x200);
    for (unsigned long i = 0; i < len && i < 4096; i++) dst[i] = ((const uint8_t *)buf)[i];
    seL4_SetMR(0, SYS_SEND);
    seL4_SetMR(1, (seL4_Word)sockfd);
    seL4_SetMR(2, (seL4_Word)len);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 3));
    return (int)seL4_GetMR(0);
}

static int sbx_recv(int sockfd, void *buf, unsigned long len, int flags) {
    (void)flags;
    seL4_SetMR(0, SYS_RECV);
    seL4_SetMR(1, (seL4_Word)sockfd);
    seL4_SetMR(2, (seL4_Word)len);
    microkit_ppcall(my_channel, microkit_msginfo_new(0, 3));
    int got = (int)seL4_GetMR(0);
    if (got > 0) {
        volatile uint8_t *src = (volatile uint8_t *)(sandbox_io + 0x200);
        for (int i = 0; i < got; i++) ((uint8_t *)buf)[i] = src[i];
    }
    return got;
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
    syscalls.fork       = sbx_fork;
    syscalls.exit_proc  = sbx_exit_proc;

    syscalls.chmod      = sbx_chmod;
    syscalls.chown      = sbx_chown;
    syscalls.ftruncate  = sbx_ftruncate;
    syscalls.fcntl      = sbx_fcntl;
    syscalls.kill_proc  = sbx_kill_proc;
    syscalls.getprocs   = sbx_getprocs;
    syscalls.timer_freq = sbx_timer_freq;
    syscalls.socket     = sbx_socket;
    syscalls.connect    = sbx_connect;
    syscalls.bind       = sbx_bind;
    syscalls.listen     = sbx_listen;
    syscalls.accept     = sbx_accept;
    syscalls.send       = sbx_send;
    syscalls.recv       = sbx_recv;
}

/* ── Execute code ──────────────────────────────────── */

static void run_program(void) {
    /* Record stack top on first call */
    /* stack_top already set in init() */


    /* Check for fork resume */
    uint32_t fork_flag = RD32(sandbox_io, SBX_FORK_FLAG);
    /* Check for suspend/resume (proc_state in heap) */
    proc_state_t *ps = (proc_state_t *)sandbox_heap;
    if (ps->magic == PROC_STATE_MAGIC && ps->is_valid) {
            WR32(sandbox_io, SBX_STATUS, SBX_ST_RUNNING);

        /* Flush code icache */
        uint32_t code_size = RD32(sandbox_io, SBX_CODE_SIZE);
        arch_flush_code_region(sandbox_code, code_size + 64 * 1024);

        /* Restore globals */
        heap_used    = ps->heap_used;
        out_len      = ps->out_len;
        puts_count   = ps->puts_count;
        stack_top    = ps->stack_top;
        for (int fi = 0; fi < MAX_PROC_FDS; fi++) {
            fd_pos[fi]  = ps->fd_pos[fi];
            fd_size[fi] = ps->fd_size[fi];
        }
        suspend_pending = 0;
        out_len = 0;  /* Clear output buffer to avoid replaying pre-fork output */

        /* Copy saved stack snapshot back to its original location.
         * This is safe because we are running on the Microkit HANDLER stack,
         * which is separate from the execution stack being restored. */
        uint8_t *snap_src = (uint8_t *)((uintptr_t)ps + sizeof(proc_state_t));
        uint8_t *snap_dst = (uint8_t *)ps->saved_sp;
        for (uint32_t si = 0; si < ps->saved_stack_size; si++)
            snap_dst[si] = snap_src[si];

        /* Clear the magic so we don't re-trigger on next run */
        ps->magic = 0;
        ps->is_valid = 0;

    
        /* Restore CPU context — jumps into sbx_fork() on the execution stack */
        arch_context_t *ctx = (arch_context_t *)ps->ctx;
        arch_restore_context(ctx);
        /* Never reached */
        return;
    }

    if (fork_flag) {
        /* This is a forked child — code and heap already copied by orchestrator */
        WR32(sandbox_io, SBX_STATUS, SBX_ST_RUNNING);
        out_len = 0;

        /* Restore sandbox stack from heap copy FIRST (before context restore) */
        uint8_t *stack_save = (uint8_t *)(sandbox_heap + FORK_STACK_OFFSET);
        uint32_t save_size = *(uint32_t *)stack_save;
        uintptr_t save_sp = *(uintptr_t *)(stack_save + 4);
        uint8_t *stack_data = stack_save + 16;
        uint8_t *dst = (uint8_t *)save_sp;
        for (uint32_t i = 0; i < save_size; i++)
            dst[i] = stack_data[i];

        /* Read context from IO page */
        arch_context_t fork_ctx;
        volatile uint8_t *ctx_src = (volatile uint8_t *)(sandbox_io + SBX_FORK_CTX);
        uint8_t *ctx_dst = (uint8_t *)&fork_ctx;
        for (int i = 0; i < (int)sizeof(arch_context_t); i++)
            ctx_dst[i] = ctx_src[i];

        /* Flush code region icache */
        uint32_t code_size = RD32(sandbox_io, SBX_CODE_SIZE);
        uint32_t bss_extra = 64 * 1024;
        arch_flush_code_region(sandbox_code, code_size + bss_extra);

            /* Jump into the forked context — returns into sbx_fork() as child */
        arch_restore_context(&fork_ctx);
        /* Never reached */
        return;
    }

    uint32_t code_size = RD32(sandbox_io, SBX_CODE_SIZE);
    if (code_size == 0 || code_size > (16 * 1024 * 1024)) {
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
        if (code_size + bss_clear > 16 * 1024 * 1024) bss_clear = (16 * 1024 * 1024) - code_size;
        for (uint32_t i = 0; i < bss_clear; i++) bss_start[i] = 0;
    }

    WR32(sandbox_io, SBX_STATUS, SBX_ST_RUNNING);

    /* Data/instruction cache sync */
    /* Clean data cache and invalidate instruction cache for code region */
    arch_flush_code_region(sandbox_code, code_size);

    /* Set args pointer */
    syscalls.args = (const char *)(sandbox_io + SBX_ARGS);

    /* Jump to code: entry point is the start of sandbox_code */
    program_entry_t entry = (program_entry_t)sandbox_code;
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
    /* Record stack top at earliest point — covers notified() and run_program() frames */
    if (stack_top == 0) {
        __asm__ volatile("mov %0, sp" : "=r"(stack_top));
    }
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
            break;
        case SBX_CMD_SUSPEND:
            /* Orchestrator wants us to suspend — set flag, will be caught at next syscall */
            suspend_pending = 1;
            WR32(sandbox_io, SBX_SUSPEND_FLAG, 1);
            break;
        case SBX_CMD_RESUME: {
            /* Restore a previously suspended process.
             * The orchestrator has already copied code+heap from swap.
             * proc_state_t at heap[0] has the saved context.
             * 
             * We delegate to run_program() which runs on the HANDLER stack.
             * run_program() detects the resume via proc_state magic,
             * restores globals, then calls arch_restore_context() which
             * switches SP to the EXECUTION stack (saved in proc_state).
             * This avoids the handler stack / execution stack overlap problem.
             */
            run_program();
            microkit_notify(my_channel);
            break;
        }
        default:
            break;
        }
        /* Clear command */
        WR32(sandbox_io, SBX_CMD, SBX_CMD_NOP);
    }
}
