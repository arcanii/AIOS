/*
 * uk/pal/sel4/boot.c -- the REAL seL4 PAL backend, Phase A: the AIOS userspace kernel boots as an
 * seL4 root task on qemu-arm-virt and prints its banner over serial.
 *
 * This is the seL4 counterpart of pal_linux_common.c -- but it is a seL4 ROOT TASK, not a Linux
 * tracer. It grows phase by phase (docs/PLAN_20260709_sel4_real_port.md); THIS file is Phase A:
 *   - a real console: pal_host_write -> seL4_DebugPutChar (the debug kernel's serial), so the
 *     host-agnostic kernel's kputs banner appears over QEMU serial;
 *   - a real root-task main() that calls the (unchanged) aios_kernel.c entry, compiled with
 *     -Dmain=aios_kernel_main so sel4runtime's main() is THIS one;
 *   - pal_guest_spawn REFUSES (Phase A has no loader/trap loop yet -- families A/B are Phase B/C),
 *     so the kernel prints its banner, fails to start init, and returns -- an honest Phase-A boot.
 * Every other pal.h primitive is a stub returning a safe error; each is made real in its phase (the
 * seL4 proof obligations are documented in uk/pal/pal_sel4.c + docs/DESIGN_20260703_pal_sel4_seam.md).
 * The uk/pal/pal_sel4.c SCAFFOLD stays untouched as the `make PAL=sel4` link-purity canary; this is
 * the file that actually boots.
 */
#include <sel4/sel4.h>

#include "pal.h"            /* the contract (pulls in aios_abi.h) */
#include "aios_version.h"

/* aios_kernel.c's main(), renamed by -Dmain=aios_kernel_main (build plumbing; the source is byte-
 * identical). It runs the AIOS kernel: services guest syscalls until no guests remain, returns the
 * init exit code. In Phase A pal_guest_spawn refuses, so it returns quickly. */
int aios_kernel_main(int argc, char **argv);

/* --- the bring-up console (Phase A): seL4_DebugPutChar on the debug (printing) kernel ---------------
 * The verified/release kernel has printing OFF -> a later phase replaces this with a serial/console
 * server + a mapped PL011 (uk/pal/sel4/console.c). For bring-up, DebugPutChar is the honest console. */
static void con_write(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) seL4_DebugPutChar(s[i]);
}
static void con_puts(const char *s) { size_t n = 0; while (s[n]) n++; con_write(s, n); }
static void con_put_int(long v) {
    char b[24]; int i = (int)sizeof b; b[--i] = '\0';
    unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) b[--i] = '0';
    while (u) { b[--i] = (char)('0' + (u % 10)); u /= 10; }
    if (v < 0) b[--i] = '-';
    con_puts(&b[i]);
}

/* ================================ Phase A: REAL ==================================================== */

/* The host's std streams as backing handles (0/1/2). The kernel seeds AIOS_FD_* from these. */
pal_file_t pal_host_std(int which) { return (pal_file_t)which; }

/* write: the kernel's diagnostic + stdout path. Phase A routes the three std streams to the debug
 * console; any other backing is a stub (no fs/net yet). */
long pal_host_write(pal_file_t f, const void *buf, size_t len) {
    if (f == 0 || f == 1 || f == 2) { con_write((const char *)buf, len); return (long)len; }
    return -AIOS_ENOSYS;
}

/* getcwd: seed init's cwd to "/" (the confined root). Not reached in Phase A (spawn refuses first),
 * but well-defined so the kernel's init-cwd seeding is correct once spawn works. */
long pal_host_getcwd(char *buf, size_t size) {
    if (buf && size >= 2) { buf[0] = '/'; buf[1] = '\0'; return 1; }
    return -AIOS_ENOSYS;
}

/* spawn: Phase A has no ELF loader / fault trap loop (those are Phase B/C, family A/B). Announce and
 * refuse -> the kernel reports "could not start init" and returns, an honest Phase-A boot. */
pal_pid_t pal_guest_spawn(const char *path, char *const argv[]) {
    (void)path; (void)argv;
    con_puts("[pal_sel4] Phase A: root task booted on seL4; the guest loader + fault trap loop are\n");
    con_puts("[pal_sel4] Phase B/C -- not spawning a guest yet. (docs/PLAN_20260709_sel4_real_port.md)\n");
    return PAL_PID_NONE;
}

/* ================================ stubs (made real in their phase) ================================= */
/* Phase B (family A -- fault trap loop + registers + guest memory) */
int      pal_guest_next(pal_pid_t *w, pal_syscall_t *s, int *c) { (void)w;(void)s;(void)c; return -1; }
int      pal_take_term_signal(void) { return 0; }
int      pal_guest_return(pal_pid_t w, uint64_t r) { (void)w;(void)r; return -1; }
int      pal_guest_resume(pal_pid_t w) { (void)w; return -1; }
size_t   pal_guest_read (pal_pid_t w, uint64_t g, void *d, size_t n) { (void)w;(void)g;(void)d;(void)n; return 0; }
size_t   pal_guest_write(pal_pid_t w, uint64_t g, const void *s, size_t n) { (void)w;(void)g;(void)s;(void)n; return 0; }
int      pal_guest_setret(pal_pid_t w, uint64_t r) { (void)w;(void)r; return -1; }
int      pal_guest_deliver(pal_pid_t w, uint64_t h, uint64_t sg, uint64_t t, void *sv) { (void)w;(void)h;(void)sg;(void)t;(void)sv; return -1; }
int      pal_guest_sigreturn(pal_pid_t w, const void *sv) { (void)w;(void)sv; return -1; }
/* Phase C (family B -- loader/pager) */
uint64_t pal_guest_mmap(pal_pid_t w, size_t n) { (void)w;(void)n; return 0; }
int      pal_guest_exec(pal_pid_t w, const char *p, uint64_t a, uint64_t e) { (void)w;(void)p;(void)a;(void)e; return -1; }
pal_pid_t pal_guest_fork(pal_pid_t p) { (void)p; return PAL_PID_NONE; }
int      pal_guest_exit(pal_pid_t w, int c) { (void)w;(void)c; return -1; }
int      pal_host_pipe(pal_file_t *rd, pal_file_t *wr) { (void)rd;(void)wr; return -1; }
/* Phase D (family C -- fs server) */
pal_file_t pal_host_open(const char *p, uint64_t fl, uint64_t m) { (void)p;(void)fl;(void)m; return (pal_file_t)-AIOS_ENOSYS; }
long     pal_host_read (pal_file_t f, void *b, size_t n) { (void)f;(void)b;(void)n; return -AIOS_ENOSYS; }
int      pal_host_close(pal_file_t f) { (void)f; return 0; }
long long pal_host_lseek(pal_file_t f, long long o, int w) { (void)f;(void)o;(void)w; return -AIOS_ENOSYS; }
int      pal_host_fstat(pal_file_t f, struct aios_stat *o) { (void)f;(void)o; return -AIOS_ENOSYS; }
long     pal_host_getdents(pal_file_t f, void *b, size_t n) { (void)f;(void)b;(void)n; return -AIOS_ENOSYS; }
int      pal_host_stat  (const char *p, struct aios_stat *o, int fo) { (void)p;(void)o;(void)fo; return -AIOS_ENOSYS; }
int      pal_host_unlink(const char *p) { (void)p; return -AIOS_ENOSYS; }
int      pal_host_mkdir (const char *p, unsigned int m) { (void)p;(void)m; return -AIOS_ENOSYS; }
int      pal_host_rmdir (const char *p) { (void)p; return -AIOS_ENOSYS; }
int      pal_host_rename(const char *o, const char *n) { (void)o;(void)n; return -AIOS_ENOSYS; }
int      pal_host_chdir (const char *p) { (void)p; return -AIOS_ENOSYS; }
pal_file_t pal_host_openat   (pal_file_t d, const char *p, uint64_t fl, uint64_t m) { (void)d;(void)p;(void)fl;(void)m; return (pal_file_t)-AIOS_ENOSYS; }
int      pal_host_fstatat  (pal_file_t d, const char *p, struct aios_stat *o, int fo) { (void)d;(void)p;(void)o;(void)fo; return -AIOS_ENOSYS; }
int      pal_host_unlinkat (pal_file_t d, const char *p, int rd) { (void)d;(void)p;(void)rd; return -AIOS_ENOSYS; }
int      pal_host_faccessat(pal_file_t d, const char *p, int am) { (void)d;(void)p;(void)am; return -AIOS_ENOSYS; }
long     pal_host_readlink (const char *p, char *b, size_t n) { (void)p;(void)b;(void)n; return -AIOS_ENOSYS; }
int      pal_host_fchmodat (pal_file_t d, const char *p, unsigned int m, int nf) { (void)d;(void)p;(void)m;(void)nf; return -AIOS_ENOSYS; }
int      pal_host_fchownat (pal_file_t d, const char *p, unsigned int o, unsigned int g, int nf) { (void)d;(void)p;(void)o;(void)g;(void)nf; return -AIOS_ENOSYS; }
int      pal_host_symlinkat(const char *t, pal_file_t d, const char *l) { (void)t;(void)d;(void)l; return -AIOS_ENOSYS; }
int      pal_host_linkat   (pal_file_t od, const char *op, pal_file_t nd, const char *np, int fo) { (void)od;(void)op;(void)nd;(void)np;(void)fo; return -AIOS_ENOSYS; }
int      pal_host_utimensat(pal_file_t d, const char *p, const struct aios_timespec *t, int nf) { (void)d;(void)p;(void)t;(void)nf; return -AIOS_ENOSYS; }
/* Phase E (family C -- console/termios) */
int      pal_host_isatty(pal_file_t f) { (void)f; return 0; }
int      pal_host_tcgetattr(pal_file_t f, struct aios_termios *o) { (void)f;(void)o; return -AIOS_ENOSYS; }
int      pal_host_tcsetattr(pal_file_t f, int a, const struct aios_termios *i) { (void)f;(void)a;(void)i; return -AIOS_ENOSYS; }
int      pal_host_clock_gettime(int id, struct aios_timespec *o) { (void)id;(void)o; return -AIOS_ENOSYS; }
/* Phase F (family C -- net server) */
pal_file_t pal_host_socket (int d, int t, int p) { (void)d;(void)t;(void)p; return (pal_file_t)-AIOS_ENOSYS; }
int      pal_host_connect  (pal_file_t f, const void *a, unsigned int l) { (void)f;(void)a;(void)l; return -AIOS_ENOSYS; }
int      pal_host_bind     (pal_file_t f, const void *a, unsigned int l) { (void)f;(void)a;(void)l; return -AIOS_ENOSYS; }
int      pal_host_listen   (pal_file_t f, int b) { (void)f;(void)b; return -AIOS_ENOSYS; }
pal_file_t pal_host_accept (pal_file_t f, void *a, unsigned int *l) { (void)f;(void)a;(void)l; return (pal_file_t)-AIOS_ENOSYS; }
int      pal_host_setsockopt (pal_file_t f, int lv, int on, const void *ov, unsigned int ol) { (void)f;(void)lv;(void)on;(void)ov;(void)ol; return -AIOS_ENOSYS; }
int      pal_host_getsockname(pal_file_t f, void *a, unsigned int *l) { (void)f;(void)a;(void)l; return -AIOS_ENOSYS; }
int      pal_host_sock_error (pal_file_t f) { (void)f; return -AIOS_ENOSYS; }
int      pal_host_sock_writable(pal_file_t f) { (void)f; return -AIOS_ENOSYS; }
void     pal_net_watch_reset(void) { }
void     pal_net_watch_add  (pal_file_t f, int w) { (void)f;(void)w; }
void     pal_net_watch_timeout(int ms) { (void)ms; }
int      pal_net_have_watches(void) { return 0; }
int      pal_net_wait_ready (void) { return 0; }

/* ================================ the root task entry ============================================== */
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    con_puts("\n[pal_sel4] AIOS userspace kernel -- Phase A: booted as an seL4 root task (qemu-arm-virt).\n");

    /* Run the host-agnostic kernel. It prints its banner (via pal_host_write -> the console above),
     * tries to start init (pal_guest_spawn refuses in Phase A), and returns. */
    char *gargv[] = { (char *)"aios-uk", (char *)"/sbin/init", 0 };
    int code = aios_kernel_main(2, gargv);

    con_puts("[pal_sel4] Phase A complete: aios_kernel returned code=");
    con_put_int(code);
    con_puts(" (expected -1: no guest spawned). The seL4 root task is host-agnostic-kernel-clean.\n");
    con_puts("[pal_sel4] halting.\n");
    seL4_TCB_Suspend(seL4_CapInitThreadTCB);
    for (;;) { }
    return code;
}
