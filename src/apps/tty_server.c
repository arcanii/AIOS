/*
 * AIOS 0.4.x -- TTY Server
 *
 * Replaces serial_server. Provides:
 *   - Backward compat: SER_PUTC, SER_GETC, SER_PUTS, SER_KEY_PUSH
 *   - New TTY protocol: TTY_WRITE, TTY_READ, TTY_INPUT, TTY_IOCTL
 *   - Line discipline: cooked mode (echo, backspace, Ctrl-U, Ctrl-C)
 *   - Raw mode: pass-through (for ZLE / shell line editors)
 *   - Full termios: ISIG, ICRNL, ECHO, ICANON, VMIN/VTIME
 *
 * Endpoint:
 *   EP (argv[0]) -- all IPC (process I/O + keyboard input from root)
 *
 * Output: seL4_DebugPutChar (no serial_server dependency)
 * Input:  root sends TTY_INPUT/SER_KEY_PUSH with keystrokes
 *
 * v0.4.99: Server-side termios struct, ISIG control, TCGETS/TCSETS IPC
 */
#include <sel4/sel4.h>
#include <stdint.h>

/* IPC labels -- keep in sync with tty.h */
#define SER_PUTC        1
#define SER_GETC        2
#define SER_PUTS        3
#define SER_KEY_PUSH    4

#define TTY_WRITE       70
#define TTY_READ        71
#define TTY_IOCTL       72
#define TTY_INPUT       75
#define TTY_POLL        76   /* v0.4.99: check if input available */

#define TTY_IOCTL_SET_RAW       1
#define TTY_IOCTL_SET_COOKED    2
#define TTY_IOCTL_ECHO_ON       3
#define TTY_IOCTL_ECHO_OFF      4
#define TTY_IOCTL_GET_MODE      5
#define TTY_IOCTL_TCGETS        6   /* v0.4.99: get full termios */
#define TTY_IOCTL_TCSETS        7   /* v0.4.99: set full termios */
#define TTY_IOCTL_TCSETSW       8
#define TTY_IOCTL_TCSETSF       9

#define TTY_MODE_COOKED  0
#define TTY_MODE_RAW     1

/* termios flag bits (match musl/Linux) */
#define T_ICRNL   0x0100
#define T_OPOST   0x0001
#define T_ONLCR   0x0004
#define T_ECHO    0x0008
#define T_ICANON  0x0002
#define T_ISIG    0x0001
#define T_CS8     0x0030
#define T_CREAD   0x0080
#define T_CLOCAL  0x0800
#define T_B9600   0x000D

/* c_cc indices */
#define T_VINTR   0
#define T_VQUIT   1
#define T_VERASE  2
#define T_VKILL   3
#define T_VEOF    4
#define T_VMIN    6
#define T_VTIME   5
#define T_VSUSP   10
#define T_VWERASE 14

/* -- Keystroke ring buffer (raw chars from root) -- */
#define KEY_BUF_SZ  512
static char key_buf[KEY_BUF_SZ];
static int key_head = 0, key_tail = 0;

static int key_empty(void) { return key_head == key_tail; }
static int key_count(void) {
    return (key_head - key_tail + KEY_BUF_SZ) % KEY_BUF_SZ;
}
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

/* -- Cooked mode line buffer -- */
#define LINE_BUF_SZ 256
static char line_buf[LINE_BUF_SZ];
static int line_len = 0;

/* Completed line queue -- holds finished lines for TTY_READ */
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

/* -- Server-side termios state (v0.4.99) -- */
static uint32_t ts_iflag = T_ICRNL;
static uint32_t ts_oflag = T_OPOST | T_ONLCR;
static uint32_t ts_cflag = T_CS8 | T_CREAD | T_CLOCAL | T_B9600;
static uint32_t ts_lflag = T_ECHO | T_ICANON | T_ISIG;
static uint8_t  ts_cc[20] = {
    [T_VINTR]   = 0x03,
    [T_VQUIT]   = 0x1C,
    [T_VERASE]  = 0x7F,
    [T_VKILL]   = 0x15,
    [T_VEOF]    = 0x04,
    [T_VMIN]    = 1,
    [T_VTIME]   = 0,
    [T_VSUSP]   = 0x1A,
    [T_VWERASE] = 0x17,
};

/* Derived state from termios */
static int tty_mode = TTY_MODE_COOKED;
static int tty_echo = 1;
static int tty_isig = 1;
static int tty_icrnl = 1;

/* Apply derived state from termios flags */
static void termios_sync(void) {
    tty_mode  = (ts_lflag & T_ICANON) ? TTY_MODE_COOKED : TTY_MODE_RAW;
    tty_echo  = (ts_lflag & T_ECHO) ? 1 : 0;
    tty_isig  = (ts_lflag & T_ISIG) ? 1 : 0;
    tty_icrnl = (ts_iflag & T_ICRNL) ? 1 : 0;
}

/* -- Output helper -- */
static void tty_putc(char c) {
    seL4_DebugPutChar(c);
}

static void tty_puts(const char *s) {
    while (*s) tty_putc(*s++);
}

/* -- Line discipline: process one input character -- */
static void line_discipline(char c) {
    /* CR -> NL conversion */
    if (tty_icrnl && c == '\r') c = '\n';

    if (tty_mode == TTY_MODE_RAW) {
        /* Raw: all chars go to key_buf for single-char reads */
        key_push(c);
        return;
    }

    /* Signal generation (only when ISIG is set) */
    if (tty_isig) {
        if (c == (char)ts_cc[T_VINTR]) {
            if (tty_echo) tty_puts("^C\n");
            line_len = 0;
            lq_push('\n');
            key_push(c);
            return;
        }
    }

    /* Cooked mode */
    switch (c) {
    case 0x04: /* Ctrl-D (EOF) */
        if (line_len == 0) {
            lq_push(0x04);
        } else {
            for (int i = 0; i < line_len; i++) lq_push(line_buf[i]);
            line_len = 0;
        }
        key_push(c);
        break;

    case 0x15: /* Ctrl-U -- kill line */
        if (tty_echo) {
            for (int i = 0; i < line_len; i++) tty_puts("\b \b");
        }
        line_len = 0;
        break;

    case 0x17: /* Ctrl-W -- kill word */
        if (tty_echo) {
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
        key_push(c);
        break;

    case '\n':  /* Enter -- complete line */
        if (tty_echo) tty_putc('\n');
        for (int i = 0; i < line_len; i++) lq_push(line_buf[i]);
        lq_push('\n');
        line_len = 0;
        key_push('\n');
        break;

    default:
        if (c >= 0x20 && c < 0x7F && line_len < LINE_BUF_SZ - 1) {
            line_buf[line_len++] = c;
            if (tty_echo) tty_putc(c);
        }
        key_push(c);
        break;
    }
}

/* Pack termios into MRs: 4 words (iflag,oflag,cflag,lflag) + 3 words (cc[0..19] packed) */
static void termios_pack_reply(void) {
    seL4_SetMR(0, (seL4_Word)ts_iflag);
    seL4_SetMR(1, (seL4_Word)ts_oflag);
    seL4_SetMR(2, (seL4_Word)ts_cflag);
    seL4_SetMR(3, (seL4_Word)ts_lflag);
    /* Pack cc[0..19] into 3 MRs (8 bytes per MR on aarch64) */
    for (int m = 0; m < 3; m++) {
        seL4_Word w = 0;
        for (int b = 0; b < 8 && (m * 8 + b) < 20; b++) {
            w |= ((seL4_Word)ts_cc[m * 8 + b]) << (b * 8);
        }
        seL4_SetMR(4 + m, w);
    }
}

/* Unpack termios from MRs */
static void termios_unpack(void) {
    ts_iflag = (uint32_t)seL4_GetMR(1);
    ts_oflag = (uint32_t)seL4_GetMR(2);
    ts_cflag = (uint32_t)seL4_GetMR(3);
    ts_lflag = (uint32_t)seL4_GetMR(4);
    for (int m = 0; m < 3; m++) {
        seL4_Word w = seL4_GetMR(5 + m);
        for (int b = 0; b < 8 && (m * 8 + b) < 20; b++) {
            ts_cc[m * 8 + b] = (uint8_t)((w >> (b * 8)) & 0xFF);
        }
    }
    termios_sync();
}

/* -- Argument parsing -- */
static long parse_num(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

/* -- Main -- */
int main(int argc, char *argv[]) {
    seL4_CPtr ep = 0;
    if (argc > 0) ep = (seL4_CPtr)parse_num(argv[0]);

    while (1) {
        seL4_Word badge;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);
        seL4_Word len = seL4_MessageInfo_get_length(msg);

        switch (label) {

        /* -- Legacy serial protocol (mini_shell compat) -- */

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
            line_discipline((char)seL4_GetMR(0));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;

        /* -- New TTY protocol -- */

        case TTY_INPUT:
            line_discipline((char)seL4_GetMR(0));
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;

        case TTY_WRITE: {
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
            int max_len = (int)seL4_GetMR(0);

            /* In raw mode, read from key_buf; in cooked mode, from line_queue */
            int rlen = 0;
            int max_mr_bytes = ((int)seL4_MsgMaxLength - 1) * 8;
            if (max_len > max_mr_bytes) max_len = max_mr_bytes;

            if (tty_mode == TTY_MODE_RAW) {
                /* Raw: return whatever is in key_buf */
                int avail = key_count();
                rlen = avail < max_len ? avail : max_len;
                seL4_SetMR(0, (seL4_Word)rlen);
                if (rlen > 0) {
                    int mr = 1;
                    seL4_Word w = 0;
                    for (int i = 0; i < rlen; i++) {
                        int c = key_pop();
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
            } else {
                /* Cooked: return from line_queue */
                int avail = lq_avail();
                rlen = avail < max_len ? avail : max_len;
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
            }
            break;
        }

        case TTY_POLL: {
            /* v0.4.99: return count of available input bytes */
            int avail = 0;
            if (tty_mode == TTY_MODE_RAW) {
                avail = key_count();
            } else {
                avail = lq_avail();
            }
            seL4_SetMR(0, (seL4_Word)avail);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }

        case TTY_IOCTL: {
            int op = (int)seL4_GetMR(0);
            int result = 0;
            switch (op) {
            case TTY_IOCTL_SET_RAW:
                ts_lflag &= ~T_ICANON;
                termios_sync();
                lq_head = lq_tail = 0;
                key_head = key_tail = 0;
                line_len = 0;
                break;
            case TTY_IOCTL_SET_COOKED:
                ts_lflag |= T_ICANON;
                termios_sync();
                key_head = key_tail = 0;
                line_len = 0;
                break;
            case TTY_IOCTL_ECHO_ON:
                ts_lflag |= T_ECHO;
                termios_sync();
                break;
            case TTY_IOCTL_ECHO_OFF:
                ts_lflag &= ~T_ECHO;
                termios_sync();
                break;
            case TTY_IOCTL_GET_MODE:
                result = tty_mode;
                break;
            case TTY_IOCTL_TCGETS:
                /* v0.4.99: return full termios via MRs */
                termios_pack_reply();
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 7));
                goto next_msg;
            case TTY_IOCTL_TCSETS:
            case TTY_IOCTL_TCSETSW:
            case TTY_IOCTL_TCSETSF:
                /* v0.4.99: receive full termios via MRs */
                termios_unpack();
                if (op == TTY_IOCTL_TCSETSF) {
                    /* Flush input */
                    lq_head = lq_tail = 0;
                    key_head = key_tail = 0;
                    line_len = 0;
                }
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
next_msg:
        continue;
    }
    return 0;
}
