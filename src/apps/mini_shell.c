/*
 * AIOS 0.4.x — Interactive Mini Shell
 * All I/O via IPC to serial_server
 */
#include <stdio.h>
#include <sel4/sel4.h>

#define SER_PUTC 1
#define SER_GETC 2
#define SER_PUTS 3

static seL4_CPtr serial_ep;

static void ser_putc(char c) {
    seL4_SetMR(0, (seL4_Word)c);
    seL4_Call(serial_ep, seL4_MessageInfo_new(SER_PUTC, 0, 0, 1));
}

static void ser_puts(const char *s) {
    while (*s) {
        int n = 0;
        while (s[n] && n < 4) {
            seL4_SetMR(n, (seL4_Word)s[n]);
            n++;
        }
        seL4_Call(serial_ep, seL4_MessageInfo_new(SER_PUTS, 0, 0, n));
        s += n;
    }
}

static int ser_getc(void) {
    seL4_MessageInfo_t reply = seL4_Call(
        serial_ep, seL4_MessageInfo_new(SER_GETC, 0, 0, 0));
    return (int)(long)seL4_GetMR(0);
}

static void ser_put_dec(unsigned long v) {
    char buf[20]; int i = 0;
    if (v == 0) { ser_putc('0'); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) ser_putc(buf[i]);
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

#define LINE_MAX 128
static char line[LINE_MAX];

static void read_line(int *len) {
    *len = 0;
    while (*len < LINE_MAX - 1) {
        int c = ser_getc();
        if (c < 0) {
            /* No input — yield and retry */
            seL4_Yield();
            continue;
        }
        if (c == '\r' || c == '\n') {
            ser_putc('\n');
            break;
        }
        if ((c == 0x7f || c == '\b') && *len > 0) {
            (*len)--;
            ser_puts("\b \b");
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
            ser_puts("Commands: help, echo, hello, exit\n");
        } else if (str_eq(line, "hello")) {
            ser_puts("Hello from AIOS 0.4.x!\n");
        } else if (str_eq(line, "exit")) {
            ser_puts("Goodbye.\n");
            return 0;
        } else if (line[0] == 'e' && line[1] == 'c' && line[2] == 'h' &&
                   line[3] == 'o' && line[4] == ' ') {
            ser_puts(line + 5);
            ser_putc('\n');
        } else {
            ser_puts(line);
            ser_puts(": command not found\n");
        }
    }
    return 0;
}
