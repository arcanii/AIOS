/*
 * AIOS 0.4.x — Serial Server
 * Maps PL011 UART, provides putc/getc via IPC endpoint.
 * For now uses seL4_DebugPutChar for output (real UART mapping later).
 *
 * IPC Protocol:
 *   Label 1 (PUTC): MR0 = char -> no reply data
 *   Label 2 (GETC): -> MR0 = char or -1
 *   Label 3 (PUTS): MR0..MRn = chars -> no reply data
 */
#include <stdio.h>
#include <sel4/sel4.h>

#define SER_PUTC 1
#define SER_GETC 2
#define SER_PUTS 3

int main(int argc, char *argv[]) {
    seL4_CPtr ep = 8; /* default */
    if (argc > 0 && argv[0]) {
        ep = 0;
        for (const char *p = argv[0]; *p >= '0' && *p <= '9'; p++)
            ep = ep * 10 + (*p - '0');
    }

    printf("[serial] Started on EP %lu\n", (unsigned long)ep);

    /* Serve forever */
    while (1) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);
        seL4_Word len = seL4_MessageInfo_get_length(msg);

        switch (label) {
        case SER_PUTC: {
            char c = (char)seL4_GetMR(0);
            seL4_DebugPutChar(c);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;
        }
        case SER_GETC: {
            /* TODO: real UART RX. For now return -1 (no input) */
            seL4_SetMR(0, (seL4_Word)-1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case SER_PUTS: {
            for (seL4_Word i = 0; i < len; i++) {
                char c = (char)seL4_GetMR(i);
                seL4_DebugPutChar(c);
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
