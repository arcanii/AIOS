#include "aios.h"

#define LINE_MAX 256
static char line[LINE_MAX];
static int  line_len;

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int starts_with(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

static void print(const char *s) { aios_puts_direct(s); }
static void printc(char c) { aios_putc_direct(c); }

static void print_dec(unsigned int n) {
    char buf[12]; int i = 0;
    if (n == 0) { printc('0'); return; }
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i--) printc(buf[i]);
}

static void read_line(void) {
    line_len = 0;
    while (1) {
        int c = aios_getc();
        if (c < 0) continue;
        if (c == '\r' || c == '\n') {
            printc('\n');
            line[line_len] = '\0';
            return;
        }
        if ((c == 0x7f || c == '\b') && line_len > 0) {
            line_len--;
            print("\b \b");
            continue;
        }
        if (c >= 0x20 && line_len < LINE_MAX - 1) {
            line[line_len++] = (char)c;
            printc((char)c);
        }
    }
}

static void cmd_ls(void) {
    char buf[16 * 64];
    int count = aios_readdir(buf, 64);
    if (count < 0) { print("Error\n"); return; }
    for (int i = 0; i < count; i++) {
        char *ent = buf + i * 16;
        char name[13]; int p = 0;
        for (int j = 0; j < 8; j++)
            if (ent[j] != ' ') name[p++] = ent[j];
        if (ent[8] != ' ') {
            name[p++] = '.';
            for (int j = 8; j < 11; j++)
                if (ent[j] != ' ') name[p++] = ent[j];
        }
        name[p] = '\0';
        unsigned int sz = (unsigned char)ent[12] | ((unsigned char)ent[13]<<8)
                        | ((unsigned char)ent[14]<<16) | ((unsigned char)ent[15]<<24);
        print("  "); print(name);
        for (int j = p; j < 14; j++) printc(' ');
        print_dec(sz); print("\n");
    }
}

static void cmd_cat(const char *fname) {
    int fd = aios_open(fname);
    if (fd < 0) { print("File not found: "); print(fname); print("\n"); return; }
    int size = aios_filesize();
    char buf[512];
    int off = 0;
    while (off < size) {
        int n = size - off; if (n > 512) n = 512;
        int got = aios_read(fd, buf, n);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) printc(buf[i]);
        off += got;
    }
    print("\n");
    aios_close(fd);
}

static void cmd_help(void) {
    print("Commands:\n");
    print("  help       - this message\n");
    print("  ls         - list files\n");
    print("  cat <file> - display file contents\n");
    print("  info       - system information\n");
    print("  exit       - return to orchestrator\n");
}

static void cmd_info(void) {
    print("AIOS Shell v0.1\n");
    print("  Kernel:  seL4 14.0.0 (Microkit 2.1.0)\n");
    print("  Arch:    AArch64 (Cortex-A53)\n");
    print("  Shell:   sandbox PD (PPC syscalls)\n");
    print("  I/O:     blocking getc via PPC\n");
}

AIOS_ENTRY {
    print("AIOS Shell v0.1\n");
    print("Type 'help' for commands.\n\n");

    while (1) {
        print("$ ");
        read_line();
        if (line_len == 0) continue;

        if (str_eq(line, "help")) cmd_help();
        else if (str_eq(line, "ls") || str_eq(line, "dir")) cmd_ls();
        else if (starts_with(line, "cat ")) {
            char *fn = line + 4;
            while (*fn == ' ') fn++;
            if (*fn) cmd_cat(fn); else print("Usage: cat <file>\n");
        }
        else if (str_eq(line, "info")) cmd_info();
        else if (str_eq(line, "exit") || str_eq(line, "quit")) {
            print("Exiting shell.\n");
            return 0;
        }
        else { print("Unknown: "); print(line); print("\n"); }
    }
}
