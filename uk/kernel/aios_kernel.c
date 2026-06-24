/*
 * aios_kernel.c -- the AIOS userspace kernel. Host-agnostic core.
 *
 * Dispatches the AIOS ABI for guest programs using ONLY the PAL (pal.h) -- no host headers, no
 * host syscalls. This exact code is meant to run unchanged over the Linux PAL (ptrace) today and
 * a future seL4 PAL (IPC) tomorrow (docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md).
 *
 * M1: WRITE + EXIT (first light).
 * M2: a VFS behind the ABI -- the kernel owns the AIOS fd namespace (an fd table mapping AIOS
 *     fds to opaque PAL backing objects) and services OPEN/READ/WRITE/CLOSE. Programs do real
 *     file I/O THROUGH the kernel; the kernel reaches storage only via the PAL host gateway.
 */
#include "aios_abi.h"
#include "pal.h"
#include <stddef.h>
#include <stdint.h>

/* ---- kernel-internal diagnostics (its OWN stderr, separate from the guest fd table) ---- */
static size_t kstrlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void kputs(const char *s) { pal_host_write(pal_host_std(AIOS_FD_STDERR), s, kstrlen(s)); }
static void kput_int(long v) {
    char b[24]; int i = sizeof b; int neg = v < 0;
    unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    b[--i] = '\0';
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    pal_host_write(pal_host_std(AIOS_FD_STDERR), &b[i], (size_t)(sizeof b - 1 - i));
}

/* ---- the AIOS file-descriptor table: the kernel-owned fd namespace ----
 * M2 keeps one global table (one guest). It becomes per-process when the kernel runs many. */
#define AIOS_MAX_FD 64
static pal_file_t g_back[AIOS_MAX_FD];   /* AIOS fd -> opaque PAL backing object */
static int        g_used[AIOS_MAX_FD];

static void fd_table_init(void) {
    g_back[AIOS_FD_STDIN]  = pal_host_std(0); g_used[AIOS_FD_STDIN]  = 1;
    g_back[AIOS_FD_STDOUT] = pal_host_std(1); g_used[AIOS_FD_STDOUT] = 1;
    g_back[AIOS_FD_STDERR] = pal_host_std(2); g_used[AIOS_FD_STDERR] = 1;
}
static int fd_alloc(void) {
    for (int i = 3; i < AIOS_MAX_FD; i++) if (!g_used[i]) return i;
    return -1;
}
static int fd_valid(uint64_t fd) { return fd < AIOS_MAX_FD && g_used[fd]; }

/* ---- AIOS syscall handlers (host-agnostic; reach the host only via the PAL) ---- */

static long sys_open(uint64_t gpath, uint64_t flags, uint64_t mode) {
    char path[256];
    size_t n = pal_guest_read(gpath, path, sizeof path - 1);
    if (n == 0) return -1;
    path[n] = '\0';                                  /* backstop terminator */
    pal_file_t f = pal_host_open(path, flags, mode);
    if (f == PAL_FILE_INVALID) return -1;
    int fd = fd_alloc();
    if (fd < 0) { pal_host_close(f); return -1; }    /* fd table full */
    g_back[fd] = f; g_used[fd] = 1;
    return fd;
}

static long sys_read(uint64_t fd, uint64_t gbuf, uint64_t len) {
    if (!fd_valid(fd)) return -1;
    char tmp[1024];
    size_t total = 0;
    while (total < len) {
        size_t chunk = (size_t)len - total;
        if (chunk > sizeof tmp) chunk = sizeof tmp;
        long n = pal_host_read(g_back[fd], tmp, chunk);
        if (n < 0)  return total ? (long)total : -1;
        if (n == 0) break;                           /* EOF */
        size_t put = pal_guest_write(gbuf + total, tmp, (size_t)n);
        total += put;
        if (put < (size_t)n) break;                  /* guest buffer not fully writable */
    }
    return (long)total;
}

static long sys_write(uint64_t fd, uint64_t gbuf, uint64_t len) {
    if (!fd_valid(fd)) return -1;
    char tmp[1024];
    size_t total = 0;
    while (total < len) {
        size_t chunk = (size_t)len - total;
        if (chunk > sizeof tmp) chunk = sizeof tmp;
        size_t got = pal_guest_read(gbuf + total, tmp, chunk);
        if (got == 0) break;
        long w = pal_host_write(g_back[fd], tmp, got);
        if (w < 0) return total ? (long)total : -1;
        total += (size_t)w;
        if ((size_t)w < got) break;
    }
    return (long)total;
}

static long sys_close(uint64_t fd) {
    if (!fd_valid(fd)) return -1;
    if (fd > AIOS_FD_STDERR) pal_host_close(g_back[fd]);  /* never close the std streams */
    g_used[fd] = 0;
    return 0;
}

static long sys_lseek(uint64_t fd, uint64_t off, uint64_t whence) {
    if (!fd_valid(fd)) return -1;
    return (long)pal_host_lseek(g_back[fd], (long long)off, (int)whence);
}

static long sys_fstat(uint64_t fd, uint64_t gstat) {
    if (!fd_valid(fd)) return -1;
    struct aios_stat s;
    s._pad = 0;
    if (pal_host_fstat(g_back[fd], &s.size, &s.mode) != 0) return -1;
    if (pal_guest_write(gstat, &s, sizeof s) != sizeof s) return -1;
    return 0;
}

/* The kernel's per-program dispatch loop: trap a syscall, service it per the AIOS ABI, return. */
int aios_kernel_run(const char *guest_path, char *const guest_argv[]) {
    fd_table_init();
    if (pal_spawn_guest(guest_path, guest_argv) != 0) return -1;
    for (;;) {
        pal_syscall_t sc;
        int code = 0;
        int r = pal_guest_trap_next(&sc, &code);
        if (r == 0) return code;          /* guest exited (host-detected) */
        if (r < 0)  return -1;

        /* mmap and exec rewrite the guest's own syscall in place (into a host mmap / execve) and
         * consume the syscall exit themselves, so they do NOT go through pal_guest_return. exec
         * either replaces the image (the loop keeps dispatching the new program) or plants -1. */
        if (sc.nr == AIOS_SYS_MMAP) { pal_guest_mmap((size_t)sc.arg[0]); continue; }
        if (sc.nr == AIOS_SYS_EXEC) { pal_guest_exec(sc.arg[0], sc.arg[1], sc.arg[2]); continue; }

        uint64_t ret;
        switch (sc.nr) {
        case AIOS_SYS_WRITE: ret = (uint64_t)sys_write(sc.arg[0], sc.arg[1], sc.arg[2]); break;
        case AIOS_SYS_READ:  ret = (uint64_t)sys_read (sc.arg[0], sc.arg[1], sc.arg[2]); break;
        case AIOS_SYS_OPEN:  ret = (uint64_t)sys_open (sc.arg[0], sc.arg[1], sc.arg[2]); break;
        case AIOS_SYS_CLOSE: ret = (uint64_t)sys_close(sc.arg[0]);                       break;
        case AIOS_SYS_LSEEK: ret = (uint64_t)sys_lseek(sc.arg[0], sc.arg[1], sc.arg[2]); break;
        case AIOS_SYS_FSTAT: ret = (uint64_t)sys_fstat(sc.arg[0], sc.arg[1]);            break;
        case AIOS_SYS_EXIT:  return (int)sc.arg[0];   /* clean AIOS-ABI exit */
        default:             ret = (uint64_t)-1;      /* unknown AIOS syscall */          break;
        }
        if (pal_guest_return(ret) != 0) return -1;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { kputs("usage: aios-uk <guest-program> [args...]\n"); return 2; }
    const char *guest = argv[1];
    /* The guest's argv is the kernel's argv shifted by one: guest argv[0] = the guest program. */
    char *const *guest_argv = (char *const *)&argv[1];

    kputs("[aios-uk] AIOS userspace kernel -- M3d (process model: exec) (Linux/ptrace PAL)\n");
    kputs("[aios-uk] launching guest: ");
    kputs(guest);
    kputs("\n");

    int code = aios_kernel_run(guest, guest_argv);

    kputs("[aios-uk] guest exited via AIOS ABI, code=");
    kput_int(code);
    kputs("\n");
    return code;
}
