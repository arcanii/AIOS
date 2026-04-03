/*
 * AIOS 0.4.x miniShell — thin exec launcher
 *
 * Builtins: cd, exit
 * Everything else → exec from /bin/ via PATH
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

/* Send command to exec_thread */
static int do_exec(const char *cmdline) {
    if (!exec_ep) return -1;
    int pl = str_len(cmdline);
    seL4_SetMR(0, (seL4_Word)pl);
    int mr = 1; seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)cmdline[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
    }
    seL4_MessageInfo_t reply = seL4_Call(exec_ep,
        seL4_MessageInfo_new(EXEC_RUN, 0, 0, mr));
    return (int)(long)seL4_GetMR(0);
}

/* Build exec command: "name arg1 arg2" with args resolved */
static int exec_cmd(const char *name, const char *args, int raw_args) {
    char buf[512];
    int bi = 0;
    const char *p = name;
    while (*p && bi < 500) buf[bi++] = *p++;
    if (args) {
        const char *a = args;
        while (*a && bi < 500) {
            buf[bi++] = ' ';
            const char *start = a;
            while (*a && *a != ' ') a++;
            int alen = (int)(a - start);
            if (!raw_args && start[0] != '/' && start[0] != '-') {
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

/* Commands where first arg is NOT a path (e.g., pattern for grep) */
static int is_raw_cmd(const char *cmd) {
    return str_eq(cmd, "echo") || str_eq(cmd, "grep") ||
           str_eq(cmd, "yes");
}

/* Line reader */
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

        /* Split command + args */
        char *arg = 0;
        for (int i = 0; i < len; i++) {
            if (line[i] == ' ') { line[i] = '\0'; arg = line + i + 1; break; }
        }

        /* Builtins: cd, exit */
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
            /* Everything else → exec from /bin/ */
            int raw = is_raw_cmd(line);
            /* If no args, pass CWD for commands that need it */
            char *effective_arg = arg;
            char cwd_arg[256];
            if (!arg && (str_eq(line, "ls") || str_eq(line, "pwd"))) {
                str_cpy(cwd_arg, cwd);
                effective_arg = cwd_arg;
            }
            int ret = exec_cmd(line, effective_arg, raw);
            if (ret != 0) {
                ser_puts(line);
                ser_puts(": command not found\n");
            }
        }
    }
    return 0;
}
