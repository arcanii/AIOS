/*
 * AIOS getty -- login gate
 *
 * Displays banner, handles authentication, then fork+exec mini_shell.
 * When shell exits (logout), loops back to login prompt.
 *
 * AiosChildApp (not PosixApp) -- manual cap parsing.
 * Links aios_posix for fork/exec/waitpid.
 */
#include <stdint.h>
#include <sel4/sel4.h>
#include "aios/version.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "aios_posix.h"
#include <pwd.h>

extern seL4_CPtr pipe_ep;

#define SER_PUTC  1
#define SER_GETC  2
#define AUTH_LOGIN 40
#define PIPE_SET_IDENTITY 74
#define TTY_IOCTL       72
#define TTY_IOCTL_SET_RAW  1
#define TTY_IOCTL_SET_COOKED 2
#define TTY_IOCTL_ECHO_ON  3
#define TTY_IOCTL_ECHO_OFF 4

static seL4_CPtr serial_ep, fs_ep, auth_ep;

/* Drop to the logged-in identity. v0.4.190: this MUST run in the forked child
 * (which is still root, inherited from getty) right before exec -- pipe_server
 * now only honours PIPE_SET_IDENTITY from a root caller, so getty itself stays
 * root across logins and the privilege drop happens per-shell in the child. */
static void drop_identity(uint32_t uid, uint32_t gid) {
    if (!pipe_ep) return;
    seL4_SetMR(0, (seL4_Word)uid);
    seL4_SetMR(1, (seL4_Word)gid);
    seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_SET_IDENTITY, 0, 0, 2));
}

/* ---- Helpers ---- */

static long parse_num(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void str_cpy(char *d, const char *s) { while ((*d++ = *s++)); }

static void ser_putc(char c) {
    seL4_SetMR(0, (seL4_Word)c);
    seL4_Call(serial_ep, seL4_MessageInfo_new(SER_PUTC, 0, 0, 1));
}
static void ser_puts(const char *s) { while (*s) ser_putc(*s++); }
static int ser_getc(void) {
    seL4_MessageInfo_t r = seL4_Call(serial_ep,
        seL4_MessageInfo_new(SER_GETC, 0, 0, 0));
    return (int)(long)seL4_GetMR(0);
}
static void tty_echo_set(int on) {
    seL4_SetMR(0, (seL4_Word)(on ? TTY_IOCTL_ECHO_ON : TTY_IOCTL_ECHO_OFF));
    seL4_Call(serial_ep, seL4_MessageInfo_new(TTY_IOCTL, 0, 0, 1));
}
static void tty_set_raw(void) {
    seL4_SetMR(0, (seL4_Word)TTY_IOCTL_SET_RAW);
    seL4_Call(serial_ep, seL4_MessageInfo_new(TTY_IOCTL, 0, 0, 1));
}
static void tty_set_cooked(void) {
    seL4_SetMR(0, (seL4_Word)TTY_IOCTL_SET_COOKED);
    seL4_Call(serial_ep, seL4_MessageInfo_new(TTY_IOCTL, 0, 0, 1));
}

/* ---- Service supervisor ----
 * Respawn a crashed long-lived network service (netconsole, sshd). getty is a
 * direct spawn (NOT a forked child), so it can re-fork -- the reverted v0.4.171
 * attempt failed only because it used a separate supervisor child, which hit the
 * AIOS no-fork-of-fork rule. Death is detected by a /proc/status NAME poll (a
 * non-blocking file read), because the waitpid shim has no WNOHANG (PIPE_WAIT
 * blocks). Rate-limited: poll ~every 4s, never respawn within ~10s of a spawn, so
 * a crash-looping service cannot spin getty. Runs only while idle at the login
 * prompt -- never during a serial session. This recovers a CRASHED service; it
 * cannot help the ~32s kernel TLBI stall (that freezes getty too). */
static uint64_t rd_cntpct(void) {
    uint64_t v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v;
}
static uint64_t sup_hz = 0;     /* cntfrq, cached */
static uint64_t sup_last = 0;   /* last /proc/status poll */
static uint64_t nc_at = 0;      /* last netconsole (re)spawn */
static uint64_t sd_at = 0;      /* last sshd (re)spawn */

/* minimal substring match (getty avoids string.h, see str_len/str_cpy above). */
static int str_has(const char *hay, const char *ndl) {
    for (; *hay; hay++) {
        const char *h = hay, *n = ndl;
        while (*n && *h == *n) { h++; n++; }
        if (!*n) return 1;
    }
    return 0;
}

static char sup_buf[8192];
static const char *read_proc_status(void) {
    int fd = open("/proc/status", O_RDONLY);
    if (fd < 0) return (const char *)0;
    int total = 0, n;
    while (total < (int)sizeof(sup_buf) - 1 &&
           (n = (int)read(fd, sup_buf + total, sizeof(sup_buf) - 1 - total)) > 0)
        total += n;
    close(fd);
    sup_buf[total] = '\0';
    return sup_buf;
}

static pid_t spawn_service(const char *path, const char *name) {
    pid_t p = fork();
    if (p == 0) {
        char *av[] = { (char *)name, (void *)0 };
        execv(path, av);
        _exit(127);
    }
    return p;
}

static void supervise(void) {
    uint64_t now = rd_cntpct();
    if (!sup_hz) {
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(sup_hz));
        if (!sup_hz) sup_hz = 54000000u;
    }
    if (sup_last && (now - sup_last) < sup_hz * 4) return;   /* poll ~every 4s */
    sup_last = now;
    const char *st = read_proc_status();
    if (!st) return;                                          /* cannot tell -> skip */
    if (!str_has(st, "netconsole") && (now - nc_at) > sup_hz * 10) {
        ser_puts("getty: netconsole down -- respawning\n");
        spawn_service("/bin/netconsole", "netconsole");
        nc_at = now;
    }
    if (!str_has(st, "sshd") && (now - sd_at) > sup_hz * 10) {
        ser_puts("getty: sshd down -- respawning\n");
        spawn_service("/bin/sshd", "sshd");
        sd_at = now;
    }
}

/* ser_getc that supervises while the serial line is idle (returns -1). */
static int ser_getc_idle(void) {
    int c = ser_getc();
    if (c < 0) supervise();
    return c;
}

/* ---- Password input with masking ---- */

static void read_password(char *buf, int max) {
    int len = 0;
    tty_set_raw();
    while (len < max - 1) {
        int c = ser_getc();
        if (c < 0) continue;
        if (c == '\r' || c == '\n') { ser_putc('\n'); break; }
        if ((c == 0x7f || c == '\b') && len > 0) {
            len--; ser_putc('\b'); ser_putc(' '); ser_putc('\b');
            continue;
        }
        if (c == 0x1b) {
            int c2 = ser_getc();
            if (c2 == '[') ser_getc();
            continue;
        }
        if (c == 0x03) { buf[0] = 0; ser_puts("^C\n"); return; }
        if (c >= 0x20 && c < 127) { buf[len++] = (char)c; ser_putc('*'); }
    }
    buf[len] = '\0';
    tty_set_cooked();
}

/* ---- Login (AUTH_LOGIN IPC) ---- */

static int do_login(uint32_t *uid, uint32_t *gid,
                    uint32_t *token, char *username) {
    if (!auth_ep) {
        /* No auth server -- auto-login as root */
        str_cpy(username, "root");
        *uid = 0; *gid = 0; *token = 1;
        return 1;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        char password[64];
        int ulen = 0;

        tty_set_raw();
        ser_puts("\nAIOS login: ");
        while (ulen < 31) {
            int c = ser_getc_idle();   /* supervises crashed services while idle */
            if (c < 0) continue;
            if (c == '\r' || c == '\n') { ser_putc('\n'); break; }
            if ((c == 0x7f || c == '\b') && ulen > 0) {
                ulen--; ser_putc('\b'); ser_putc(' '); ser_putc('\b');
                continue;
            }
            if (c == 0x1b) {
                int c2 = ser_getc();
                if (c2 == '[') ser_getc();
                continue;
            }
            if (c == 0x03) { ser_puts("^C\n"); ulen = 0; break; }
            if (c >= 0x20 && c < 127) {
                username[ulen++] = (char)c;
                ser_putc((char)c);
            }
        }
        username[ulen] = '\0';
        if (ulen == 0) { tty_set_cooked(); continue; }

        tty_set_cooked();
        ser_puts("Password: ");
        read_password(password, 64);

        /* Pack username into MRs */
        int mr = 0;
        seL4_SetMR(mr++, (seL4_Word)ulen);
        seL4_Word w = 0;
        for (int i = 0; i < ulen; i++) {
            w |= ((seL4_Word)(uint8_t)username[i]) << ((i % 8) * 8);
            if (i % 8 == 7 || i == ulen - 1) { seL4_SetMR(mr++, w); w = 0; }
        }

        /* Pack password */
        int plen = str_len(password);
        seL4_SetMR(mr++, (seL4_Word)plen);
        w = 0;
        for (int i = 0; i < plen; i++) {
            w |= ((seL4_Word)(uint8_t)password[i]) << ((i % 8) * 8);
            if (i % 8 == 7 || i == plen - 1) { seL4_SetMR(mr++, w); w = 0; }
        }

        /* Scrub password from stack */
        for (int i = 0; i < 64; i++) password[i] = 0;

        seL4_MessageInfo_t reply = seL4_Call(auth_ep,
            seL4_MessageInfo_new(AUTH_LOGIN, 0, 0, mr));
        uint32_t status = (uint32_t)seL4_GetMR(0);
        if (status == 0) {
            *uid = (uint32_t)seL4_GetMR(1);
            *gid = (uint32_t)seL4_GetMR(2);
            *token = (uint32_t)seL4_GetMR(3);
            return 1;
        }
        ser_puts("Login incorrect\n");
    }
    return 0;
}

/* ---- MOTD display via FS_CAT IPC ---- */

static void display_hw_banner(void) {
    /* Read /proc/hw: line1=cpu, line2=blk, line3=net, line4=ram */
    if (!fs_ep) {
        ser_puts("  Kernel:  seL4 15.0.0\n");
        return;
    }
    char hw[128];
    int hw_len = 0;
    /* Read /proc/hw via FS_CAT IPC */
    char hpath[] = "/proc/hw";
    int hpl = 8;
    seL4_SetMR(0, (seL4_Word)hpl);
    int hmr = 1;
    seL4_Word hw2 = 0;
    for (int i = 0; i < hpl; i++) {
        hw2 |= ((seL4_Word)(uint8_t)hpath[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == hpl - 1) { seL4_SetMR(hmr++, hw2); hw2 = 0; }
    }
    seL4_MessageInfo_t hreply = seL4_Call(fs_ep,
        seL4_MessageInfo_new(11, 0, 0, hmr));
    seL4_Word htotal = seL4_GetMR(0);
    if (htotal > 0 && htotal < 128) {
        int hrmrs = (int)seL4_MessageInfo_get_length(hreply) - 1;
        for (int i = 0; i < hrmrs && hw_len < (int)htotal; i++) {
            seL4_Word rw = seL4_GetMR(i + 1);
            for (int j = 0; j < 8 && hw_len < (int)htotal; j++)
                hw[hw_len++] = (char)((rw >> (j * 8)) & 0xFF);
        }
    }
    hw[hw_len] = 0;
    /* Parse lines: cpu_compat count\nblk\nnet\nram\n */
    char cpu[32] = {0}, blk[16] = {0}, net[16] = {0}, ram[16] = {0};
    int line = 0, pos = 0;
    char *dests[] = { cpu, blk, net, ram };
    int maxes[] = { 31, 15, 15, 15 };
    for (int i = 0; i < hw_len && line < 4; i++) {
        if (hw[i] == 10) { line++; pos = 0; }
        else if (pos < maxes[line]) { dests[line][pos++] = hw[i]; }
    }
    ser_puts("  Kernel:  seL4 15.0.0\n");
    ser_puts("  Arch:    AArch64 (");
    if (cpu[0]) ser_puts(cpu); else ser_puts("unknown");
    ser_puts(")\n");
    ser_puts("  FS:      ext2 (");
    if (blk[0] == 'e') ser_puts("eMMC SDHCI");
    else ser_puts("virtio-blk");
    ser_puts(")\n");
    if (ram[0]) {
        ser_puts("  RAM:     ");
        ser_puts(ram);
        ser_puts(" MB\n");
    }
}

static void display_motd(void) {
    if (!fs_ep) return;
    char mpath[] = "/etc/motd";
    int mpl = 9;
    seL4_SetMR(0, (seL4_Word)mpl);
    int mmr = 1;
    seL4_Word mw = 0;
    for (int i = 0; i < mpl; i++) {
        mw |= ((seL4_Word)(uint8_t)mpath[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == mpl - 1) { seL4_SetMR(mmr++, mw); mw = 0; }
    }
    seL4_MessageInfo_t mreply = seL4_Call(fs_ep,
        seL4_MessageInfo_new(11, 0, 0, mmr));
    seL4_Word mtotal = seL4_GetMR(0);
    if (mtotal > 0) {
        int mrmrs = (int)seL4_MessageInfo_get_length(mreply) - 1;
        int mgot = 0;
        for (int i = 0; i < mrmrs; i++) {
            seL4_Word rw = seL4_GetMR(i + 1);
            for (int j = 0; j < 8 && mgot < (int)mtotal; j++) {
                ser_putc((char)((rw >> (j * 8)) & 0xFF));
                mgot++;
            }
        }
    }
}

/* ---- Main: banner, login loop, fork+exec shell ---- */

int main(int argc, char *argv[]) {
    serial_ep = 0; fs_ep = 0; auth_ep = 0;
    if (argc > 0) serial_ep = (seL4_CPtr)parse_num(argv[0]);
    if (argc > 1) fs_ep     = (seL4_CPtr)parse_num(argv[1]);
    if (argc > 3) auth_ep   = (seL4_CPtr)parse_num(argv[3]);
    if (argc > 4) pipe_ep   = (seL4_CPtr)parse_num(argv[4]);

    /* Init POSIX shim (needed for fork/exec/waitpid) */
    aios_init_caps(serial_ep, fs_ep, auth_ep, pipe_ep);

    ser_puts("\n============================================\n");
    ser_puts("  " AIOS_VERSION_FULL "\n");
    ser_puts("============================================\n");

    /* Auto-start the background network services (v0.4.163 netconsole :2323 --
     * UNAUTHENTICATED root, trusted LAN/dev only; v0.4.177 sshd :2222 -- encrypted;
     * v0.4.166 one-shot SNTP wall-clock sync). Spawned HERE from getty (a
     * post-boot-settle process) via the normal fork+exec, NOT from boot_services --
     * a back-to-back boot-time spawn there raced frame reclamation and aborted the
     * root server. The children inherit the standard server caps (net/crypto/auth)
     * from the spawn machinery; sshd also gets fd1 = the tty (it must NOT be given
     * a >FILE / >/dev/null redirect -- AIOS dup2 does not route file->pipe, so that
     * would leak the shell relay output). Non-fatal: login still runs if any fail.
     * netconsole + sshd are SUPERVISED (respawned if they crash, see supervise());
     * sntp is one-shot (meant to exit after the sync) so it is not supervised. The
     * v0.4.171 supervisor-CHILD attempt was reverted (a forked child cannot fork);
     * getty itself re-forks fine, which is why the respawn lives here. */
    if (spawn_service("/bin/netconsole", "netconsole") < 0)
        ser_puts("getty: netconsole spawn failed (network shell off)\n");
    if (spawn_service("/bin/sshd", "sshd") < 0)
        ser_puts("getty: sshd spawn failed (ssh off)\n");
    spawn_service("/bin/aios/sntp", "sntp");
    /* Seed the supervisor clock so the boot grace (~10s) holds -- gives the freshly
     * spawned services time to appear in /proc/status before the first poll. */
    nc_at = sd_at = sup_last = rd_cntpct();

    while (1) {
        uint32_t uid = 0, gid = 0, token = 0;
        char username[32];
        username[0] = '\0';

        if (!do_login(&uid, &gid, &token, username)) {
            ser_puts("Too many failed attempts.\n");
            continue;
        }

        /* Identity is dropped in the forked child below (drop_identity),
         * not here -- getty stays root so it can authenticate the next login
         * (v0.4.190: PIPE_SET_IDENTITY is now root-only). */

        display_motd();
        /* Read /proc/hw for dynamic hardware info */
        display_hw_banner();
        ser_puts("  Built:   " AIOS_BUILD_DATE "\n");
        ser_puts("Welcome, "); ser_puts(username); ser_puts("\n\n");

        /* Determine login shell from /etc/passwd */
        struct passwd *pw = getpwuid(uid);
        const char *shell_path = "/bin/dash";
        const char *shell_name = "dash";
        if (pw && pw->pw_shell && pw->pw_shell[0]) {
            shell_path = pw->pw_shell;
            /* Extract basename for argv[0] */
            const char *p = pw->pw_shell;
            const char *base = p;
            while (*p) { if (*p == '/') base = p + 1; p++; }
            shell_name = base;
        }

        /* Fork+exec login shell with retry and backoff */
        int shell_started = 0;
        for (int retry = 0; retry < 3; retry++) {
            pid_t pid = fork();
            if (pid < 0) {
                ser_puts("getty: fork failed, retry ");
                ser_putc((char)('1' + retry));
                ser_puts("/3\n");
                /* Backoff: busy-wait via ARM generic timer */
                uint64_t freq, bstart, bnow;
                __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
                __asm__ volatile("mrs %0, cntpct_el0" : "=r"(bstart));
                uint64_t btarget = bstart + freq * (uint64_t)(retry + 1) / 2;
                do {
                    seL4_Yield();
                    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(bnow));
                } while (bnow < btarget);
                continue;
            }
            if (pid == 0) {
                /* Child is root (inherited); drop to the user before exec.
                 * The exec'd shell keeps this uid (pipe_server preserves the
                 * slot uid across PIPE_EXEC). */
                drop_identity(uid, gid);
                char *sh_argv[] = {(char *)shell_name, (void *)0};
                execv(shell_path, sh_argv);
                _exit(127);
            }
            shell_started = 1;
            int status = 0;
            waitpid(pid, &status, 0);
            if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                ser_puts("getty: shell killed by signal ");
                if (sig >= 10) ser_putc((char)('0' + sig / 10));
                ser_putc((char)('0' + sig % 10));
                ser_puts("\n");
            }
            break;
        }
        if (!shell_started) {
            ser_puts("getty: persistent fork failure, running shell in-process\n");
            /* getty is still root here (gate allows it); drop before exec. NOTE
             * this consumes getty -- no further logins after this fallback. */
            drop_identity(uid, gid);
            char *sh_argv[] = {(char *)shell_name, (void *)0};
            execv(shell_path, sh_argv);
            /* execv returned -- log and fall through to login */
            ser_puts("getty: execv failed, returning to login\n");
        }
        ser_puts("\n");
    }
    return 0;
}
