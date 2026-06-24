/*
 * aios_kernel.c -- the AIOS userspace kernel. Host-agnostic core.
 *
 * Dispatches the AIOS ABI for guest programs using ONLY the PAL (pal.h) -- no host headers, no
 * host syscalls. This exact code is meant to run unchanged over the Linux PAL (ptrace) today and
 * a future seL4 PAL (IPC) tomorrow; that portability is the whole point of the pivot
 * (docs/DESIGN_20260624_aios_userspace_kernel_on_linux.md).
 *
 * M1 (first light): service WRITE + EXIT, proving an AIOS-ABI binary runs and its syscalls are
 * trapped + serviced by this kernel. A real AIOS kernel will route WRITE through its VFS; M1
 * hands it straight to the PAL host gateway.
 */
#include "aios_abi.h"
#include "pal.h"
#include <stddef.h>
#include <stdint.h>

/* Tiny host-syscall-free helpers so this file needs no libc I/O (keeps the seam honest). */
static size_t kstrlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void kputs(int fd, const char *s) { pal_host_write(fd, s, kstrlen(s)); }
static void kput_int(int fd, long v) {              /* minimal signed-decimal, no stdio */
    char b[24]; int i = sizeof b; int neg = v < 0; unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    b[--i] = '\0';
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    pal_host_write(fd, &b[i], (size_t)(sizeof b - 1 - i));
}

/* AIOS WRITE: bounce the guest buffer out via the PAL, then emit it through the host gateway. */
static long sys_write(uint64_t fd, uint64_t gbuf, uint64_t len) {
    char tmp[1024];
    size_t total = 0;
    while (total < len) {
        size_t chunk = (size_t)len - total;
        if (chunk > sizeof tmp) chunk = sizeof tmp;
        size_t got = pal_guest_read(gbuf + total, tmp, chunk);
        if (got == 0) break;
        long w = pal_host_write((int)fd, tmp, got);
        if (w < 0) return -1;
        total += (size_t)w;
        if ((size_t)w < got) break;
    }
    return (long)total;
}

/* The kernel's per-program dispatch loop: trap a syscall, service it per the AIOS ABI, return. */
int aios_kernel_run(const char *guest_path) {
    if (pal_spawn_guest(guest_path) != 0) return -1;
    for (;;) {
        pal_syscall_t sc;
        int code = 0;
        int r = pal_guest_trap_next(&sc, &code);
        if (r == 0) return code;          /* guest exited (host-detected) */
        if (r < 0)  return -1;

        uint64_t ret;
        switch (sc.nr) {
        case AIOS_SYS_WRITE:
            ret = (uint64_t)sys_write(sc.arg[0], sc.arg[1], sc.arg[2]);
            break;
        case AIOS_SYS_EXIT:
            return (int)sc.arg[0];        /* clean AIOS-ABI exit */
        default:
            ret = (uint64_t)-1;           /* unknown AIOS syscall -> -1 (catches host-ABI leakage) */
            break;
        }
        if (pal_guest_return(ret) != 0) return -1;
    }
}

int main(int argc, char **argv) {
    const char *guest = (argc > 1) ? argv[1] : "./guest_hello";
    kputs(2, "[aios-uk] AIOS userspace kernel -- M1 first light (Linux/ptrace PAL)\n");
    kputs(2, "[aios-uk] launching guest: ");
    kputs(2, guest);
    kputs(2, "\n");

    int code = aios_kernel_run(guest);

    kputs(2, "[aios-uk] guest exited via AIOS ABI, code=");
    kput_int(2, code);
    kputs(2, "\n");
    return code;
}
