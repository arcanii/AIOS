/*
 * AIOS 0.4.x — Mini Shell (Phase 3c test)
 */
#include <sel4/sel4.h>

#define SER_PUTC 1

static seL4_CPtr serial_ep;

static void ser_putc(char c) {
    seL4_SetMR(0, (seL4_Word)c);
    seL4_Call(serial_ep, seL4_MessageInfo_new(SER_PUTC, 0, 0, 1));
}

static void ser_puts(const char *s) {
    while (*s) ser_putc(*s++);
}

int main(int argc, char *argv[]) {
    serial_ep = 8;
    if (argc > 0 && argv[0]) {
        serial_ep = 0;
        for (const char *p = argv[0]; *p >= '0' && *p <= '9'; p++)
            serial_ep = serial_ep * 10 + (*p - '0');
    }

    seL4_DebugPutChar('\n');

    ser_puts("============================================\n");
    ser_puts("  AIOS 0.4.x Mini Shell\n");
    ser_puts("  All I/O via IPC to serial_server\n");
    ser_puts("============================================\n");
    ser_puts("\n");
    ser_puts("[shell] echo test: Hello from AIOS!\n");
    ser_puts("[shell] IPC putc working for all output.\n");
    ser_puts("[shell] Exiting.\n");

    return 0;
}
