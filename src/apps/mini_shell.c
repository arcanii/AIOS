/*
 * AIOS 0.4.x miniShell — thin exec launcher
 *
 * Builtins: cd, exit
 * Everything else → exec (alias or direct CPIO name)
 *
 * argv[0] = serial_ep, argv[1] = fs_ep, argv[2] = exec_ep
 */
#include <stdint.h>
#include <sel4/sel4.h>

#define SER_PUTC 1
#define SER_GETC 2
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

/* ── Alias table ── */
struct alias { const char *cmd; const char *prog; int raw; };
static const struct alias aliases[] = {
    { "cat",      "gnu_cat",        0 },
    { "ls",       "posix_ls",       0 },
    { "wc",       "posix_wc",       0 },
    { "head",     "posix_head",     0 },
    { "uname",    "posix_uname",    0 },
    { "echo",     "posix_echo",     1 },
    { "ps",       "posix_ps",       0 },
    { "grep",     "posix_grep",     1 },
    { "sort",     "posix_sort",     0 },
    { "id",       "posix_id",       0 },
    { "whoami",   "posix_whoami",   0 },
    { "date",     "posix_date",     0 },
    { "env",      "posix_env",      0 },
    { "yes",      "posix_yes",      1 },
    { "basename", "posix_basename", 0 },
    { "dirname",  "posix_dirname",  0 },
    { "true",     "posix_true",     0 },
    { "false",    "posix_false",    0 },
    { "pwd",      "posix_pwd",      0 },
    { "mkdir",    "posix_mkdir",    0 },
    { "touch",    "posix_touch",    0 },
    { "rm",       "posix_rm",       0 },
    { "help",     "posix_help",     0 },
    { 0, 0, 0 }
};

static const struct alias *lookup_alias(const char *cmd) {
    for (int i = 0; aliases[i].cmd; i++)
        if (str_eq(cmd, aliases[i].cmd)) return &aliases[i];
    return 0;
}

/* ── Exec ── */
static int do_exec(const char *cmdline) {
    if (!exec_ep) return -1;
    int pl = str_len(cmdline);
    seL4_SetMR(0, (seL4_Word)pl);
    int mr = 1; seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)cmdline[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(exec_ep, seL4_MessageInfo_new(EXEC_RUN, 0, 0, mr));
    return (int)(long)seL4_GetMR(0);
}

static int exec_cmd(const char *prog, const char *args, int raw) {
    char buf[512];
    int bi = 0;
    const char *p = prog;
    while (*p && bi < 500) buf[bi++] = *p++;
    if (args) {
        const char *a = args;
        while (*a && bi < 500) {
            buf[bi++] = ' ';
            const char *start = a;
            while (*a && *a != ' ') a++;
            int alen = (int)(a - start);
            if (!raw && start[0] != '/' && start[0] != '-') {
                char word[128], abs[256];
                int wi = 0;
                while (wi < alen && wi < 127) { word[wi]=start[wi]; wi++; }
                word[wi] = '\0';
                resolve(word, abs, sizeof(abs));
                const char *q = abs;
                while (*q && bi < 500) buf[bi++] = *q++;
            } else {
                for (int i = 0; i < alen && bi < 500; i++) buf[bi++] = start[i];
            }
            while (*a == ' ') a++;
        }
    }
    buf[bi] = '\0';
    return do_exec(buf);
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

    ser_puts("\n============================================\n");
    ser_puts("  AIOS 0.4.x miniShell\n");
    ser_puts("============================================\n\n");

    while (1) {
        ser_puts(cwd); ser_puts(" $ ");
        int len; read_line(&len);
        if (len == 0) continue;

        char *arg = 0;
        for (int i = 0; i < len; i++) {
            if (line[i] == ' ') { line[i] = '\0'; arg = line + i + 1; break; }
        }

        /* Only true builtins: cd, exit */
        if (str_eq(line, "cd")) {
            if (!arg || !arg[0]) { str_cpy(cwd, "/"); }
            else if (str_eq(arg, "..")) {
                int l = str_len(cwd);
                if (l > 1) { l--; while (l > 0 && cwd[l] != '/') l--; if (l == 0) l = 1; cwd[l] = '\0'; }
            } else {
                char path[256]; resolve(arg, path, sizeof(path));
                str_cpy(cwd, path);
                int l = str_len(cwd);
                if (l > 1 && cwd[l-1] == '/') cwd[l-1] = '\0';
            }
        } else if (str_eq(line, "exit")) {
            ser_puts("Goodbye.\n");
            return 0;
        } else {
            /* Alias or direct exec */
            const struct alias *al = lookup_alias(line);
            if (al) {
                if (exec_cmd(al->prog, arg, al->raw) != 0) {
                    ser_puts(line); ser_puts(": command not found\n");
                }
            } else {
                if (arg) *(arg - 1) = ' ';
                if (do_exec(line) != 0) {
                    ser_puts(line); ser_puts(": command not found\n");
                }
            }
        }
    }
    return 0;
}
