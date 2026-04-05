/*
 * AIOS 0.4.x miniShell — thin exec launcher
 *
 * Builtins: cd, exit
 * Everything else: search $PATH, exec full path
 *
 * argv[0] = serial_ep, argv[1] = fs_ep, argv[2] = exec_ep
 */
#include <stdint.h>
#include <sel4/sel4.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "aios_posix.h"
extern seL4_CPtr pipe_ep;

#define SER_PUTC 1
#define SER_GETC 2
#define FS_STAT  12
#define EXEC_RUN 20
#define EXEC_RUN_BG 24
#define AUTH_LOGIN  40
#define PIPE_CREATE 60
#define PIPE_CLOSE  63
#define PIPE_KILL   64
#define AUTH_SU     48
#define AUTH_PASSWD 47

static seL4_CPtr serial_ep, fs_ep, exec_ep, auth_ep;
static uint32_t session_token = 0;
static uint32_t session_uid = 0;
static uint32_t session_gid = 0;
static char session_user[32] = "???";
static char session_home[64] = "/";

/* su stack — save/restore previous identity */
#define SU_STACK_MAX 4
static struct {
    uint32_t uid, gid, token;
    char user[32];
} su_stack[SU_STACK_MAX];
static int su_depth = 0;

/* Command history */
#define HIST_MAX 16
static char hist[HIST_MAX][256];
static int hist_count = 0;
static int hist_pos = 0;

/* Background jobs tracking */
#define MAX_BG_JOBS 8
static struct { int active; int pid; char name[64]; } bg_jobs[MAX_BG_JOBS];
static int bg_count = 0;

static void ser_putc(char c) {
    seL4_SetMR(0, (seL4_Word)c);
    seL4_Call(serial_ep, seL4_MessageInfo_new(SER_PUTC, 0, 0, 1));
}
static void ser_puts(const char *s) { while (*s) ser_putc(*s++); }
static int ser_getc(void) {
    seL4_MessageInfo_t r = seL4_Call(serial_ep, seL4_MessageInfo_new(SER_GETC, 0, 0, 0));
    return (int)(long)seL4_GetMR(0);
}

static long parse_num(const char *s) {
    long v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; } return v;
}
static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; } return *a == *b;
}
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void str_cpy(char *d, const char *s) { while ((*d++ = *s++)); }

static const char *env_get(const char *key);

/* Expand $VAR references in a string */
static void expand_vars(char *out, const char *in, int maxlen) {
    int oi = 0;
    while (*in && oi < maxlen - 1) {
        if (*in == '$' && (in[1] == '_' || (in[1] >= 'A' && in[1] <= 'Z') ||
                           (in[1] >= 'a' && in[1] <= 'z'))) {
            /* Extract variable name */
            in++;
            char vname[64]; int vi = 0;
            while ((*in >= 'A' && *in <= 'Z') || (*in >= 'a' && *in <= 'z') ||
                   (*in >= '0' && *in <= '9') || *in == '_') {
                if (vi < 63) vname[vi++] = *in;
                in++;
            }
            vname[vi] = '\0';
            /* Look up in env */
            const char *val = env_get(vname);
            if (val) {
                while (*val && oi < maxlen - 1) out[oi++] = *val++;
            }
        } else {
            out[oi++] = *in++;
        }
    }
    out[oi] = '\0';
}

/* Strip matching quotes from a string in-place */
static void strip_quotes(char *s) {
    int len = str_len(s);
    if (len < 2) return;
    if ((s[0] == '"' && s[len-1] == '"') || (s[0] == '\'' && s[len-1] == '\'')) {
        for (int i = 0; i < len - 2; i++) s[i] = s[i+1];
        s[len-2] = '\0';
    }
}
static int uint_to_dec(uint32_t v, char *buf) {
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
    char tmp[12]; int ti = 0;
    while (v) { tmp[ti++] = '0' + v % 10; v /= 10; }
    int len = ti;
    while (ti--) *buf++ = tmp[ti];
    *buf = 0;
    return len;
}

/* ── Environment ── */
#define ENV_MAX 16
static char env_store[ENV_MAX][128];
static int env_count = 0;

static void env_set(const char *key, const char *val) {
    int kl = str_len(key);
    /* Update existing */
    for (int i = 0; i < env_count; i++) {
        int match = 1;
        for (int j = 0; j < kl; j++) {
            if (env_store[i][j] != key[j]) { match = 0; break; }
        }
        if (match && env_store[i][kl] == '=') {
            int p = kl + 1, vi = 0;
            while (val[vi] && p < 127) env_store[i][p++] = val[vi++];
            env_store[i][p] = '\0';
            return;
        }
    }
    /* Add new */
    if (env_count < ENV_MAX) {
        int p = 0;
        for (int i = 0; key[i] && p < 126; i++) env_store[env_count][p++] = key[i];
        env_store[env_count][p++] = '=';
        for (int i = 0; val[i] && p < 127; i++) env_store[env_count][p++] = val[i];
        env_store[env_count][p] = '\0';
        env_count++;
    }
}

static const char *env_get(const char *key) {
    int kl = str_len(key);
    for (int i = 0; i < env_count; i++) {
        int match = 1;
        for (int j = 0; j < kl; j++) {
            if (env_store[i][j] != key[j]) { match = 0; break; }
        }
        if (match && env_store[i][kl] == '=') return env_store[i] + kl + 1;
    }
    return 0;
}

/* ── CWD ── */
static char cwd[256] = "/";

static void resolve(const char *arg, char *out, int outsz) {
    if (!arg || !arg[0]) { str_cpy(out, cwd); return; }
    if (arg[0] == '/') { int i = 0; while (arg[i] && i < outsz-1) { out[i]=arg[i]; i++; } out[i]='\0'; return; }
    int ci = 0;
    while (cwd[ci] && ci < outsz-2) { out[ci]=cwd[ci]; ci++; }
    if (ci > 1) out[ci++] = '/';
    int ai = 0;
    while (arg[ai] && ci < outsz-1) { out[ci++]=arg[ai++]; }
    out[ci] = '\0';
}

/* ── Check if file exists via FS_STAT IPC ── */
static int file_exists(const char *path) {
    if (!fs_ep) return 0;
    int pl = str_len(path);
    seL4_SetMR(0, (seL4_Word)pl);
    int mr = 1;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)path[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(fs_ep, seL4_MessageInfo_new(FS_STAT, 0, 0, mr));
    return seL4_GetMR(0) != 0;
}

/* ── Search $PATH for command, return full path ── */
static int find_in_path(const char *name, char *out, int outsz) {
    /* If name contains '/', use as-is */
    for (int i = 0; name[i]; i++) {
        if (name[i] == '/') {
            int j = 0;
            while (name[j] && j < outsz - 1) { out[j] = name[j]; j++; }
            out[j] = '\0';
            return file_exists(out);
        }
    }

    /* Search each directory in PATH */
    const char *path_val = env_get("PATH");
    if (!path_val) path_val = "/bin";

    const char *p = path_val;
    while (*p) {
        /* Extract one directory */
        char dir[128];
        int di = 0;
        while (*p && *p != ':' && di < 126) dir[di++] = *p++;
        dir[di] = '\0';
        if (*p == ':') p++;

        /* Build full path: dir/name */
        int oi = 0;
        for (int i = 0; dir[i] && oi < outsz - 2; i++) out[oi++] = dir[i];
        out[oi++] = '/';
        for (int i = 0; name[i] && oi < outsz - 1; i++) out[oi++] = name[i];
        out[oi] = '\0';

        if (file_exists(out)) return 1;
    }
    return 0;
}

/* ── Fork+exec+waitpid: Unix-way command execution ── */
static int fork_exec_wait(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv(path, argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int fork_exec_bg(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv(path, argv);
        _exit(127);
    }
    return (int)pid;
}

/* Unix-way exec: parse path + args, fork+exec+waitpid */
static int do_exec(const char *path, const char *arg) {
    char *argv[32];
    int argc = 0;
    /* argv[0] = basename of path */
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    static char argv0[64];
    int bi = 0;
    while (*base && bi < 63) argv0[bi++] = *base++;
    argv0[bi] = 0;
    argv[argc++] = argv0;

    /* Split arg by spaces into argv[1..] */
    static char argbuf[512];
    if (arg) {
        int ai = 0;
        while (*arg && ai < 510) argbuf[ai++] = *arg++;
        argbuf[ai] = 0;
        char *p = argbuf;
        while (*p && argc < 31) {
            while (*p == ' ') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) { *p = 0; p++; }
        }
    }
    argv[argc] = 0;
    return fork_exec_wait(path, argv);
}

static int do_exec_bg(const char *path, const char *arg) {
    char *argv[32];
    int argc = 0;
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    static char bg_argv0[64];
    int bi = 0;
    while (*base && bi < 63) bg_argv0[bi++] = *base++;
    bg_argv0[bi] = 0;
    argv[argc++] = bg_argv0;
    static char bg_argbuf[512];
    if (arg) {
        int ai = 0;
        while (*arg && ai < 510) bg_argbuf[ai++] = *arg++;
        bg_argbuf[ai] = 0;
        char *p = bg_argbuf;
        while (*p && argc < 31) {
            while (*p == ' ') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) { *p = 0; p++; }
        }
    }
    argv[argc] = 0;
    return fork_exec_bg(path, argv);
}

/* ── Exec a command via IPC ── */
static int do_exec_piped(const char *cmdline, int stdout_pipe, int stdin_pipe) {
    if (!exec_ep) return -1;
    /* Build command with extended CWD marker: CWD=uid:gid:spipe:rpipe:/path */
    char piped_cmd[512];
    int pi = 0;
    const char *p = cmdline;

    /* Copy everything up to "CWD=" */
    while (*p) {
        if (p[0]=='C'&&p[1]=='W'&&p[2]=='D'&&p[3]=='=') break;
        piped_cmd[pi++] = *p++;
    }
    if (*p) {
        /* Found CWD= — insert pipe info after uid:gid: */
        piped_cmd[pi++] = 'C'; piped_cmd[pi++] = 'W';
        piped_cmd[pi++] = 'D'; piped_cmd[pi++] = '=';
        p += 4; /* skip CWD= */
        /* Copy uid */
        while (*p && *p != ':') piped_cmd[pi++] = *p++;
        if (*p == ':') piped_cmd[pi++] = *p++;
        /* Copy gid */
        while (*p && *p != ':') piped_cmd[pi++] = *p++;
        if (*p == ':') piped_cmd[pi++] = *p++;
        /* Insert spipe:rpipe: */
        char sp[12], rp[12];
        uint_to_dec(stdout_pipe < 0 ? 99 : (uint32_t)stdout_pipe, sp);
        uint_to_dec(stdin_pipe < 0 ? 99 : (uint32_t)stdin_pipe, rp);
        for (int x=0; sp[x]; x++) piped_cmd[pi++] = sp[x];
        piped_cmd[pi++] = ':';
        for (int x=0; rp[x]; x++) piped_cmd[pi++] = rp[x];
        piped_cmd[pi++] = ':';
        /* Copy remaining path */
        while (*p && pi < 510) piped_cmd[pi++] = *p++;
    }
    piped_cmd[pi] = '\0';

    /* Send via normal exec IPC */
    int pl = str_len(piped_cmd);
    seL4_SetMR(0, (seL4_Word)pl);
    int mr = 1;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)piped_cmd[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(exec_ep, seL4_MessageInfo_new(EXEC_RUN, 0, 0, mr));
    return (int)(long)seL4_GetMR(0);
}

static int do_exec_bg_ipc(const char *cmdline) {
    if (!exec_ep) return -1;
    int pl = str_len(cmdline);
    seL4_SetMR(0, (seL4_Word)pl);
    int mr = 1;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)cmdline[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(exec_ep, seL4_MessageInfo_new(EXEC_RUN_BG, 0, 0, mr));
    return (int)(long)seL4_GetMR(0);
}

static int do_exec_ipc(const char *cmdline) {
    if (!exec_ep) return -1;
    int pl = str_len(cmdline);
    seL4_SetMR(0, (seL4_Word)pl);
    int mr = 1;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)cmdline[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(exec_ep, seL4_MessageInfo_new(EXEC_RUN, 0, 0, mr));
    return (int)(long)seL4_GetMR(0);
}

/* ── Line editor with cursor movement ── */
#define LINE_MAX 256
static char line[LINE_MAX];

/* ESC sequence state machine: 0=normal, 1=got ESC, 2=got ESC[ */
static void read_line(int *len) {
    int cursor = 0;  /* cursor position within line */
    int esc_state = 0;
    *len = 0;

    while (*len < LINE_MAX - 1) {
        int c = ser_getc();
        if (c < 0) continue;

        /* ESC sequence state machine */
        if (esc_state == 1) {
            esc_state = (c == '[') ? 2 : 0;
            if (esc_state == 0 && c != 0x1b) goto normal;
            continue;
        }
        if (esc_state == 2) {
            esc_state = 0;
            if (c == 'D' && cursor > 0) {
                /* LEFT */
                cursor--;
                ser_puts("\033[D");
            } else if (c == 'C' && cursor < *len) {
                /* RIGHT */
                cursor++;
                ser_puts("\033[C");
            } else if (c == 'A') {
                /* UP — previous history */
                if (hist_pos > 0) {
                    hist_pos--;
                    while (cursor > 0) { ser_puts("\033[D"); cursor--; }
                    for (int x = 0; x < *len; x++) ser_putc(' ');
                    for (int x = 0; x < *len; x++) ser_puts("\033[D");
                    *len = 0; cursor = 0;
                    while (hist[hist_pos][*len] && *len < 255) {
                        line[*len] = hist[hist_pos][*len];
                        ser_putc(line[*len]);
                        (*len)++; cursor++;
                    }
                }
            } else if (c == 'B') {
                /* DOWN — next history */
                if (hist_pos < hist_count) {
                    hist_pos++;
                    while (cursor > 0) { ser_puts("\033[D"); cursor--; }
                    for (int x = 0; x < *len; x++) ser_putc(' ');
                    for (int x = 0; x < *len; x++) ser_puts("\033[D");
                    *len = 0; cursor = 0;
                    if (hist_pos < hist_count) {
                        while (hist[hist_pos][*len] && *len < 255) {
                            line[*len] = hist[hist_pos][*len];
                            ser_putc(line[*len]);
                            (*len)++; cursor++;
                        }
                    }
                }
            }
            continue;
        }
        if (c == 0x1b) { esc_state = 1; continue; }

normal:
        if (c == 0x03) {
            /* Ctrl-C — cancel current input */
            ser_puts("^C\n");
            *len = 0;
            line[0] = '\0';
            break;
        }
        if (c == '\r' || c == '\n') { ser_putc('\n'); break; }

        if ((c == 0x7f || c == '\b') && cursor > 0) {
            /* Delete char before cursor */
            cursor--;
            for (int i = cursor; i < *len - 1; i++) line[i] = line[i + 1];
            (*len)--;
            /* Redraw from cursor: move back, print rest, space over old last, reposition */
            ser_puts("\033[D");
            for (int i = cursor; i < *len; i++) ser_putc(line[i]);
            ser_putc(' ');
            /* Move cursor back to position */
            int back = *len - cursor + 1;
            for (int i = 0; i < back; i++) ser_puts("\033[D");
            continue;
        }

        if (c >= 0x20 && c < 127) {
            /* Insert char at cursor */
            for (int i = *len; i > cursor; i--) line[i] = line[i - 1];
            line[cursor] = (char)c;
            (*len)++;
            /* Print from cursor to end */
            for (int i = cursor; i < *len; i++) ser_putc(line[i]);
            cursor++;
            /* Move cursor back to position */
            int back = *len - cursor;
            for (int i = 0; i < back; i++) ser_puts("\033[D");
        }
    }
    line[*len] = '\0';
}

/* ── Login ── */
static void read_password(char *buf, int max) {
    int len = 0;
    while (len < max - 1) {
        int c = ser_getc();
        if (c < 0) continue;
        if (c == '\r' || c == '\n') { ser_putc('\n'); break; }
        if ((c == 0x7f || c == '\b') && len > 0) {
            len--; ser_putc('\b'); ser_putc(' '); ser_putc('\b'); continue;
        }
        if (c == 0x1b) {
            int c2 = ser_getc();
            if (c2 == '[') { ser_getc(); }
            continue;
        }
        if (c == 0x1b) { int c2 = ser_getc(); if (c2 == '[') ser_getc(); continue; }
        if (c >= 0x20 && c < 127) { buf[len++] = (char)c; ser_putc('*'); }
    }
    buf[len] = '\0';
}

static int do_login(void) {
    if (!auth_ep) {
        /* No auth server — auto-login as root */
        str_cpy(session_user, "root");
        str_cpy(session_home, "/");
        session_uid = 0; session_gid = 0; session_token = 1;
        return 1;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        char username[32], password[64];

        ser_puts("\nAIOS login: ");
        int ulen = 0;
        while (ulen < 31) {
            int c = ser_getc();
            if (c < 0) continue;
            if (c == '\r' || c == '\n') { ser_putc('\n'); break; }
            if ((c == 0x7f || c == '\b') && ulen > 0) {
                ulen--; ser_putc('\b'); ser_putc(' '); ser_putc('\b'); continue;
            }
            if (c == 0x1b) {
                int c2 = ser_getc();
                if (c2 == '[') { ser_getc(); }
                continue;
            }
            if (c == 0x1b) { int c2 = ser_getc(); if (c2 == '[') ser_getc(); continue; }
            if (c >= 0x20 && c < 127) { username[ulen++] = (char)c; ser_putc((char)c); }
        }
        username[ulen] = '\0';
        if (ulen == 0) continue;

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

        /* Pack password into MRs */
        int plen = str_len(password);
        seL4_SetMR(mr++, (seL4_Word)plen);
        w = 0;
        for (int i = 0; i < plen; i++) {
            w |= ((seL4_Word)(uint8_t)password[i]) << ((i % 8) * 8);
            if (i % 8 == 7 || i == plen - 1) { seL4_SetMR(mr++, w); w = 0; }
        }

        /* Clear password from stack */
        for (int i = 0; i < 64; i++) password[i] = 0;

        seL4_MessageInfo_t reply = seL4_Call(auth_ep,
            seL4_MessageInfo_new(AUTH_LOGIN, 0, 0, mr));
        uint32_t status = (uint32_t)seL4_GetMR(0);

        if (status == 0) {
            session_uid = (uint32_t)seL4_GetMR(1);
            session_gid = (uint32_t)seL4_GetMR(2);
            session_token = (uint32_t)seL4_GetMR(3);
            str_cpy(session_user, username);
            return 1;
        }
        ser_puts("Login incorrect\n");
    }
    return 0;
}

/* ── Main ── */
int main(int argc, char *argv[]) {
    serial_ep = 0; fs_ep = 0; exec_ep = 0;
    if (argc > 0) serial_ep = (seL4_CPtr)parse_num(argv[0]);
    if (argc > 1) fs_ep = (seL4_CPtr)parse_num(argv[1]);
    if (argc > 2) exec_ep = (seL4_CPtr)parse_num(argv[2]);
    if (argc > 3) auth_ep = (seL4_CPtr)parse_num(argv[3]);
    if (argc > 4) pipe_ep = (seL4_CPtr)parse_num(argv[4]);

    /* Initialize POSIX shim with endpoint caps */
    aios_init_caps(serial_ep, fs_ep, auth_ep, pipe_ep);

    /* Default environment */
    env_set("PATH", "/bin");
    env_set("HOME", "/");
    env_set("USER", "root");
    env_set("SHELL", "/bin/mini_shell");
    env_set("TERM", "vt100");

    ser_puts("\n============================================\n");
    ser_puts("  AIOS 0.4.x\n");
    ser_puts("============================================\n");

login_gate:
    session_token = 0;
    if (!do_login()) {
        ser_puts("Too many failed attempts.\n");
        goto login_gate;
    }

    /* Set env from session */
    env_set("USER", session_user);
    env_set("HOME", session_home);
    str_cpy(cwd, session_home);
    env_set("PWD", cwd);

    ser_puts("\n");
    /* Display MOTD */
    if (fs_ep) {
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
    ser_puts("Welcome, "); ser_puts(session_user); ser_puts("\n\n");

    while (1) {
        ser_puts(cwd); ser_puts(" $ ");
        int len; read_line(&len);
        if (len == 0) continue;

        /* Expand $VAR references */
        {
            char expanded[256];
            expand_vars(expanded, line, 256);
            int ei = 0;
            while (expanded[ei] && ei < 255) { line[ei] = expanded[ei]; ei++; }
            line[ei] = '\0';
            len = ei;
        }

        /* Save to history (skip duplicates) */
        if (hist_count == 0 || !str_eq(line, hist[hist_count - 1])) {
            if (hist_count >= HIST_MAX) {
                for (int hi = 0; hi < HIST_MAX - 1; hi++)
                    for (int hj = 0; hj < 256; hj++)
                        hist[hi][hj] = hist[hi+1][hj];
                hist_count = HIST_MAX - 1;
            }
            int hi = 0;
            while (hi <= len && hi < 255) { hist[hist_count][hi] = line[hi]; hi++; }
            hist[hist_count][hi] = '\0';
            hist_count++;
        }
        hist_pos = hist_count;

        /* Check for background & */
        int run_bg = 0;
        {
            int tl = str_len(line);
            if (tl > 0 && line[tl - 1] == '&') {
                run_bg = 1;
                line[tl - 1] = '\0';
                tl--;
                while (tl > 0 && line[tl - 1] == ' ') line[--tl] = '\0';
                len = tl;
            }
        }

        /* Check for pipe | (supports multi-pipe: a | b | c) */
        {
            /* Count pipe segments */
            int pipe_positions[8];
            int num_pipes = 0;
            for (int ci = 0; ci < len && num_pipes < 8; ci++) {
                if (line[ci] == '|' && (ci + 1 >= len || line[ci+1] != '|')) {
                    pipe_positions[num_pipes++] = ci;
                }
            }
            if (num_pipes > 0 && pipe_ep) {
                /* Split into segments */
                char *segments[9];  /* max 8 pipes = 9 segments */
                int num_segments = 0;
                segments[num_segments++] = line;
                for (int pi = 0; pi < num_pipes; pi++) {
                    line[pipe_positions[pi]] = '\0';
                    char *next = line + pipe_positions[pi] + 1;
                    while (*next == ' ') next++;
                    segments[num_segments++] = next;
                }
                /* Trim trailing spaces from each segment */
                for (int si = 0; si < num_segments; si++) {
                    int sl = str_len(segments[si]);
                    while (sl > 0 && segments[si][sl-1] == ' ') segments[si][--sl] = '\0';
                }

                /* Create pipe_ids between each pair */
                int pipe_ids[8];
                int pipes_ok = 1;
                for (int pi = 0; pi < num_pipes; pi++) {
                    seL4_MessageInfo_t pr = seL4_Call(pipe_ep,
                        seL4_MessageInfo_new(PIPE_CREATE, 0, 0, 0));
                    pipe_ids[pi] = (int)(long)seL4_GetMR(0);
                    if (pipe_ids[pi] < 0) { pipes_ok = 0; break; }
                }
                if (!pipes_ok) {
                    ser_puts("pipe: failed to create\n");
                    /* Clean up any created pipes */
                    for (int pi = 0; pi < num_pipes; pi++) {
                        if (pipe_ids[pi] >= 0) {
                            seL4_SetMR(0, (seL4_Word)pipe_ids[pi]);
                            seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_CLOSE, 0, 0, 1));
                        }
                    }
                    continue;
                }

                /* Execute each segment with appropriate pipe redirection */
                for (int si = 0; si < num_segments; si++) {
                    int spipe = (si < num_segments - 1) ? pipe_ids[si] : -1;  /* stdout pipe */
                    int rpipe = (si > 0) ? pipe_ids[si - 1] : -1;             /* stdin pipe */

                    /* Parse command + args for this segment */
                    char *seg = segments[si];
                    char *sarg = 0;
                    for (int i = 0; seg[i]; i++) {
                        if (seg[i] == ' ') { seg[i] = '\0'; sarg = seg+i+1; break; }
                    }
                    strip_quotes(seg);
                    if (sarg) {
                        char *p = sarg;
                        while (*p) { strip_quotes(p); while(*p&&*p!=' ')p++; while(*p==' ')p++; }
                    }

                    char spath[256];
                    if (find_in_path(seg, spath, sizeof(spath))) {
                        char scmd[512]; int sci = 0;
                        const char *cp = spath;
                        while (*cp && sci < 500) scmd[sci++] = *cp++;
                        if (sarg) { scmd[sci++] = ' '; cp = sarg; while (*cp && sci < 500) scmd[sci++] = *cp++; }
                        scmd[sci++] = ' '; scmd[sci++] = 'C'; scmd[sci++] = 'W'; scmd[sci++] = 'D'; scmd[sci++] = '=';
                        char ud[12], gd[12]; uint_to_dec(session_uid, ud); uint_to_dec(session_gid, gd);
                        for (int x=0;ud[x]&&sci<500;x++) scmd[sci++]=ud[x]; scmd[sci++]=':';
                        for (int x=0;gd[x]&&sci<500;x++) scmd[sci++]=gd[x]; scmd[sci++]=':';
                        const char *cw=cwd; while(*cw&&sci<500) scmd[sci++]=*cw++; scmd[sci]='\0';
                        do_exec_piped(scmd, spipe, rpipe);
                    } else {
                        ser_puts(seg); ser_puts(": command not found\n");
                    }
                }

                /* Close all pipes */
                for (int pi = 0; pi < num_pipes; pi++) {
                    seL4_SetMR(0, (seL4_Word)pipe_ids[pi]);
                    seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_CLOSE, 0, 0, 1));
                }
                continue;
            }
        }

        /* Check for && / || chains first (before command split) */
        {
            int chain_pos = -1, chain_type = 0;
            for (int ci = 0; ci < len - 1; ci++) {
                if (line[ci] == '&' && line[ci+1] == '&') { chain_pos = ci; chain_type = 1; break; }
                if (line[ci] == '|' && line[ci+1] == '|') { chain_pos = ci; chain_type = 2; break; }
            }
            if (chain_type) {
                /* Split into left and right commands */
                line[chain_pos] = '\0';
                char *right_cmd = line + chain_pos + 2;
                while (*right_cmd == ' ') right_cmd++;
                /* Trim trailing spaces from left */
                int ll = chain_pos - 1;
                while (ll >= 0 && line[ll] == ' ') line[ll--] = '\0';

                /* Execute left command */
                char *larg = 0;
                for (int i = 0; line[i]; i++) {
                    if (line[i] == ' ') { line[i] = '\0'; larg = line + i + 1; break; }
                }
                int left_ok = 0;
                /* Check builtins */
                if (str_eq(line, "true")) { left_ok = 1; }
                else if (str_eq(line, "false")) { left_ok = 0; }
                else {
                    char lpath[256];
                    if (find_in_path(line, lpath, sizeof(lpath))) {
                        char lcmd[512]; int lci = 0;
                        const char *p = lpath;
                        while (*p && lci < 500) lcmd[lci++] = *p++;
                        if (larg) { lcmd[lci++] = ' '; p = larg; while (*p && lci < 500) lcmd[lci++] = *p++; }
                        lcmd[lci++] = ' '; lcmd[lci++] = 'C'; lcmd[lci++] = 'W'; lcmd[lci++] = 'D'; lcmd[lci++] = '=';
                        char ud[12], gd[12]; uint_to_dec(session_uid, ud); uint_to_dec(session_gid, gd);
                        for (int x=0;ud[x]&&lci<500;x++) lcmd[lci++]=ud[x]; lcmd[lci++]=':';
                        for (int x=0;gd[x]&&lci<500;x++) lcmd[lci++]=gd[x]; lcmd[lci++]=':';
                        const char *cw=cwd; while(*cw&&lci<500) lcmd[lci++]=*cw++; lcmd[lci]='\0';
                        left_ok = (do_exec(lpath, larg) == 0);
                    }
                }

                /* Execute right based on chain type */
                if ((chain_type == 1 && left_ok) || (chain_type == 2 && !left_ok)) {
                    /* Reconstruct as if user typed right_cmd */
                    int rl = str_len(right_cmd);
                    for (int i = 0; i <= rl; i++) line[i] = right_cmd[i];
                    len = rl;
                    /* Fall through to normal processing below */
                } else {
                    continue; /* Skip right side */
                }
            }
        }

        /* Split command + args */
        char *arg = 0;
        for (int i = 0; i < len; i++) {
            if (line[i] == ' ') { line[i] = '\0'; arg = line + i + 1; break; }
        }
        /* Strip quotes from command and args */
        strip_quotes(line);
        if (arg) {
            /* Strip quotes from each space-separated arg */
            char *p = arg;
            while (*p) {
                strip_quotes(p);
                while (*p && *p != ' ') p++;
                while (*p == ' ') p++;
            }
        }

        /* ── Detect redirection ── */
        char *redir_out = 0;    /* > filename */
        char *redir_in = 0;     /* < filename */
        int redir_append = 0;   /* >> = 1 */
        if (arg) {
            /* Scan args for >, >>, < operators */
            char *p = arg;
            char *prev_end = 0;
            while (*p) {
                if (*p == '>' && *(p+1) == '>') {
                    *p = '\0';
                    redir_append = 1;
                    redir_out = p + 2;
                    while (*redir_out == ' ') redir_out++;
                    strip_quotes(redir_out);
                    /* Trim trailing spaces from redir filename */
                    int rl = str_len(redir_out);
                    while (rl > 0 && redir_out[rl-1] == ' ') redir_out[--rl] = '\0';
                    break;
                } else if (*p == '>') {
                    *p = '\0';
                    redir_out = p + 1;
                    while (*redir_out == ' ') redir_out++;
                    strip_quotes(redir_out);
                    int rl = str_len(redir_out);
                    while (rl > 0 && redir_out[rl-1] == ' ') redir_out[--rl] = '\0';
                    break;
                } else if (*p == '<') {
                    *p = '\0';
                    redir_in = p + 1;
                    while (*redir_in == ' ') redir_in++;
                    strip_quotes(redir_in);
                    int rl = str_len(redir_in);
                    while (rl > 0 && redir_in[rl-1] == ' ') redir_in[--rl] = '\0';
                    break;
                }
                p++;
            }
            /* Trim trailing spaces from remaining args */
            int al = str_len(arg);
            while (al > 0 && arg[al-1] == ' ') arg[--al] = '\0';
            if (arg[0] == '\0') arg = 0;
        }

        /* ── Builtins ── */
        if (str_eq(line, "cd")) {
            if (!arg || !arg[0]) {
                const char *home = env_get("HOME");
                str_cpy(cwd, home ? home : "/");
            } else if (str_eq(arg, "..")) {
                int l = str_len(cwd);
                if (l > 1) { l--; while (l > 0 && cwd[l] != '/') l--; if (l == 0) l = 1; cwd[l] = '\0'; }
            } else {
                char path[256]; resolve(arg, path, sizeof(path));
                str_cpy(cwd, path);
                int l = str_len(cwd);
                if (l > 1 && cwd[l-1] == '/') cwd[l-1] = '\0';
            }
            env_set("PWD", cwd);
        } else if (str_eq(line, "exit") || str_eq(line, "logout")) {
            if (su_depth > 0) {
                /* Pop su stack — return to previous identity */
                su_depth--;
                session_uid = su_stack[su_depth].uid;
                session_gid = su_stack[su_depth].gid;
                session_token = su_stack[su_depth].token;
                str_cpy(session_user, su_stack[su_depth].user);
                env_set("USER", session_user);
                ser_puts("Returned to "); ser_puts(session_user); ser_puts("\n");
            } else {
                ser_puts("Goodbye, "); ser_puts(session_user); ser_puts("\n");
                goto login_gate;
            }
        } else if (str_eq(line, "jobs")) {
            /* List background jobs */
            int found = 0;
            for (int ji = 0; ji < MAX_BG_JOBS; ji++) {
                if (bg_jobs[ji].active) {
                    ser_puts("["); 
                    char jb[12]; int ji2 = 0; int jv = ji + 1;
                    if (jv == 0) jb[ji2++] = '0';
                    else { char jt[12]; int jti=0; while(jv){jt[jti++]='0'+jv%10;jv/=10;} while(jti--)jb[ji2++]=jt[jti]; }
                    jb[ji2] = 0; ser_puts(jb);
                    ser_puts("] PID ");
                    ji2 = 0; jv = bg_jobs[ji].pid;
                    if (jv == 0) jb[ji2++] = '0';
                    else { char jt[12]; int jti=0; while(jv){jt[jti++]='0'+jv%10;jv/=10;} while(jti--)jb[ji2++]=jt[jti]; }
                    jb[ji2] = 0; ser_puts(jb);
                    ser_puts("  "); ser_puts(bg_jobs[ji].name); ser_puts("\n");
                    found++;
                }
            }
            if (!found) ser_puts("No background jobs\n");
        } else if (str_eq(line, "kill")) {
            /* kill <pid> — terminate a process */
            if (!arg || !arg[0]) {
                ser_puts("usage: kill <pid>\n");
            } else {
                int kpid = 0;
                const char *ka = arg;
                while (*ka >= '0' && *ka <= '9') { kpid = kpid * 10 + (*ka - '0'); ka++; }
                if (kpid <= 1) {
                    ser_puts("kill: cannot kill PID 0 or 1\n");
                } else if (pipe_ep) {
                    seL4_SetMR(0, (seL4_Word)kpid);
                    seL4_MessageInfo_t reply = seL4_Call(pipe_ep,
                        seL4_MessageInfo_new(PIPE_KILL, 0, 0, 1));
                    int result = (int)(long)seL4_GetMR(0);

                    if (result == 0) {
                        ser_puts("[proc] killed PID ");
                        char pb[12]; int pi2 = 0; int pv = kpid;
                        if (pv == 0) pb[pi2++] = '0';
                        else { char pt[12]; int pti=0; while(pv){pt[pti++]='0'+pv%10;pv/=10;} while(pti--)pb[pi2++]=pt[pti]; }
                        pb[pi2] = 0; ser_puts(pb); ser_puts("\n");
                    } else {
                        ser_puts("kill: no such process\n");
                    }
                }
            }
        } else if (str_eq(line, "dmesg")) {
            /* Read and display kernel log */
            if (fs_ep) {
                char dpath[] = "/proc/log";
                int pl = 9;
                seL4_SetMR(0, (seL4_Word)pl);
                int mr = 1;
                seL4_Word w = 0;
                for (int i = 0; i < pl; i++) {
                    w |= ((seL4_Word)(uint8_t)dpath[i]) << ((i % 8) * 8);
                    if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
                }
                seL4_MessageInfo_t reply = seL4_Call(fs_ep,
                    seL4_MessageInfo_new(11 /* FS_CAT */, 0, 0, mr));
                seL4_Word total = seL4_GetMR(0);
                int rmrs = (int)seL4_MessageInfo_get_length(reply) - 1;
                int got = 0;
                for (int i = 0; i < rmrs; i++) {
                    seL4_Word rw = seL4_GetMR(i + 1);
                    for (int j = 0; j < 8 && got < (int)total; j++) {
                        ser_putc((char)((rw >> (j * 8)) & 0xFF));
                        got++;
                    }
                }
            }
        } else if (str_eq(line, "export")) {
            /* export VAR=value */
            if (arg) {
                char key[64], val[64];
                int ki = 0, vi = 0;
                const char *p = arg;
                while (*p && *p != '=' && ki < 63) key[ki++] = *p++;
                key[ki] = '\0';
                if (*p == '=') {
                    p++;
                    while (*p && vi < 63) val[vi++] = *p++;
                    val[vi] = '\0';
                    env_set(key, val);
                }
            }
        } else if (str_eq(line, "su")) {
            /* su [username] — switch user */
            char su_user[32];
            char su_pass[64];
            if (arg && arg[0]) {
                int i = 0;
                while (arg[i] && i < 31) { su_user[i] = arg[i]; i++; }
                su_user[i] = '\0';
            } else {
                /* Default: su to root */
                su_user[0] = 'r'; su_user[1] = 'o'; su_user[2] = 'o';
                su_user[3] = 't'; su_user[4] = '\0';
            }
            /* Root doesn't need a password for su */
            if (session_uid == 0) {
                su_pass[0] = '\0';
            } else {
                ser_puts("Password: ");
                read_password(su_pass, 64);
            }

            if (auth_ep) {
                /* Pack: MR0=token, then username, then password */
                int mr = 0;
                seL4_SetMR(mr++, (seL4_Word)session_token);
                /* Pack username */
                int ulen = str_len(su_user);
                seL4_SetMR(mr++, (seL4_Word)ulen);
                seL4_Word w = 0;
                for (int i = 0; i < ulen; i++) {
                    w |= ((seL4_Word)(uint8_t)su_user[i]) << ((i % 8) * 8);
                    if (i % 8 == 7 || i == ulen - 1) { seL4_SetMR(mr++, w); w = 0; }
                }
                /* Pack password */
                int plen = str_len(su_pass);
                seL4_SetMR(mr++, (seL4_Word)plen);
                w = 0;
                for (int i = 0; i < plen; i++) {
                    w |= ((seL4_Word)(uint8_t)su_pass[i]) << ((i % 8) * 8);
                    if (i % 8 == 7 || i == plen - 1) { seL4_SetMR(mr++, w); w = 0; }
                }
                /* Clear password */
                for (int i = 0; i < 64; i++) su_pass[i] = 0;

                seL4_MessageInfo_t reply = seL4_Call(auth_ep,
                    seL4_MessageInfo_new(AUTH_SU, 0, 0, mr));
                uint32_t status = (uint32_t)seL4_GetMR(0);
                if (status == 0) {
                    /* Push current identity to su stack */
                    if (su_depth < SU_STACK_MAX) {
                        su_stack[su_depth].uid = session_uid;
                        su_stack[su_depth].gid = session_gid;
                        su_stack[su_depth].token = session_token;
                        str_cpy(su_stack[su_depth].user, session_user);
                        su_depth++;
                    }
                    session_uid = (uint32_t)seL4_GetMR(1);
                    session_gid = (uint32_t)seL4_GetMR(2);
                    str_cpy(session_user, su_user);
                    env_set("USER", session_user);
                    ser_puts("Switched to "); ser_puts(session_user); ser_puts("\n");
                } else {
                    ser_puts("su: authentication failure\n");
                }
            } else {
                ser_puts("su: no auth server\n");
            }
        } else if (str_eq(line, "passwd")) {
            /* passwd [username] — change password */
            char pw_user[32];
            if (arg && arg[0]) {
                int i = 0;
                while (arg[i] && i < 31) { pw_user[i] = arg[i]; i++; }
                pw_user[i] = '\0';
            } else {
                str_cpy(pw_user, session_user);
            }
            /* Only root can change other users' passwords */
            if (session_uid != 0 && !str_eq(pw_user, session_user)) {
                ser_puts("passwd: permission denied\n");
            } else if (auth_ep) {
                char new_pass[64], confirm[64];
                ser_puts("New password: ");
                read_password(new_pass, 64);
                ser_puts("Confirm password: ");
                read_password(confirm, 64);

                if (!str_eq(new_pass, confirm)) {
                    ser_puts("passwd: passwords don't match\n");
                    for (int i = 0; i < 64; i++) { new_pass[i] = 0; confirm[i] = 0; }
                } else {
                    /* Pack: MR0=token, then username, then password */
                    int mr = 0;
                    seL4_SetMR(mr++, (seL4_Word)session_token);
                    int ulen = str_len(pw_user);
                    seL4_SetMR(mr++, (seL4_Word)ulen);
                    seL4_Word w = 0;
                    for (int i = 0; i < ulen; i++) {
                        w |= ((seL4_Word)(uint8_t)pw_user[i]) << ((i % 8) * 8);
                        if (i % 8 == 7 || i == ulen - 1) { seL4_SetMR(mr++, w); w = 0; }
                    }
                    int plen = str_len(new_pass);
                    seL4_SetMR(mr++, (seL4_Word)plen);
                    w = 0;
                    for (int i = 0; i < plen; i++) {
                        w |= ((seL4_Word)(uint8_t)new_pass[i]) << ((i % 8) * 8);
                        if (i % 8 == 7 || i == plen - 1) { seL4_SetMR(mr++, w); w = 0; }
                    }
                    for (int i = 0; i < 64; i++) { new_pass[i] = 0; confirm[i] = 0; }

                    seL4_MessageInfo_t reply = seL4_Call(auth_ep,
                        seL4_MessageInfo_new(AUTH_PASSWD, 0, 0, mr));
                    uint32_t status = (uint32_t)seL4_GetMR(0);
                    if (status == 0) {
                        ser_puts("passwd: password updated for ");
                        ser_puts(pw_user); ser_puts("\n");
                    } else {
                        ser_puts("passwd: failed\n");
                    }
                }
            } else {
                ser_puts("passwd: no auth server\n");
            }
        } else {
            /* ── Search PATH and exec ── */
            char full_path[256];
            if (find_in_path(line, full_path, sizeof(full_path))) {
                /* Build command: full_path + args */
                char cmd[512];
                int ci = 0;
                const char *p = full_path;
                while (*p && ci < 500) cmd[ci++] = *p++;
                if (arg) {
                    cmd[ci++] = ' ';
                    p = arg;
                    while (*p && ci < 500) cmd[ci++] = *p++;
                }
                /* Append CWD marker */
                cmd[ci++] = ' ';
                cmd[ci++] = 'C'; cmd[ci++] = 'W'; cmd[ci++] = 'D'; cmd[ci++] = '=';
                char uid_dec[12], gid_dec[12];
                uint_to_dec(session_uid, uid_dec);
                uint_to_dec(session_gid, gid_dec);
                for (int x = 0; uid_dec[x] && ci < 500; x++) cmd[ci++] = uid_dec[x];
                cmd[ci++] = ':';
                for (int x = 0; gid_dec[x] && ci < 500; x++) cmd[ci++] = gid_dec[x];
                cmd[ci++] = ':';
                const char *cw = cwd;
                while (*cw && ci < 500) cmd[ci++] = *cw++;
                cmd[ci] = '\0';

                if (redir_out && pipe_ep) {
                    /* Output redirection: capture stdout to pipe, write to file */
                    seL4_MessageInfo_t pr = seL4_Call(pipe_ep,
                        seL4_MessageInfo_new(PIPE_CREATE, 0, 0, 0));
                    int rpid = (int)(long)seL4_GetMR(0);
                    if (rpid >= 0) {
                        do_exec_piped(cmd, rpid, -1);
                        /* Read pipe contents and write to file via FS */
                        char fbuf[4096]; int fpos = 0;
                        while (fpos < 4095) {
                            int want = 900;
                            if (fpos + want > 4095) want = 4095 - fpos;
                            seL4_SetMR(0, (seL4_Word)rpid);
                            seL4_SetMR(1, (seL4_Word)want);
                            seL4_MessageInfo_t rr = seL4_Call(pipe_ep,
                                seL4_MessageInfo_new(62, 0, 0, 2));
                            int got = (int)(long)seL4_GetMR(0);
                            if (got <= 0) break;
                            int mr = 1;
                            for (int i = 0; i < got; i++) {
                                if (i % 8 == 0 && i > 0) mr++;
                                fbuf[fpos++] = (char)((seL4_GetMR(mr) >> ((i % 8) * 8)) & 0xFF);
                            }
                        }
                        /* Resolve path */
                        char rpath[256];
                        if (redir_out[0] != '/') {
                            int ri = 0;
                            const char *cw2 = cwd;
                            while (*cw2 && ri < 250) rpath[ri++] = *cw2++;
                            if (ri > 1) rpath[ri++] = '/';
                            while (*redir_out && ri < 255) rpath[ri++] = *redir_out++;
                            rpath[ri] = '\0';
                        } else {
                            int ri = 0;
                            while (*redir_out && ri < 255) rpath[ri++] = *redir_out++;
                            rpath[ri] = '\0';
                        }
                        /* Write to file via FS_WRITE_FILE IPC */
                        if (fs_ep && fpos > 0) {
                            int pl = str_len(rpath);
                            seL4_SetMR(0, (seL4_Word)pl);
                            int mr = 1;
                            seL4_Word w = 0;
                            for (int i = 0; i < pl; i++) {
                                w |= ((seL4_Word)(uint8_t)rpath[i]) << ((i % 8) * 8);
                                if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
                            }
                            seL4_SetMR(mr++, (seL4_Word)fpos);
                            w = 0;
                            for (int i = 0; i < fpos; i++) {
                                w |= ((seL4_Word)(uint8_t)fbuf[i]) << ((i % 8) * 8);
                                if (i % 8 == 7 || i == fpos - 1) { seL4_SetMR(mr++, w); w = 0; }
                            }
                            seL4_Call(fs_ep, seL4_MessageInfo_new(15, 0, 0, mr));
                        }
                        seL4_SetMR(0, (seL4_Word)rpid);
                        seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_CLOSE, 0, 0, 1));
                    }
                } else if (redir_in && pipe_ep) {
                    /* Input redirection: read file, feed to stdin pipe */
                    /* Resolve path */
                    char rpath[256];
                    if (redir_in[0] != '/') {
                        int ri = 0;
                        const char *cw2 = cwd;
                        while (*cw2 && ri < 250) rpath[ri++] = *cw2++;
                        if (ri > 1) rpath[ri++] = '/';
                        const char *rr = redir_in;
                        while (*rr && ri < 255) rpath[ri++] = *rr++;
                        rpath[ri] = '\0';
                    } else {
                        int ri = 0;
                        while (*redir_in && ri < 255) rpath[ri++] = *redir_in++;
                        rpath[ri] = '\0';
                    }
                    /* Read file via FS_CAT */
                    char fbuf[4096]; int flen = 0;
                    {
                        int pl = str_len(rpath);
                        seL4_SetMR(0, (seL4_Word)pl);
                        int mr = 1;
                        seL4_Word w = 0;
                        for (int i = 0; i < pl; i++) {
                            w |= ((seL4_Word)(uint8_t)rpath[i]) << ((i % 8) * 8);
                            if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
                        }
                        seL4_MessageInfo_t rr = seL4_Call(fs_ep,
                            seL4_MessageInfo_new(11, 0, 0, mr));
                        seL4_Word total = seL4_GetMR(0);
                        int rmrs = (int)seL4_MessageInfo_get_length(rr) - 1;
                        for (int i = 0; i < rmrs && flen < 4095; i++) {
                            seL4_Word rw = seL4_GetMR(i + 1);
                            for (int j = 0; j < 8 && flen < (int)total && flen < 4095; j++)
                                fbuf[flen++] = (char)((rw >> (j * 8)) & 0xFF);
                        }
                    }
                    if (flen > 0) {
                        /* Create pipe, fill it, exec with stdin←pipe */
                        seL4_MessageInfo_t pr = seL4_Call(pipe_ep,
                            seL4_MessageInfo_new(PIPE_CREATE, 0, 0, 0));
                        int rpid = (int)(long)seL4_GetMR(0);
                        if (rpid >= 0) {
                            /* Write file contents to pipe */
                            int sent = 0;
                            while (sent < flen) {
                                int chunk = flen - sent;
                                if (chunk > 900) chunk = 900;
                                seL4_SetMR(0, (seL4_Word)rpid);
                                seL4_SetMR(1, (seL4_Word)chunk);
                                int mr = 2;
                                seL4_Word w = 0;
                                for (int i = 0; i < chunk; i++) {
                                    w |= ((seL4_Word)(uint8_t)fbuf[sent+i]) << ((i % 8) * 8);
                                    if (i % 8 == 7 || i == chunk - 1) { seL4_SetMR(mr++, w); w = 0; }
                                }
                                seL4_Call(pipe_ep, seL4_MessageInfo_new(61, 0, 0, mr));
                                sent += chunk;
                            }
                            do_exec_piped(cmd, -1, rpid);
                            seL4_SetMR(0, (seL4_Word)rpid);
                            seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_CLOSE, 0, 0, 1));
                        }
                    } else {
                        ser_puts(rpath); ser_puts(": No such file\n");
                    }
                } else if (run_bg) {
                    int bgpid = do_exec_bg(full_path, arg);
                    if (bgpid > 0) {
                        /* Track background job */
                        for (int ji = 0; ji < MAX_BG_JOBS; ji++) {
                            if (!bg_jobs[ji].active) {
                                bg_jobs[ji].active = 1;
                                bg_jobs[ji].pid = bgpid;
                                int ni = 0;
                                const char *np = line;
                                while (*np && ni < 63) bg_jobs[ji].name[ni++] = *np++;
                                bg_jobs[ji].name[ni] = '\0';
                                ser_puts("[");
                                char jb[12]; int ji2=0; int jv=ji+1;
                                if(jv==0)jb[ji2++]='0'; else{char jt[12];int jti=0;while(jv){jt[jti++]='0'+jv%10;jv/=10;}while(jti--)jb[ji2++]=jt[jti];}
                                jb[ji2]=0; ser_puts(jb);
                                ser_puts("] PID ");
                                ji2=0; jv=bgpid;
                                if(jv==0)jb[ji2++]='0'; else{char jt[12];int jti=0;while(jv){jt[jti++]='0'+jv%10;jv/=10;}while(jti--)jb[ji2++]=jt[jti];}
                                jb[ji2]=0; ser_puts(jb); ser_puts("\n");
                                break;
                            }
                        }
                    }
                } else {
                    do_exec(full_path, arg);
                }
            } else {
                ser_puts(line);
                ser_puts(": command not found\n");
            }
        }
    }
    return 0;
}
