/*
 * AIOS 0.4.x — Serial Server with real PL011 UART
 *
 * Receives from root task:
 *   argv[0] = endpoint slot
 *   argv[1] = UART frame cap slot
 *   argv[2] = IRQ handler cap slot
 *
 * IPC Protocol:
 *   Label 1 (PUTC): MR0 = char
 *   Label 2 (GETC): reply MR0 = char or -1
 *   Label 3 (PUTS): MR0..MRn = chars
 */
#include <stdio.h>
#include <stdint.h>
#include <sel4/sel4.h>

#define SER_PUTC 1
#define SER_GETC 2
#define SER_PUTS 3

/* PL011 registers */
#define UART_DR    0x000
#define UART_FR    0x018
#define UART_IMSC  0x038
#define UART_ICR   0x044
#define FR_RXFE    (1 << 4)
#define FR_TXFF    (1 << 5)

static volatile uint32_t *uart_base;

static void uart_putc(char c) {
    if (!uart_base) { seL4_DebugPutChar(c); return; }
    while (uart_base[UART_FR/4] & FR_TXFF) ;
    uart_base[UART_DR/4] = (uint32_t)c;
}

static int uart_getc(void) {
    if (!uart_base) return -1;
    if (uart_base[UART_FR/4] & FR_RXFE) return -1;
    return (int)(uart_base[UART_DR/4] & 0xFF);
}

static long parse_num(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

int main(int argc, char *argv[]) {
    seL4_CPtr ep = 0, uart_frame_cap = 0, irq_cap = 0;

    if (argc > 0) ep = (seL4_CPtr)parse_num(argv[0]);
    if (argc > 1) uart_frame_cap = (seL4_CPtr)parse_num(argv[1]);
    if (argc > 2) irq_cap = (seL4_CPtr)parse_num(argv[2]);

    printf("[serial] EP=%lu UART_FRAME=%lu IRQ=%lu\n",
           (unsigned long)ep, (unsigned long)uart_frame_cap,
           (unsigned long)irq_cap);

    /* Map UART frame if we have the cap */
    if (uart_frame_cap) {
        /* Map the frame at a fixed virtual address in our VSpace */
        void *uart_vaddr = (void *)0x10000000UL;

        /* Use seL4_ARM_Page_Map to map the frame cap into our VSpace */
        seL4_Error err = seL4_ARM_Page_Map(
            uart_frame_cap,        /* frame cap */
            seL4_CapInitThreadVSpace, /* our VSpace */
            (seL4_Word)uart_vaddr, /* vaddr */
            seL4_AllRights,
            seL4_ARM_Default_VMAttributes);
        if (err) {
            printf("[serial] WARNING: UART map failed: %d, using DebugPutChar\n", err);
        } else {
            uart_base = (volatile uint32_t *)uart_vaddr;
            printf("[serial] UART mapped at %p\n", uart_vaddr);
        }
    }

    if (!uart_base) {
        printf("[serial] Fallback: using seL4_DebugPutChar\n");
    }

    /* Acknowledge IRQ if we have it */
    if (irq_cap) {
        seL4_IRQHandler_Ack(irq_cap);
    }

    /* Service loop */
    while (1) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);
        seL4_Word len = seL4_MessageInfo_get_length(msg);

        switch (label) {
        case SER_PUTC: {
            char c = (char)seL4_GetMR(0);
            uart_putc(c);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;
        }
        case SER_GETC: {
            int c = uart_getc();
            seL4_SetMR(0, (seL4_Word)c);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }
        case SER_PUTS: {
            for (seL4_Word i = 0; i < len; i++) {
                uart_putc((char)seL4_GetMR(i));
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
