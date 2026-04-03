/*
 * AIOS 0.4.x — Interactive Shell with filesystem
 * argv[0] = serial ep, argv[1] = fs ep
 */
#include <stdint.h>
#include <sel4/sel4.h>

#define SER_PUTC 1
#define SER_GETC 2
#define FS_LS   10
#define FS_CAT  11

static seL4_CPtr serial_ep;
static seL4_CPtr fs_ep;
static seL4_CPtr exec_ep;

#define EXEC_RUN 20

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
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}
static int str_prefix(const char *s, const char *p) {
    while (*p && *s == *p) { s++; p++; }
    return *p == '\0';
}
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void str_cpy(char *d, const char *s) { while ((*d++ = *s++)); }

/* Current working directory */
static char cwd[256] = "/";

/* Build absolute path from cwd + relative */
static void resolve(const char *arg, char *out, int outsz) {
    if (!arg || !arg[0]) {
        str_cpy(out, cwd);
        return;
    }
    if (arg[0] == '/') {
        /* Absolute */
        int i = 0;
        while (arg[i] && i < outsz - 1) { out[i] = arg[i]; i++; }
        out[i] = '\0';
        return;
    }
    /* Relative: cwd + "/" + arg */
    int ci = 0;
    while (cwd[ci] && ci < outsz - 2) { out[ci] = cwd[ci]; ci++; }
    if (ci > 1) out[ci++] = '/'; /* add / unless cwd is just "/" */
    int ai = 0;
    while (arg[ai] && ci < outsz - 1) { out[ci++] = arg[ai++]; }
    out[ci] = '\0';
}

/* Pack a path string into seL4 message registers for FS IPC */
static int pack_path(const char *path) {
    int pl = str_len(path);
    seL4_SetMR(0, (seL4_Word)pl);
    int mr_idx = 1;
    seL4_Word w = 0;
    for (int i = 0; i < pl; i++) {
        w |= ((seL4_Word)(uint8_t)path[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == pl - 1) {
            seL4_SetMR(mr_idx++, w);
            w = 0;
        }
    }
    return mr_idx;
}

/* Unpack reply data from MRs as chars */
static void unpack_reply(seL4_MessageInfo_t reply) {
    seL4_Word total = seL4_GetMR(0);
    if (total == 0) return;
    int mrs = (int)seL4_MessageInfo_get_length(reply) - 1;
    for (int i = 0; i < mrs; i++) {
        seL4_Word rw = seL4_GetMR(i + 1);
        for (int j = 0; j < 8 && (int)(i * 8 + j) < (int)total; j++) {
            ser_putc((char)((rw >> (j * 8)) & 0xFF));
        }
    }
}

/* Read file into buffer via FS IPC */
static int fs_read(const char *fpath, char *buf, int bufsz) {
    if (!fs_ep) return -1;
    char abs[256];
    resolve(fpath, abs, sizeof(abs));
    int mrs = pack_path(abs);
    seL4_MessageInfo_t reply = seL4_Call(fs_ep, seL4_MessageInfo_new(FS_CAT, 0, 0, mrs));
    seL4_Word total = seL4_GetMR(0);
    if (total == 0) return 0;
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

static void put_dec(unsigned long v) {
    char tmp[20]; int ti = 0;
    if (v == 0) { ser_putc('0'); return; }
    while (v) { tmp[ti++] = '0' + v % 10; v /= 10; }
    while (ti--) ser_putc(tmp[ti]);
}

static void cmd_wc(const char *arg) {
    if (!arg) { ser_puts("Usage: wc <file>\n"); return; }
    char buf[4096];
    int n = fs_read(arg, buf, sizeof(buf));
    if (n <= 0) { ser_puts("wc: "); ser_puts(arg); ser_puts(": No such file\n"); return; }
    int lines = 0, words = 0, in_word = 0;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') lines++;
        if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t') in_word = 0;
        else if (!in_word) { in_word = 1; words++; }
    }
    put_dec(lines); ser_putc(' ');
    put_dec(words); ser_putc(' ');
    put_dec(n); ser_putc(' ');
    ser_puts(arg); ser_putc('\n');
}

static void cmd_head(const char *arg) {
    if (!arg) { ser_puts("Usage: head <file>\n"); return; }
    char buf[4096];
    int n = fs_read(arg, buf, sizeof(buf));
    if (n <= 0) { ser_puts("head: "); ser_puts(arg); ser_puts(": No such file\n"); return; }
    int lines = 0;
    for (int i = 0; i < n && lines < 10; i++) {
        ser_putc(buf[i]);
        if (buf[i] == '\n') lines++;
    }
}

static void cmd_ls(const char *arg) {
    if (!fs_ep) { ser_puts("No filesystem\n"); return; }
    char path[256];
    resolve(arg, path, sizeof(path));
    int mrs = pack_path(path);
    seL4_MessageInfo_t reply = seL4_Call(fs_ep, seL4_MessageInfo_new(FS_LS, 0, 0, mrs));
    seL4_Word total = seL4_GetMR(0);
    if (total == 0) {
        ser_puts("ls: cannot access '");
        ser_puts(path);
        ser_puts("'\n");
        return;
    }
    unpack_reply(reply);
}

static void cmd_cat(const char *arg) {
    if (!fs_ep) { ser_puts("No filesystem\n"); return; }
    char path[256];
    resolve(arg, path, sizeof(path));
    int mrs = pack_path(path);
    seL4_MessageInfo_t reply = seL4_Call(fs_ep, seL4_MessageInfo_new(FS_CAT, 0, 0, mrs));
    seL4_Word total = seL4_GetMR(0);
    if (total == 0) {
        ser_puts("cat: ");
        ser_puts(path);
        ser_puts(": No such file\n");
        return;
    }
    unpack_reply(reply);
}

static void cmd_cd(const char *arg) {
    if (!arg || !arg[0]) {
        str_cpy(cwd, "/");
        return;
    }
    /* Handle ".." specially */
    if (str_eq(arg, "..")) {
        int len = str_len(cwd);
        if (len <= 1) return; /* already at / */
        len--; /* skip trailing char */
        while (len > 0 && cwd[len] != '/') len--;
        if (len == 0) len = 1; /* keep root / */
        cwd[len] = '\0';
        return;
    }
    char path[256];
    resolve(arg, path, sizeof(path));

    /* Verify it's a directory by trying ls */
    if (fs_ep) {
        int mrs = pack_path(path);
        seL4_MessageInfo_t reply = seL4_Call(fs_ep, seL4_MessageInfo_new(FS_LS, 0, 0, mrs));
        if (seL4_GetMR(0) == 0) {
            ser_puts("cd: ");
            ser_puts(path);
            ser_puts(": Not a directory\n");
            return;
        }
    }
    str_cpy(cwd, path);
    /* Normalize trailing slash */
    int len = str_len(cwd);
    if (len > 1 && cwd[len - 1] == '/') cwd[len - 1] = '\0';
}

#define LINE_MAX 128
static char line[LINE_MAX];

static void read_line(int *len) {
    *len = 0;
    while (*len < LINE_MAX - 1) {
        int c = ser_getc();
        if (c < 0) continue;
        if (c == '\r' || c == '\n') { ser_putc('\n'); break; }
        if ((c == 0x7f || c == '\b') && *len > 0) {
            (*len)--;
            ser_putc('\b'); ser_putc(' '); ser_putc('\b');
            continue;
        }
        if (c >= 0x20 && c < 127) {
            line[*len] = (char)c;
            (*len)++;
            ser_putc((char)c);
        }
    }
    line[*len] = '\0';
}

int main(int argc, char *argv[]) {
    serial_ep = 0; fs_ep = 0;
    if (argc > 0) serial_ep = (seL4_CPtr)parse_num(argv[0]);
    if (argc > 1) fs_ep = (seL4_CPtr)parse_num(argv[1]);
    if (argc > 2) exec_ep = (seL4_CPtr)parse_num(argv[2]);

    ser_puts("\n");
    ser_puts("============================================\n");
    ser_puts("  AIOS 0.4.x Shell\n");
    if (fs_ep) ser_puts("  Filesystem: ext2\n");
    ser_puts("============================================\n\n");


    while (1) {
        ser_puts(cwd);
        ser_puts(" $ ");
        int len;
        read_line(&len);
        if (len == 0) continue;

        /* Parse command + arg */
        char *arg = 0;
        for (int i = 0; i < len; i++) {
            if (line[i] == ' ') {
                line[i] = '\0';
                arg = line + i + 1;
                break;
            }
        }

        if (str_eq(line, "help")) {
            ser_puts("Commands: help, ls, cat, cd, pwd, wc, head, echo, uname, exit\n");
            
        } else if (str_eq(line, "ls")) {
            cmd_ls(arg);
        } else if (str_eq(line, "cat")) {
            if (!arg) ser_puts("Usage: cat <file>\n");
            else cmd_cat(arg);
        } else if (str_eq(line, "cd")) {
            cmd_cd(arg);
        } else if (str_eq(line, "pwd")) {
            ser_puts(cwd);
            ser_putc('\n');
        } else if (str_eq(line, "wc")) {
            cmd_wc(arg);
        } else if (str_eq(line, "head")) {
            cmd_head(arg);
        } else if (str_eq(line, "hello")) {
            ser_puts("Hello from AIOS 0.4.x!\n");
        } else if (str_eq(line, "uname")) {
            ser_puts("AIOS 0.4.x aarch64 seL4/bare 4-core SMP\n");
        } else if (str_eq(line, "echo")) {
            if (arg) { ser_puts(arg); ser_putc('\n'); }
            else ser_putc('\n');
        } else if (str_eq(line, "exit")) {
            ser_puts("Goodbye.\n");
            return 0;
        } else if (exec_ep) {
            int pl = str_len(line);
            seL4_SetMR(0, (seL4_Word)pl);
            int mr = 1;
            seL4_Word w = 0;
            for (int i = 0; i < pl; i++) {
                w |= ((seL4_Word)(uint8_t)line[i]) << ((i % 8) * 8);
                if (i % 8 == 7 || i == pl - 1) { seL4_SetMR(mr++, w); w = 0; }
            }
            seL4_MessageInfo_t reply = seL4_Call(exec_ep,
                seL4_MessageInfo_new(EXEC_RUN, 0, 0, mr));
            int ret = (int)(long)seL4_GetMR(0);
            if (ret != 0) {
                ser_puts(line);
                ser_puts(": command not found\n");
            }
        } else {
            ser_puts(line);
            ser_puts(": command not found\n");
        }
    }
    return 0;
}
