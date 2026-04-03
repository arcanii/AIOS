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

#define SER_PUTC 1
#define SER_GETC 2
#define FS_STAT  12
#define EXEC_RUN 20

static seL4_CPtr serial_ep, fs_ep, exec_ep;

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

/* ── Exec a command via IPC ── */
static int do_exec(const char *cmdline) {
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

/* ── Line reader ── */
#define LINE_MAX 256
static char line[LINE_MAX];
static void read_line(int *len) {
    *len = 0;
    while (*len < LINE_MAX - 1) {
        int c = ser_getc();
        if (c < 0) continue;
        if (c == '\r' || c == '\n') { ser_putc('\n'); break; }
        if ((c == 0x7f || c == '\b') && *len > 0) {
            (*len)--; ser_putc('\b'); ser_putc(' '); ser_putc('\b'); continue;
        }
        if (c >= 0x20 && c < 127) { line[*len] = (char)c; (*len)++; ser_putc((char)c); }
    }
    line[*len] = '\0';
}

/* ── Main ── */
int main(int argc, char *argv[]) {
    serial_ep = 0; fs_ep = 0; exec_ep = 0;
    if (argc > 0) serial_ep = (seL4_CPtr)parse_num(argv[0]);
    if (argc > 1) fs_ep = (seL4_CPtr)parse_num(argv[1]);
    if (argc > 2) exec_ep = (seL4_CPtr)parse_num(argv[2]);

    /* Default environment */
    env_set("PATH", "/bin");
    env_set("HOME", "/");
    env_set("USER", "root");
    env_set("SHELL", "/bin/mini_shell");
    env_set("TERM", "vt100");

    ser_puts("\n============================================\n");
    ser_puts("  AIOS 0.4.x miniShell\n");
    ser_puts("============================================\n\n");

    while (1) {
        ser_puts(cwd); ser_puts(" $ ");
        int len; read_line(&len);
        if (len == 0) continue;

        /* Split command + args */
        char *arg = 0;
        for (int i = 0; i < len; i++) {
            if (line[i] == ' ') { line[i] = '\0'; arg = line + i + 1; break; }
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
        } else if (str_eq(line, "exit")) {
            ser_puts("Goodbye.\n");
            return 0;
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
                const char *cw = cwd;
                while (*cw && ci < 500) cmd[ci++] = *cw++;
                cmd[ci] = '\0';
                do_exec(cmd);
            } else {
                ser_puts(line);
                ser_puts(": command not found\n");
            }
        }
    }
    return 0;
}
