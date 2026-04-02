/*
 * AIOS 0.4.x — Mini Shell
 * Uses IPC to serial_server for I/O.
 * Demonstrates: user process -> service process -> hardware
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
        /* Pack up to 4 chars per IPC message */
        int n = 0;
        while (s[n] && n < seL4_MsgMaxLength) {
            seL4_SetMR(n, (seL4_Word)s[n]);
            n++;
        }
        seL4_Call(serial_ep, seL4_MessageInfo_new(SER_PUTS, 0, 0, n));
        s += n;
    }
}

static void ser_put_dec(unsigned long v) {
    char buf[20];
    int i = 0;
    if (v == 0) { ser_putc('0'); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) ser_putc(buf[i]);
}

int main(int argc, char *argv[]) {
    /* EP slot passed as argv[0] */
    serial_ep = 8;
    if (argc > 0 && argv[0]) {
        serial_ep = 0;
        for (const char *p = argv[0]; *p >= '0' && *p <= '9'; p++)
            serial_ep = serial_ep * 10 + (*p - '0');
    }

    ser_puts("\n");
    ser_puts("============================================\n");
    ser_puts("  AIOS 0.4.x Mini Shell\n");
    ser_puts("  I/O via IPC to serial_server\n");
    ser_puts("============================================\n\n");

    /* Demonstrate IPC-based I/O */
    ser_puts("[shell] My PID: ");
    /* We don't have getpid yet, just show we're alive */
    ser_puts("running in isolated VSpace\n");

    for (int i = 1; i <= 5; i++) {
        ser_puts("[shell] tick ");
        ser_put_dec(i);
        ser_puts("\n");
        /* Small delay via yields */
        for (int j = 0; j < 100; j++) seL4_Yield();
    }

    ser_puts("[shell] Done. Exiting.\n\n");
    return 0;
}
