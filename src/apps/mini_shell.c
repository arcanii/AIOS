/*
 * AIOS 0.4.x — Interactive Mini Shell with filesystem
 * argv[0] = serial endpoint, argv[1] = fs endpoint
 */
#include <stdint.h>
#include <sel4/sel4.h>

#define SER_PUTC 1
#define SER_GETC 2
#define FS_LS   10
#define FS_CAT  11

static seL4_CPtr serial_ep;
static seL4_CPtr fs_ep;

static void ser_putc(char c) {
    seL4_SetMR(0, (seL4_Word)c);
    seL4_Call(serial_ep, seL4_MessageInfo_new(SER_PUTC, 0, 0, 1));
}
static void ser_puts(const char *s) { while (*s) ser_putc(*s++); }

static int ser_getc(void) {
    seL4_MessageInfo_t r = seL4_Call(serial_ep, seL4_MessageInfo_new(SER_GETC, 0, 0, 0));
    return (int)(long)seL4_GetMR(0);
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}
static int str_prefix(const char *s, const char *p) {
    while (*p && *s == *p) { s++; p++; }
    return *p == '\0';
}

static long parse_num(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
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

static void cmd_ls(const char *path) {
    if (!fs_ep) { ser_puts("No filesystem\n"); return; }

    /* Send LS with inode 0 (= root) or resolve path */
    uint32_t ino = 0; /* root */
    seL4_SetMR(0, (seL4_Word)ino);
    seL4_MessageInfo_t reply = seL4_Call(fs_ep, seL4_MessageInfo_new(FS_LS, 0, 0, 1));

    seL4_Word total_len = seL4_GetMR(0);
    if (total_len == 0) { ser_puts("(empty)\n"); return; }

    /* Unpack chars from MRs */
    int mrs = (int)seL4_MessageInfo_get_length(reply) - 1;
    for (int i = 0; i < mrs; i++) {
        seL4_Word w = seL4_GetMR(i + 1);
        for (int j = 0; j < 8 && (int)(i*8+j) < (int)total_len; j++) {
            ser_putc((char)((w >> (j*8)) & 0xFF));
        }
    }
}

static void cmd_cat(const char *path) {
    if (!fs_ep) { ser_puts("No filesystem\n"); return; }

    /* Pack path into MRs */
    int pl = 0;
    while (path[pl]) pl++;

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

    seL4_MessageInfo_t reply = seL4_Call(fs_ep,
        seL4_MessageInfo_new(FS_CAT, 0, 0, mr_idx));

    seL4_Word total_len = seL4_GetMR(0);
    if (total_len == 0) { ser_puts("File not found\n"); return; }

    int mrs = (int)seL4_MessageInfo_get_length(reply) - 1;
    for (int i = 0; i < mrs; i++) {
        seL4_Word rw = seL4_GetMR(i + 1);
        for (int j = 0; j < 8 && (int)(i*8+j) < (int)total_len; j++) {
            ser_putc((char)((rw >> (j*8)) & 0xFF));
        }
    }
}

int main(int argc, char *argv[]) {
    serial_ep = 0; fs_ep = 0;
    if (argc > 0) serial_ep = (seL4_CPtr)parse_num(argv[0]);
    if (argc > 1) fs_ep = (seL4_CPtr)parse_num(argv[1]);

    ser_puts("\n");
    ser_puts("============================================\n");
    ser_puts("  AIOS 0.4.x Interactive Shell\n");
    if (fs_ep) ser_puts("  Filesystem: ext2 (mounted)\n");
    ser_puts("============================================\n\n");

    while (1) {
        ser_puts("$ ");
        int len;
        read_line(&len);
        if (len == 0) continue;

        if (str_eq(line, "help")) {
            ser_puts("Commands: help, hello, uname, ls, cat <file>, exit\n");
        } else if (str_eq(line, "hello")) {
            ser_puts("Hello from AIOS 0.4.x!\n");
        } else if (str_eq(line, "uname")) {
            ser_puts("AIOS 0.4.x aarch64 seL4/bare\n");
        } else if (str_eq(line, "ls")) {
            cmd_ls("/");
        } else if (str_prefix(line, "cat ")) {
            cmd_cat(line + 4);
        } else if (str_eq(line, "exit")) {
            ser_puts("Goodbye.\n");
            return 0;
        } else if (str_prefix(line, "echo ")) {
            ser_puts(line + 5);
            ser_putc('\n');
        } else {
            ser_puts(line);
            ser_puts(": command not found\n");
        }
    }
    return 0;
}
