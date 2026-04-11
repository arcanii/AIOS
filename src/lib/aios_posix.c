/*
 * AIOS POSIX Shim -- orchestrator (MODULARIZED v0.4.58)
 *
 * Shared state, helper functions, fd table, init, and __wrap_main.
 * Syscall handlers live in posix_file/stat/dir/proc/time/misc/thread.c
 */
#include "posix_internal.h"
#include <arch_stdio.h>
#include <muslcsys/vsyscall.h>

/* ---- Shared global state ---- */
seL4_CPtr ser_ep = 0;
seL4_CPtr fs_ep_cap = 0;
seL4_CPtr thread_ep = 0;
seL4_CPtr auth_ep = 0;
seL4_CPtr pipe_ep = 0;
seL4_CPtr net_ep = 0;
seL4_CPtr disp_ep = 0;

char aios_cwd[256] = "/";
char aios_progpath[128];  /* v0.4.78: stored for /proc/self/exe */
uint32_t aios_uid = 0;
uint32_t aios_gid = 0;
int stdout_pipe_id = -1;
int stdin_pipe_id = -1;
int stdout_redir_idx = -1;  /* REDIR_STDIO_V072 */
int stderr_redir_idx = -1;
aios_fd_t stdout_redir_copy;  /* REDIR_COPY_V072 */
aios_fd_t stderr_redir_copy;

aios_sigstate_t sigstate;
int sig_dispatching = 0;

aios_fd_t aios_fds[AIOS_MAX_FDS];
int aios_pid = 1;

/* ---- Path resolution ---- */
void resolve_path(const char *pathname, char *out, int outsz) {
    if (pathname[0] == '/') {
        int i = 0;
        while (pathname[i] && i < outsz - 1) { out[i] = pathname[i]; i++; }
        out[i] = 0;
        return;
    }
    int ci = 0;
    while (aios_cwd[ci] && ci < outsz - 2) { out[ci] = aios_cwd[ci]; ci++; }
    if (pathname[0] == '.' && pathname[1] == 0) {
        out[ci] = 0;
        return;
    }
    if (pathname[0] == '.' && pathname[1] == '/') {
        pathname += 2;
    }
    if (ci > 1) out[ci++] = '/';
    while (*pathname && ci < outsz - 1) out[ci++] = *pathname++;
    out[ci] = 0;
}

/* ---- Endpoint getters ---- */
seL4_CPtr aios_get_serial_ep(void) { return ser_ep; }
seL4_CPtr aios_get_fs_ep(void) { return fs_ep_cap; }
seL4_CPtr aios_get_auth_ep(void) { return auth_ep; }
seL4_CPtr aios_get_thread_ep(void) { return thread_ep; }
seL4_CPtr aios_get_pipe_ep(void) { return pipe_ep; }

int aios_nb_getchar(void) {
    if (!ser_ep) return -1;
    seL4_SetMR(0, 0);
    seL4_Call(ser_ep, seL4_MessageInfo_new(AIOS_SER_GETC, 0, 0, 0));
    return (int)(long)seL4_GetMR(0);
}

/* ---- fd table operations ---- */
int aios_fd_alloc(void) {
    for (int i = 0; i < AIOS_MAX_FDS; i++) {
        if (!aios_fds[i].active) return i;
    }
    return -1;
}

/* ---- IPC fetch helpers ---- */
int fetch_file(const char *path, char *buf, int bufsz) {
    if (!fs_ep_cap) return -1;
    int pl = str_len(path);
    seL4_SetMR(0, (seL4_Word)pl);
    int mr = 1;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)path[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(fs_ep_cap,
        seL4_MessageInfo_new(AIOS_FS_CAT, 0, 0, mr));
    seL4_Word total = seL4_GetMR(0);
    if (total == 0) return -1;
    int rmrs = (int)seL4_MessageInfo_get_length(reply) - 1;
    int got = 0;
    for (int i = 0; i < rmrs && got < bufsz - 1; i++) {
        seL4_Word rw = seL4_GetMR(i + 1);
        for (int j = 0; j < 8 && got < (int)total && got < bufsz - 1; j++)
            buf[got++] = (char)((rw >> (j * 8)) & 0xFF);
    }
    buf[got] = '\0';
    return got;
}

int fetch_pread(const char *path, int offset, char *buf, int count) {
    if (!fs_ep_cap) return -1;
    int pl = str_len(path);
    /* MR0=path_len, MR1=offset, MR2=max_bytes, MR3+=path */
    seL4_SetMR(0, (seL4_Word)pl);
    seL4_SetMR(1, (seL4_Word)offset);
    seL4_SetMR(2, (seL4_Word)count);
    int mr = 3;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)path[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(fs_ep_cap,
        seL4_MessageInfo_new(AIOS_FS_PREAD, 0, 0, mr));
    seL4_Word got = seL4_GetMR(0);
    if (got == 0) return 0;
    int rmrs = (int)seL4_MessageInfo_get_length(reply) - 1;
    int copied = 0;
    for (int i = 0; i < rmrs && copied < (int)got; i++) {
        seL4_Word rw = seL4_GetMR(i + 1);
        for (int j = 0; j < 8 && copied < (int)got; j++)
            buf[copied++] = (char)((rw >> (j * 8)) & 0xFF);
    }
    return copied;
}

int fetch_pwrite(const char *path, int offset, const char *data, int len) {
    if (!fs_ep_cap) return -1;
    int pl = str_len(path);
    seL4_SetMR(0, (seL4_Word)pl);
    seL4_SetMR(1, (seL4_Word)offset);
    seL4_SetMR(2, (seL4_Word)len);
    int mr = 3;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)path[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    for (int i = 0; i < len; i++) {
        w |= ((seL4_Word)(uint8_t)data[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == len - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(fs_ep_cap,
        seL4_MessageInfo_new(AIOS_FS_PWRITE, 0, 0, mr));
    return (int)(long)seL4_GetMR(0);
}

int fetch_stat(const char *path, uint32_t *mode, uint32_t *size) {
    if (!fs_ep_cap) return -1;
    int pl = str_len(path);
    seL4_SetMR(0, (seL4_Word)pl);
    int mr = 1;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)path[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(fs_ep_cap,
        seL4_MessageInfo_new(12 /* FS_STAT */, 0, 0, mr));
    if (seL4_GetMR(0) == 0) return -1;
    *mode = (uint32_t)seL4_GetMR(1);
    *size = (uint32_t)seL4_GetMR(2);
    return 0;
}

size_t aios_stdio_write(void *data, size_t count) {
    /* v0.4.72: file redirect for shell > and >> */
    if (stdout_redir_idx >= 0) {
        aios_fd_t *rf = &stdout_redir_copy;
        if (rf->active && rf->path[0] && fs_ep_cap) {
            const char *src = (const char *)data;
            size_t total = 0;
            while (total < count) {
                int chunk = (int)(count - total);
                if (chunk > 800) chunk = 800;
                int wrote = fetch_pwrite(rf->path,
                    rf->is_append ? rf->size : rf->pos,
                    src + total, chunk);
                if (wrote <= 0) break;
                total += wrote;
                rf->pos += wrote;
                if (rf->pos > rf->size) rf->size = rf->pos;
            }
            return total;
        }
    }
    /* Check for pipe redirect (stdout_pipe_id set by __wrap_main) */
    if (stdout_pipe_id >= 0 && pipe_ep) {
        const char *src = (const char *)data;
        size_t sent = 0;
        while (sent < count) {
            int chunk = (int)(count - sent);
            if (chunk > 900) chunk = 900;
            seL4_SetMR(0, (seL4_Word)stdout_pipe_id);
            seL4_SetMR(1, (seL4_Word)chunk);
            int mr = 2;
            seL4_Word w = 0;
            for (int i = 0; i < chunk; i++) {
                w |= ((seL4_Word)(uint8_t)src[sent + i]) << ((i % 8) * 8);
                if (i % 8 == 7 || i == chunk - 1) { seL4_SetMR(mr++, w); w = 0; }
            }
            seL4_Call(pipe_ep, seL4_MessageInfo_new(61, 0, 0, mr));
            sent += chunk;
        }
        return count;
    }
    if (!ser_ep) return count;
    char *buf = (char *)data;
    for (size_t i = 0; i < count; i++) {
        seL4_SetMR(0, (seL4_Word)(uint8_t)buf[i]);
        seL4_Call(ser_ep, seL4_MessageInfo_new(AIOS_SER_PUTC, 0, 0, 1));
    }
    return count;
}

int aios_getchar(void) {
    if (!ser_ep) return -1;
    seL4_MessageInfo_t reply = seL4_Call(ser_ep,
        seL4_MessageInfo_new(AIOS_SER_GETC, 0, 0, 0));
    return (int)(long)seL4_GetMR(0);
}

int fetch_dir_as_getdents(const char *path, char *buf, int bufsz) {
    if (!fs_ep_cap) return -1;
    /* Multi-round: fetch raw listing in chunks via offset */
    char raw[4096];
    int pl = str_len(path);
    int got = 0;
    int total = 0;
    int offset = 0;

    do {
        seL4_SetMR(0, (seL4_Word)pl);
        seL4_SetMR(1, (seL4_Word)offset);  /* chunk offset */
        int mr = 2;  /* path starts at MR2 */
        seL4_Word w = 0;
        for (int i = 0; i < pl; i++) {
            w |= ((seL4_Word)(uint8_t)path[i]) << ((i % 8) * 8);
            if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
        }
        seL4_MessageInfo_t reply = seL4_Call(fs_ep_cap,
            seL4_MessageInfo_new(10 /* FS_LS */, 0, 0, mr));
        total = (int)seL4_GetMR(0);
        if (total == 0) return -1;

        int rmrs = (int)seL4_MessageInfo_get_length(reply) - 1;
        for (int i = 0; i < rmrs && got < 4095; i++) {
            seL4_Word rw = seL4_GetMR(i + 1);
            for (int j = 0; j < 8 && got < total && got < 4095; j++)
                raw[got++] = (char)((rw >> (j * 8)) & 0xFF);
        }
        offset = got;
    } while (got < total && got < 4095);

    raw[got] = '\0';

    /* Parse "d name\n" or "- name\n" lines into struct dirent format */
    /* struct dirent: d_ino(8) d_off(8) d_reclen(2) d_type(1) d_name[256] */
    int pos = 0;  /* position in raw */
    int out = 0;  /* position in buf */
    uint64_t fake_ino = 100;
    uint64_t d_off_counter = 0;

    while (pos < got && out < bufsz - 280) {
        char type_ch = raw[pos];
        if (type_ch != 'd' && type_ch != '-') break;
        pos += 2; /* skip "d " or "- " */

        /* Extract name */
        char name[256];
        int nl = 0;
        while (pos < got && raw[pos] != '\n' && nl < 255) {
            name[nl++] = raw[pos++];
        }
        name[nl] = '\0';
        if (pos < got && raw[pos] == '\n') pos++;

        /* Build dirent */
        int reclen = (8 + 8 + 2 + 1 + nl + 1 + 7) & ~7; /* align to 8 */
        if (reclen < 24) reclen = 24;
        if (out + reclen > bufsz) break;

        d_off_counter += reclen;
        uint8_t d_type = (type_ch == 'd') ? 4 : 8; /* DT_DIR=4, DT_REG=8 */

        /* d_ino (8 bytes) */
        uint64_t ino = fake_ino++;
        for (int i = 0; i < 8; i++) buf[out + i] = (char)((ino >> (i*8)) & 0xFF);
        /* d_off (8 bytes) */
        for (int i = 0; i < 8; i++) buf[out + 8 + i] = (char)((d_off_counter >> (i*8)) & 0xFF);
        /* d_reclen (2 bytes) */
        buf[out + 16] = (char)(reclen & 0xFF);
        buf[out + 17] = (char)((reclen >> 8) & 0xFF);
        /* d_type (1 byte) */
        buf[out + 18] = (char)d_type;
        /* d_name */
        for (int i = 0; i < nl; i++) buf[out + 19 + i] = name[i];
        buf[out + 19 + nl] = '\0';
        /* Zero padding */
        for (int i = 19 + nl + 1; i < reclen; i++) buf[out + i] = 0;

        out += reclen;
    }
    return out;
}

/* ---- Signal dispatch ---- */
static void aios_exit_cb(int code);

int aios_sig_check(void) {
    if (!pipe_ep) return 0;
    if (sig_dispatching) return 0;
    if (!sigstate.initialized) aios_sigstate_init(&sigstate);

    sig_dispatching = 1;

    /* Fetch and clear server-side pending bits */
    seL4_MessageInfo_t sr = seL4_Call(pipe_ep,
        seL4_MessageInfo_new(PIPE_SIG_FETCH, 0, 0, 0));
    (void)sr;
    uint32_t server_pending = (uint32_t)seL4_GetMR(0);
    sigstate.pending |= server_pending;

    /* Dispatch unblocked pending signals (lowest first, POSIX order) */
    uint32_t deliverable = sigstate.pending & ~(uint32_t)sigstate.blocked;
    if (!deliverable) { sig_dispatching = 0; return 0; }

    int dispatched = 0;
    for (int sig = 1; sig < AIOS_NSIG; sig++) {
        if (!(deliverable & SIG_BIT(sig))) continue;

        sigstate.pending &= ~SIG_BIT(sig);

        aios_k_sigaction_t *sa = &sigstate.actions[sig];
        void (*handler)(int) = sa->sa_handler;

        if (handler == (void (*)(int))0) {
            /* SIG_DFL */
            switch (sig) {
            case AIOS_SIGCHLD:
            case AIOS_SIGCONT:
            case 20: /* TSTP  - no job control */
            case 21: /* TTIN  - no job control */
            case 22: /* TTOU  - no job control */
            case 23: /* URG   */
                break;
            default:
                sig_dispatching = 0;
                dispatched++;
                aios_exit_cb(128 + sig);
                break; /* does not return */
            }
        } else if (handler == (void (*)(int))1) {
            /* SIG_IGN */
        } else {
            handler(sig);
            dispatched++;
        }
    }
    sig_dispatching = 0;
    return dispatched;
}

void aios_set_cwd(const char *path) {
    int i = 0;
    while (path[i] && i < 255) { aios_cwd[i] = path[i]; i++; }
    aios_cwd[i] = '\0';
}

/* Set pipe redirect IDs for the next execv() call.
 * These get packed into MR0 of PIPE_EXEC so the new process
 * inherits stdout/stdin pipe redirection. */
void aios_set_pipe_redirect(int stdout_pipe, int stdin_pipe) {
    stdout_pipe_id = stdout_pipe;
    stdin_pipe_id = stdin_pipe;
}
/* Return the pipe_server pipe_id for an aios fd, or -1 */
int aios_get_pipe_id(int fd) {
    if (fd < AIOS_FD_BASE || fd >= AIOS_FD_BASE + AIOS_MAX_FDS) return -1;
    aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
    if (!f->active || !f->is_pipe) return -1;
    return f->pipe_id;
}

/* ── Init ── */
/* v0.4.80: dynamic environment from /etc/environment + /etc/hostname */
#include "aios/config.h"

#define AIOS_ENVP_MAX 16
#define AIOS_ENVP_SZ  128
static char env_storage[AIOS_ENVP_MAX][AIOS_ENVP_SZ];
static char *aios_envp[AIOS_ENVP_MAX + 1];
static int env_count = 0;

static void env_add(const char *kv) {
    if (env_count >= AIOS_ENVP_MAX) return;
    int i = 0;
    while (kv[i] && i < AIOS_ENVP_SZ - 1) { env_storage[env_count][i] = kv[i]; i++; }
    env_storage[env_count][i] = 0;
    aios_envp[env_count] = env_storage[env_count];
    env_count++;
    aios_envp[env_count] = 0;  /* NULL terminator */
}

/* Check if env var with given prefix exists */
static int env_has(const char *prefix) {
    int plen = 0;
    while (prefix[plen]) plen++;
    for (int i = 0; i < env_count; i++) {
        int match = 1;
        for (int j = 0; j < plen; j++)
            if (env_storage[i][j] != prefix[j]) { match = 0; break; }
        if (match) return 1;
    }
    return 0;
}

static void env_init_defaults(void) {
    env_count = 0;
    aios_envp[0] = 0;

    /* Try to load from /etc/environment via IPC */
    if (fs_ep_cap) {
        char buf[1024];
        int len = fetch_file("/etc/environment", buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = 0;
            cfg_file_t cfg;
            cfg_parse_kv(buf, len, &cfg);
            for (int i = 0; i < cfg.count; i++) {
                char tmp[AIOS_ENVP_SZ];
                int ti = 0, ki = 0, vi = 0;
                while (cfg.entries[i].key[ki] && ti < AIOS_ENVP_SZ - 2)
                    tmp[ti++] = cfg.entries[i].key[ki++];
                tmp[ti++] = '=';
                while (cfg.entries[i].value[vi] && ti < AIOS_ENVP_SZ - 1)
                    tmp[ti++] = cfg.entries[i].value[vi++];
                tmp[ti] = 0;
                env_add(tmp);
            }
        }
    }

    /* Try to load hostname from /etc/hostname */
    if (fs_ep_cap && !env_has("HOSTNAME=")) {
        char hbuf[64];
        int hlen = fetch_file("/etc/hostname", hbuf, sizeof(hbuf) - 1);
        if (hlen > 0) {
            if (hbuf[hlen-1] == '\n') hbuf[--hlen] = 0;
            hbuf[hlen] = 0;
            char htmp[80];
            int hi = 0;
            const char *pfx = "HOSTNAME=";
            while (*pfx) htmp[hi++] = *pfx++;
            for (int i = 0; i < hlen && hi < 79; i++) htmp[hi++] = hbuf[i];
            htmp[hi] = 0;
            env_add(htmp);
        }
    }

    /* Fill in any missing essentials with defaults */
    if (!env_has("HOME="))     env_add("HOME=/");
    if (!env_has("PATH="))     env_add("PATH=/bin:/bin/aios");
    if (!env_has("USER="))     env_add("USER=root");
    if (!env_has("SHELL="))    env_add("SHELL=/bin/dash");
    if (!env_has("TERM="))     env_add("TERM=vt100");
    if (!env_has("HOSTNAME=")) env_add("HOSTNAME=aios");
}

void aios_exit_cb(int code) {
    /* Flush musl stdio before dying — exit via VM fault
     * bypasses atexit handlers so buffered output would be lost */
    fflush(stdout);
    fflush(stderr);
    if (pipe_ep) {
        /* Close pipe ends before exiting. This is the canonical path
         * for ALL process types (exec_server and pipe_server children).
         * handle_child_fault provides backup for crashes where exit_cb
         * never runs, but checks write_closed to avoid double-decrement. */
        if (stdout_pipe_id >= 0) {
            seL4_SetMR(0, (seL4_Word)stdout_pipe_id);
            seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_CLOSE_WRITE, 0, 0, 1));
        }
        if (stdin_pipe_id >= 0) {
            seL4_SetMR(0, (seL4_Word)stdin_pipe_id);
            seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_CLOSE_READ, 0, 0, 1));
        }
        seL4_SetMR(0, (seL4_Word)code);
        seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_EXIT, 0, 0, 1));
    }
    /* Fault to trigger reaper */
    volatile int *p = (volatile int *)0;
    *p = 0;
    __builtin_unreachable();
}

void aios_init(seL4_CPtr serial_ep, seL4_CPtr fs_endpoint) {
    /* Skip if already initialized (e.g. by __wrap_main) */
    if (ser_ep && serial_ep == 0) return;
    ser_ep = serial_ep;
    fs_ep_cap = fs_endpoint;

    /* Set libc environ */
    extern char **environ;
    env_init_defaults();
    environ = aios_envp;

    /* Set CWD from PWD env if available */
    for (int i = 0; aios_envp[i]; i++) {
        if (aios_envp[i][0]=='.' && aios_envp[i][1]=='.' && aios_envp[i][2]=='.' && aios_envp[i][3]=='.') {
            aios_set_cwd(aios_envp[i] + 4);
            break;
        }
    }

    /* Clear fd table */
    for (int i = 0; i < AIOS_MAX_FDS; i++) aios_fds[i].active = 0;

    /* Register stdio write hook */
    sel4muslcsys_register_stdio_write_fn(aios_stdio_write);

    /* Override syscalls */

    /* v0.4.78: Linux compat syscall numbers (AArch64) */
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

    muslcsys_install_syscall(__NR_write, aios_sys_write);
    muslcsys_install_syscall(__NR_read, aios_sys_read);
    muslcsys_install_syscall(__NR_close, aios_sys_close);
    muslcsys_install_syscall(__NR_writev, aios_sys_writev);
    muslcsys_install_syscall(__NR_readv, aios_sys_readv);
#ifdef __NR_open
    muslcsys_install_syscall(__NR_open, aios_sys_open);
#endif
#ifdef __NR_openat
    muslcsys_install_syscall(__NR_openat, aios_sys_openat);
#endif
#ifdef __NR_lseek
    muslcsys_install_syscall(__NR_lseek, aios_sys_lseek);
#endif
#ifdef __NR_fstat
    muslcsys_install_syscall(__NR_fstat, aios_sys_fstat);
#endif
/* fstatat on aarch64 */
    muslcsys_install_syscall(__NR_fstatat, aios_sys_fstatat);
#ifdef __NR_statx
    muslcsys_install_syscall(__NR_statx, aios_sys_statx);
#endif

    /* Easy POSIX stubs */
    muslcsys_install_syscall(__NR_exit, aios_sys_exit);
#ifdef __NR_mkdirat
    muslcsys_install_syscall(__NR_mkdirat, aios_sys_mkdirat);
#endif
#ifdef __NR_utimensat
    muslcsys_install_syscall(__NR_utimensat, aios_sys_utimensat);
#endif
#ifdef __NR_umask
    muslcsys_install_syscall(__NR_umask, aios_sys_umask);
#endif
#ifdef __NR_unlinkat
    muslcsys_install_syscall(__NR_unlinkat, aios_sys_unlinkat);
#endif
    muslcsys_install_syscall(__NR_exit_group, aios_sys_exit_group);
    muslcsys_install_syscall(__NR_chdir, aios_sys_chdir);
    muslcsys_install_syscall(__NR_getcwd, aios_sys_getcwd);
    muslcsys_install_syscall(__NR_getpid, aios_sys_getpid);
    muslcsys_install_syscall(__NR_getppid, aios_sys_getppid);
    muslcsys_install_syscall(__NR_getuid, aios_sys_getuid);
    muslcsys_install_syscall(__NR_geteuid, aios_sys_geteuid);
    muslcsys_install_syscall(__NR_getgid, aios_sys_getgid);
    muslcsys_install_syscall(__NR_getegid, aios_sys_getegid);
    muslcsys_install_syscall(__NR_uname, aios_sys_uname);
    muslcsys_install_syscall(__NR_ioctl, aios_sys_ioctl);
    muslcsys_install_syscall(__NR_fcntl, aios_sys_fcntl);
#ifdef __NR_dup
    muslcsys_install_syscall(__NR_dup, aios_sys_dup);
#endif
    muslcsys_install_syscall(__NR_dup3, aios_sys_dup3);
#ifdef __NR_access
    muslcsys_install_syscall(__NR_access, aios_sys_access);
#endif
    muslcsys_install_syscall(__NR_faccessat, aios_sys_faccessat);
    muslcsys_install_syscall(__NR_clock_gettime, aios_sys_clock_gettime);
    muslcsys_install_syscall(__NR_gettimeofday, aios_sys_gettimeofday);
    muslcsys_install_syscall(__NR_nanosleep, aios_sys_nanosleep);
    muslcsys_install_syscall(__NR_getdents64, aios_sys_getdents64);
#ifdef __NR_clone
    muslcsys_install_syscall(__NR_clone, aios_sys_clone);
#endif
#ifdef __NR_execve
    muslcsys_install_syscall(__NR_execve, aios_sys_execve);
#endif
#ifdef __NR_wait4
    muslcsys_install_syscall(__NR_wait4, aios_sys_wait4);
#endif
#ifdef __NR_exit_group
    muslcsys_install_syscall(__NR_exit_group, aios_sys_exit_group);
#endif

    /* Signal infrastructure (v0.4.53) */
    muslcsys_install_syscall(__NR_rt_sigaction, aios_sys_rt_sigaction);
    muslcsys_install_syscall(__NR_rt_sigprocmask, aios_sys_rt_sigprocmask);
    muslcsys_install_syscall(__NR_rt_sigpending, aios_sys_rt_sigpending);
    muslcsys_install_syscall(__NR_kill, aios_sys_kill);
    muslcsys_install_syscall(__NR_tgkill, aios_sys_tgkill);

    /* Register exit callback so sel4runtime_exit sends exit code via IPC */
    sel4runtime_set_exit(aios_exit_cb);
#ifdef __NR_pipe2
    muslcsys_install_syscall(__NR_pipe2, aios_sys_pipe2);
#endif
#ifdef __NR_renameat
    muslcsys_install_syscall(__NR_renameat, aios_sys_renameat);
#endif
#ifdef __NR_renameat2
    muslcsys_install_syscall(__NR_renameat2, aios_sys_renameat);
#endif
#ifdef __NR_ftruncate
    muslcsys_install_syscall(__NR_ftruncate, aios_sys_ftruncate);
    muslcsys_install_syscall(__NR_times, aios_sys_times);

    /* v0.4.62: extended POSIX syscalls (17 new) */
    muslcsys_install_syscall(__NR_pread64, aios_sys_pread64);
    muslcsys_install_syscall(__NR_pwrite64, aios_sys_pwrite64);
    muslcsys_install_syscall(__NR_fchmod, aios_sys_fchmod);
    muslcsys_install_syscall(__NR_fchmodat, aios_sys_fchmodat);
    muslcsys_install_syscall(__NR_fchown, aios_sys_fchown);
    muslcsys_install_syscall(__NR_fchownat, aios_sys_fchownat);
    muslcsys_install_syscall(__NR_linkat, aios_sys_linkat);
    muslcsys_install_syscall(__NR_symlinkat, aios_sys_symlinkat);
    muslcsys_install_syscall(__NR_readlinkat, aios_sys_readlinkat);
    muslcsys_install_syscall(__NR_setuid, aios_sys_setuid);
    muslcsys_install_syscall(__NR_setgid, aios_sys_setgid);
    muslcsys_install_syscall(__NR_setsid, aios_sys_setsid);
    muslcsys_install_syscall(__NR_getpgid, aios_sys_getpgid);
    muslcsys_install_syscall(__NR_rt_sigreturn, aios_sys_rt_sigreturn);
    muslcsys_install_syscall(__NR_sigaltstack, aios_sys_sigaltstack);
    muslcsys_install_syscall(__NR_clock_nanosleep, aios_sys_clock_nanosleep);
    muslcsys_install_syscall(__NR_mprotect, aios_sys_mprotect);

    /* v0.4.64: dash prerequisites */
    muslcsys_install_syscall(__NR_setpgid, aios_sys_setpgid);

    /* M3+M4: socket syscalls */
    muslcsys_install_syscall(__NR_listen, aios_sys_listen);
    muslcsys_install_syscall(202, aios_sys_accept4);  /* __NR_accept */
    muslcsys_install_syscall(242, aios_sys_accept4);  /* __NR_accept4 */
    muslcsys_install_syscall(__NR_socket, aios_sys_socket);
    muslcsys_install_syscall(__NR_bind, aios_sys_bind);
    muslcsys_install_syscall(__NR_sendto, aios_sys_sendto);
    muslcsys_install_syscall(__NR_recvfrom, aios_sys_recvfrom);
    muslcsys_install_syscall(__NR_setsockopt, aios_sys_setsockopt);
    muslcsys_install_syscall(__NR_shutdown_sock, aios_sys_shutdown_sock);

    /* v0.4.78: Linux compatibility syscalls */
    muslcsys_install_syscall(__NR_ppoll, aios_sys_ppoll);
    muslcsys_install_syscall(__NR_pselect6, aios_sys_pselect6);
    muslcsys_install_syscall(__NR_getrandom, aios_sys_getrandom);
    muslcsys_install_syscall(__NR_prlimit64, aios_sys_prlimit64);
    muslcsys_install_syscall(__NR_prctl, aios_sys_prctl);
    muslcsys_install_syscall(__NR_getrlimit, aios_sys_getrlimit);
    muslcsys_install_syscall(__NR_setrlimit, aios_sys_setrlimit);
    muslcsys_install_syscall(__NR_sysinfo, aios_sys_sysinfo);
    muslcsys_install_syscall(__NR_getrusage, aios_sys_getrusage);
    muslcsys_install_syscall(__NR_membarrier, aios_sys_membarrier);

    /* v0.4.79: futex for multithreaded programs */
    muslcsys_install_syscall(__NR_futex, aios_sys_futex);


#endif
}

void aios_init_full(seL4_CPtr serial_ep_arg, seL4_CPtr fs_ep_arg,
                     seL4_CPtr thread_ep_arg) {
    thread_ep = thread_ep_arg;
    aios_init(serial_ep_arg, fs_ep_arg);
}

/* aios_init_caps: initialize all endpoint caps for ChildApps
 * that link aios_posix without using __wrap_main */
void aios_init_caps(seL4_CPtr serial, seL4_CPtr fs,
                    seL4_CPtr _auth, seL4_CPtr pip) {
    aios_init(serial, fs);
    auth_ep = _auth;
    pipe_ep = pip;
}

/* __wrap_main: intercepts main() to strip cap args from argv.
 * exec_thread passes: argv[0]=serial_ep, argv[1]=fs_ep, argv[2..]=real args
 * We init the POSIX shim, then call real main with clean argv.
 */
static long _auto_parse(const char *s) {
    if (!s) return 0;
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

int __real_main(int argc, char **argv) __attribute__((weak));

int __wrap_main(int argc, char **argv) {
    /* argv layout: [ser, fs, thread, auth, pipe, net, CWD, progname, arg1, ...] */
    seL4_CPtr serial = 0, fs = 0, thr = 0, ath = 0, pip = 0, nt = 0;
    if (argc > 0 && argv[0]) serial = (seL4_CPtr)_auto_parse(argv[0]);
    if (argc > 1 && argv[1]) fs = (seL4_CPtr)_auto_parse(argv[1]);
    if (argc > 2 && argv[2]) thr = (seL4_CPtr)_auto_parse(argv[2]);
    if (argc > 3 && argv[3]) ath = (seL4_CPtr)_auto_parse(argv[3]);
    if (argc > 4 && argv[4]) pip = (seL4_CPtr)_auto_parse(argv[4]);
    if (argc > 5 && argv[5]) nt  = (seL4_CPtr)_auto_parse(argv[5]);
    thread_ep = thr;
    auth_ep = ath;
    pipe_ep = pip;
    net_ep = nt;
    seL4_CPtr dsp = 0;
    if (argc > 6 && argv[6]) dsp = (seL4_CPtr)_auto_parse(argv[6]);
    disp_ep = dsp;
    aios_init(serial, fs);

    /* Parse uid:gid:/path from argv[7] (shifted by net_ep at [5]) */
    if (argc > 7 && argv[7]) {
        const char *s = argv[7];
        if (s[0] >= '0' && s[0] <= '9') {
            /* Format: uid:gid:[spipe:rpipe:]/path */
            uint32_t uid = 0;
            while (*s >= '0' && *s <= '9') { uid = uid * 10 + (*s - '0'); s++; }
            if (*s == ':') s++;
            uint32_t gid = 0;
            while (*s >= '0' && *s <= '9') { gid = gid * 10 + (*s - '0'); s++; }
            if (*s == ':') s++;
            aios_uid = uid;
            aios_gid = gid;
            /* v0.4.80: update USER= env var from login uid */
            if (uid == 0) {
                env_add("USER=root");
            } else {
                char _ubuf[32];
                int _ui = 0;
                const char *_pfx = "USER=";
                while (*_pfx) _ubuf[_ui++] = *_pfx++;
                /* Convert uid to string */
                if (uid >= 1000) _ubuf[_ui++] = '0' + (uid/1000)%10;
                if (uid >= 100) _ubuf[_ui++] = '0' + (uid/100)%10;
                if (uid >= 10) _ubuf[_ui++] = '0' + (uid/10)%10;
                _ubuf[_ui++] = '0' + uid%10;
                _ubuf[_ui] = 0;
                env_add(_ubuf);
            }
            /* Check for optional spipe:rpipe: before /path */
            if (*s >= '0' && *s <= '9') {
                int sp = 0;
                while (*s >= '0' && *s <= '9') { sp = sp * 10 + (*s - '0'); s++; }
                if (*s == ':') s++;
                int rp = 0;
                while (*s >= '0' && *s <= '9') { rp = rp * 10 + (*s - '0'); s++; }
                if (*s == ':') s++;
                if (sp != 99) stdout_pipe_id = sp;
                if (rp != 99) stdin_pipe_id = rp;
            }
            if (*s == '/') aios_set_cwd(s);
        } else if (s[0] == '/') {
            aios_set_cwd(s);
        }
    }

    /* Force unbuffered stdout — AIOS has no real terminal,
     * musl defaults to full buffering, which loses output on exit */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* v0.4.78: store program path for /proc/self/exe */
    if (argc > 8 && argv[8]) {
        int pi = 0;
        while (argv[8][pi] && pi < 127) { aios_progpath[pi] = argv[8][pi]; pi++; }
        aios_progpath[pi] = 0;
    }

    /* Strip 8 args (ser, fs, thread, auth, pipe, net, disp, cwd) */
    return __real_main(argc - 8, argv + 8);
}
