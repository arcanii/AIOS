/*
 * AIOS 0.4.x — TTY Server
 *
 * Replaces serial_server. Provides:
 *   - Backward compat: SER_PUTC, SER_GETC, SER_PUTS, SER_KEY_PUSH
 *   - New TTY protocol: TTY_WRITE, TTY_READ, TTY_INPUT, TTY_IOCTL
 *   - Line discipline: cooked mode (echo, backspace, Ctrl-U, Ctrl-C)
 *   - Raw mode: pass-through (for shell's own line editor)
 *
 * Endpoint:
 *   EP (argv[0]) — all IPC (process I/O + keyboard input from root)
 *
 * Output: seL4_DebugPutChar (no serial_server dependency)
 * Input:  root sends TTY_INPUT/SER_KEY_PUSH with keystrokes
 */
#include <sel4/sel4.h>
#include <stdint.h>

/* IPC labels — keep in sync with tty.h */
#define SER_PUTC        1
#define SER_GETC        2
#define SER_PUTS        3
#define SER_KEY_PUSH    4

#define TTY_WRITE       70
#define TTY_READ        71
#define TTY_IOCTL       72
#define TTY_INPUT       75

#define TTY_IOCTL_SET_RAW       1
#define TTY_IOCTL_SET_COOKED    2
#define TTY_IOCTL_ECHO_ON       3
#define TTY_IOCTL_ECHO_OFF      4
#define TTY_IOCTL_GET_MODE      5

#define TTY_MODE_COOKED  0
#define TTY_MODE_RAW     1

/* ── Keystroke ring buffer (raw chars from root) ── */
#define KEY_BUF_SZ  512
static char key_buf[KEY_BUF_SZ];
static int key_head = 0, key_tail = 0;

static int key_empty(void) { return key_head == key_tail; }
static void key_push(char c) {
    int next = (key_head + 1) % KEY_BUF_SZ;
    if (next != key_tail) { key_buf[key_head] = c; key_head = next; }
}
static int key_pop(void) {
    if (key_empty()) return -1;
    char c = key_buf[key_tail];
    key_tail = (key_tail + 1) % KEY_BUF_SZ;
    return (int)(unsigned char)c;
}

/* ── Cooked mode line buffer ── */
#define LINE_BUF_SZ 256
static char line_buf[LINE_BUF_SZ];
static int line_len = 0;
static int line_ready = 0;

/* Completed line queue — holds finished lines for TTY_READ */
#define LINE_QUEUE_SZ 4096
static char line_queue[LINE_QUEUE_SZ];
static int lq_head = 0, lq_tail = 0;

static int lq_avail(void) {
    return (lq_head - lq_tail + LINE_QUEUE_SZ) % LINE_QUEUE_SZ;
}
static void lq_push(char c) {
    int next = (lq_head + 1) % LINE_QUEUE_SZ;
    if (next != lq_tail) { line_queue[lq_head] = c; lq_head = next; }
}
static int lq_pop(void) {
    if (lq_head == lq_tail) return -1;
    char c = line_queue[lq_tail];
    lq_tail = (lq_tail + 1) % LINE_QUEUE_SZ;
    return (int)(unsigned char)c;
}

/* ── TTY state ── */
static int tty_mode = TTY_MODE_COOKED;
static int tty_echo = 1;  /* ON: tty_server handles echo for all programs */

/* ── Output helper ── */
static void tty_putc(char c) {
    seL4_DebugPutChar(c);
}

static void tty_puts(const char *s) {
    while (*s) tty_putc(*s++);
}

/* ── Line discipline: process one input character ── */
static void line_discipline(char c) {
    if (tty_mode == TTY_MODE_RAW) {
        /* raw: key_buf only -- line_queue is for cooked TTY_READ */
        key_push(c);
        return;
    }

    /* Cooked mode */
    switch (c) {
    case 0x03: /* Ctrl-C */
        if (tty_echo) tty_puts("^C\n");
        /* Clear current line */
        line_len = 0;
        /* Push newline so any blocked TTY_READ wakes up with empty line */
        lq_push('\n');
        /* Also put Ctrl-C in key_buf for SER_GETC users (mini_shell) */
        key_push(c);
        break;

    case 0x04: /* Ctrl-D (EOF) */
        if (line_len == 0) {
            /* EOF on empty line — push special marker */
            lq_push(0x04);
        } else {
            /* Flush current line without newline */
            for (int i = 0; i < line_len; i++) lq_push(line_buf[i]);
            line_len = 0;
        }
        key_push(c);
        break;

    case 0x15: /* Ctrl-U — kill line */
        if (tty_echo) {
            /* Erase displayed chars */
            for (int i = 0; i < line_len; i++) tty_puts("\b \b");
        }
        line_len = 0;
        break;

    case 0x17: /* Ctrl-W — kill word */
        if (tty_echo) {
            /* Erase back to previous space */
            while (line_len > 0 && line_buf[line_len - 1] == ' ') {
                line_len--;
                tty_puts("\b \b");
            }
            while (line_len > 0 && line_buf[line_len - 1] != ' ') {
                line_len--;
                tty_puts("\b \b");
            }
        } else {
            while (line_len > 0 && line_buf[line_len - 1] == ' ') line_len--;
            while (line_len > 0 && line_buf[line_len - 1] != ' ') line_len--;
        }
        break;

    case 127:   /* DEL */
    case '\b':  /* Backspace */
        if (line_len > 0) {
            line_len--;
            if (tty_echo) tty_puts("\b \b");
        }
        /* Also let SER_GETC users see it */
        key_push(c);
        break;

    case '\r':  /* CR → treat as newline */
    case '\n':  /* Enter — complete line */
        if (tty_echo) tty_putc('\n');
        /* Push completed line into line_queue */
        for (int i = 0; i < line_len; i++) lq_push(line_buf[i]);
        lq_push('\n');
        line_len = 0;
        /* SER_GETC users get \n */
        key_push('\n');
        break;

    default:
        if (c >= 0x20 && c < 0x7F && line_len < LINE_BUF_SZ - 1) {
            line_buf[line_len++] = c;
            if (tty_echo) tty_putc(c);
        }
        /* All printable chars also go to key_buf for SER_GETC */
        key_push(c);
        break;
    }
}

/* ── Argument parsing ── */
static long parse_num(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

/* ── Main ── */
int main(int argc, char *argv[]) {
    seL4_CPtr ep = 0;
    if (argc > 0) ep = (seL4_CPtr)parse_num(argv[0]);

    while (1) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);
        seL4_Word len = seL4_MessageInfo_get_length(msg);

        switch (label) {

        /* ── Legacy serial protocol (mini_shell compat) ── */

        case SER_PUTC:
            tty_putc((char)seL4_GetMR(0));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;

        case SER_GETC: {
            int c = key_pop();
            seL4_SetMR(0, (seL4_Word)c);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }

        case SER_PUTS:
            for (seL4_Word i = 0; i < len; i++)
                tty_putc((char)seL4_GetMR(i));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;

        case SER_KEY_PUSH:
            /* Legacy: root sends raw keystroke — apply line discipline */
            line_discipline((char)seL4_GetMR(0));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;

        /* ── New TTY protocol ── */

        case TTY_INPUT:
            /* Root sends raw keystroke — apply line discipline */
            line_discipline((char)seL4_GetMR(0));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;

        case TTY_WRITE: {
            /* MR0=len, MR1..=packed data (8 chars per MR) */
            int wlen = (int)seL4_GetMR(0);
            int mr = 1;
            for (int i = 0; i < wlen; i++) {
                if (i > 0 && i % 8 == 0) mr++;
                char c = (char)((seL4_GetMR(mr) >> ((i % 8) * 8)) & 0xFF);
                tty_putc(c);
            }
            seL4_SetMR(0, (seL4_Word)wlen);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }

        case TTY_READ: {
            /* MR0=max_len → reply MR0=actual_len, MR1..=data */
            int max_len = (int)seL4_GetMR(0);
            int avail = lq_avail();
            int rlen = avail < max_len ? avail : max_len;
            /* Cap at what fits in MRs */
            int max_mr_bytes = ((int)seL4_MsgMaxLength - 1) * 8;
            if (rlen > max_mr_bytes) rlen = max_mr_bytes;

            seL4_SetMR(0, (seL4_Word)rlen);
            if (rlen > 0) {
                int mr = 1;
                seL4_Word w = 0;
                for (int i = 0; i < rlen; i++) {
                    int c = lq_pop();
                    if (c < 0) { rlen = i; break; }
                    w |= ((seL4_Word)(uint8_t)c) << ((i % 8) * 8);
                    if (i % 8 == 7 || i == rlen - 1) {
                        seL4_SetMR(mr++, w);
                        w = 0;
                    }
                }
                seL4_SetMR(0, (seL4_Word)rlen);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, mr));
            } else {
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            }
            break;
        }

        case TTY_IOCTL: {
            int op = (int)seL4_GetMR(0);
            int result = 0;
            switch (op) {
            case TTY_IOCTL_SET_RAW:
                tty_mode = TTY_MODE_RAW;
                lq_head = lq_tail = 0;
                key_head = key_tail = 0;
                line_len = 0;
                break;
            case TTY_IOCTL_SET_COOKED:
                tty_mode = TTY_MODE_COOKED;
                key_head = key_tail = 0;
                line_len = 0;
                break;
            case TTY_IOCTL_ECHO_ON:
                tty_echo = 1;
                break;
            case TTY_IOCTL_ECHO_OFF:
                tty_echo = 0;
                break;
            case TTY_IOCTL_GET_MODE:
                result = tty_mode;
                break;
            }
            seL4_SetMR(0, (seL4_Word)result);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }

        default:
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;
        }
    }
    return 0;
}
