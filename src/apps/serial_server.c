/*
 * AIOS 0.4.x — Serial Server
 *
 * Two endpoints:
 *   EP (argv[0])  — shell I/O (PUTC, GETC, PUTS)
 *   KEP (argv[1]) — keyboard input from root (KEY_PUSH)
 *
 * GETC: if buffer empty, polls KEP briefly then returns -1
 */
#include <sel4/sel4.h>

#define SER_PUTC     1
#define SER_GETC     2
#define SER_PUTS     3
#define SER_KEY_PUSH 4

#define KEY_BUF_SIZE 512
static char key_buf[KEY_BUF_SIZE];
static int key_head = 0, key_tail = 0;

static int key_empty(void) { return key_head == key_tail; }
static void key_push(char c) {
    int next = (key_head + 1) % KEY_BUF_SIZE;
    if (next != key_tail) { key_buf[key_head] = c; key_head = next; }
}
static int key_pop(void) {
    if (key_empty()) return -1;
    char c = key_buf[key_tail];
    key_tail = (key_tail + 1) % KEY_BUF_SIZE;
    return (int)c;
}

static long parse_num(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

int main(int argc, char *argv[]) {
    seL4_CPtr ep = 0, kep = 0;
    if (argc > 0) ep = (seL4_CPtr)parse_num(argv[0]);
    if (argc > 1) kep = (seL4_CPtr)parse_num(argv[1]);


    while (1) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);
        seL4_Word len = seL4_MessageInfo_get_length(msg);

        switch (label) {
        case SER_PUTC:
            seL4_DebugPutChar((char)seL4_GetMR(0));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;

        case SER_GETC: {
            /* Check buffer first */
            int c = key_pop();
            seL4_SetMR(0, (seL4_Word)c);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }

        case SER_PUTS:
            for (seL4_Word i = 0; i < len; i++)
                seL4_DebugPutChar((char)seL4_GetMR(i));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;

        case SER_KEY_PUSH:
            key_push((char)seL4_GetMR(0));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;

        default:
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;
        }
    }
    return 0;
}
