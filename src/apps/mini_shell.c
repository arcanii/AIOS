/*
 * AIOS 0.4.x — Interactive Mini Shell
 *
 * Priority: 100 (below root=150, below serial=200)
 * GETC returns -1 if no char. Shell retries.
 * Between each seL4_Call round-trip, higher-priority root
 * gets to run and poll UART.
 */
#include <sel4/sel4.h>

#define SER_PUTC 1
#define SER_GETC 2

static seL4_CPtr serial_ep;

static void ser_putc(char c) {
    seL4_SetMR(0, (seL4_Word)c);
    seL4_Call(serial_ep, seL4_MessageInfo_new(SER_PUTC, 0, 0, 1));
}

static void ser_puts(const char *s) {
    while (*s) ser_putc(*s++);
}

static int ser_getc(void) {
    seL4_MessageInfo_t reply = seL4_Call(
        serial_ep, seL4_MessageInfo_new(SER_GETC, 0, 0, 0));
    return (int)(long)seL4_GetMR(0);
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int str_prefix(const char *s, const char *prefix) {
    while (*prefix && *s == *prefix) { s++; prefix++; }
    return *prefix == '\0';
}

#define LINE_MAX 128
static char line[LINE_MAX];

static void read_line(int *len) {
    *len = 0;
    while (*len < LINE_MAX - 1) {
        int c = ser_getc();
        if (c < 0) continue;  /* retry — root polls UART between our IPC calls */
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
    serial_ep = 8;
    if (argc > 0 && argv[0]) {
        serial_ep = 0;
        for (const char *p = argv[0]; *p >= '0' && *p <= '9'; p++)
            serial_ep = serial_ep * 10 + (*p - '0');
    }

    ser_puts("\n");
    ser_puts("============================================\n");
    ser_puts("  AIOS 0.4.x Interactive Shell\n");
    ser_puts("  I/O via IPC to serial_server\n");
    ser_puts("============================================\n\n");

    while (1) {
        ser_puts("$ ");
        int len;
        read_line(&len);
        if (len == 0) continue;

        if (str_eq(line, "help")) {
            ser_puts("Commands: help, echo, hello, uname, exit\n");
        } else if (str_eq(line, "hello")) {
            ser_puts("Hello from AIOS 0.4.x!\n");
        } else if (str_eq(line, "uname")) {
            ser_puts("AIOS 0.4.x aarch64 seL4/bare\n");
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
