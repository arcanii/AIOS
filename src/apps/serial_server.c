/*
 * AIOS 0.4.x — Serial Server
 * TX: seL4_DebugPutChar, RX: direct UART read
 * argv[0]=ep  argv[1]=uart_frame  argv[2]=irq
 */
#include <stdio.h>
#include <stdint.h>
#include <sel4/sel4.h>

#define SER_PUTC 1
#define SER_GETC 2
#define SER_PUTS 3

static long parse_num(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

int main(int argc, char *argv[]) {
    seL4_CPtr ep = 0;
    if (argc > 0) ep = (seL4_CPtr)parse_num(argv[0]);

    /* Skip UART mapping for now — use DebugPutChar for TX,
     * return -1 for RX (no keyboard yet) */
    printf("[serial] EP=%lu (no UART map, debug mode)\n", (unsigned long)ep);
    printf("[serial] Ready.\n");

    while (1) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);
        seL4_Word len = seL4_MessageInfo_get_length(msg);

        switch (label) {
        case SER_PUTC: {
            seL4_DebugPutChar((char)seL4_GetMR(0));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;
        }
        case SER_GETC: {
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case SER_PUTS: {
            for (seL4_Word i = 0; i < len; i++) {
                seL4_DebugPutChar((char)seL4_GetMR(i));
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;
        }
        default:
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;
        }
    }
    return 0;
}
