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
 * v0.4.99:  Server-side termios struct, ISIG control, TCGETS/TCSETS IPC
 * v0.4.295: MULTI-INSTANCE (PTY step 1). All per-TTY state lives in tty_inst_t;
 *           instance 0 is the serial/keyboard console (output -> mini-UART + HDMI),
 *           byte-identical to before. PTY instances (1..MAX_TTY-1) are wired in a
 *           later step (docs/DESIGN_PTY_SSH.md). This step is pure scaffolding:
 *           the existing single-console protocol all targets instance 0.
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
#define TTY_PTY_ALLOC       80  /* v0.4.295: master-side PTY ops (instance id in MR0) */
#define TTY_PTY_INPUT       81
#define TTY_PTY_MASTER_READ 82
#define TTY_PTY_WINSZ       83
#define TTY_PTY_FREE        84
#define TTY_PTY_SLAVE_READ  85  /* v0.4.296: slave-side PTY ops (instance id in MR0) */
#define TTY_PTY_SLAVE_WRITE 86
#define TTY_PTY_SLAVE_POLL  87
#define TTY_PTY_SLAVE_IOCTL 88

#define DISP_CONSOLE    116  /* mirror output to the HDMI fb_console (root_shared.h) */

#define PIPE_SIGNAL     75   /* pipe_server: kill(pid,sig); kill(0,SIGINT) -> fg_pid */
#define SIG_INT          2   /* SIGINT for the line-discipline VINTR (PTY ISIG) */

#define TTY_IOCTL_SET_RAW       1
#define TTY_IOCTL_SET_COOKED    2
#define TTY_IOCTL_ECHO_ON       3
#define TTY_IOCTL_ECHO_OFF      4
#define TTY_IOCTL_GET_MODE      5
#define TTY_IOCTL_TCGETS        6   /* v0.4.99: get full termios */
#define TTY_IOCTL_TCSETS        7   /* v0.4.99: set full termios */
#define TTY_IOCTL_TCSETSW       8
#define TTY_IOCTL_TCSETSF       9
#define TTY_IOCTL_GET_WINSZ     10  /* v0.4.296: reply MR0=rows, MR1=cols */

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

#define KEY_BUF_SZ   512
#define LINE_BUF_SZ  256
#define LINE_QUEUE_SZ 4096
#define MASTER_OUT_SZ 8192   /* PTY: shell output + echo awaiting the master (sshd) */
#define MAX_TTY      8     /* instance 0 = serial console; 1.. = PTYs */

/* -- Per-instance TTY state (v0.4.295 PTY step 1/2) -- */
typedef struct {
    int used;             /* slot in use (instance 0 = serial, always used) */
    int is_pty;           /* 0 = serial console (output -> UART+HDMI); 1 = PTY (-> master_out) */
    /* raw input ring (keystrokes; raw mode + signal/cooked feed) */
    char key_buf[KEY_BUF_SZ]; int key_head, key_tail;
    /* cooked-mode line being assembled */
    char line_buf[LINE_BUF_SZ]; int line_len;
    /* completed lines awaiting TTY_READ */
    char line_queue[LINE_QUEUE_SZ]; int lq_head, lq_tail;
    /* PTY master-output ring: shell stdout + echo, drained by TTY_PTY_MASTER_READ */
    char master_out[MASTER_OUT_SZ]; int mo_head, mo_tail;
    uint16_t ws_row, ws_col;
    /* server-side termios */
    uint32_t iflag, oflag, cflag, lflag; uint8_t cc[20];
    /* derived from termios */
    int mode, echo, isig, icrnl;
} tty_inst_t;

static tty_inst_t tty_inst[MAX_TTY];

/* -- Keystroke ring buffer -- */
static int key_empty(tty_inst_t *t) { return t->key_head == t->key_tail; }
static int key_count(tty_inst_t *t) {
    return (t->key_head - t->key_tail + KEY_BUF_SZ) % KEY_BUF_SZ;
}
static void key_push(tty_inst_t *t, char c) {
    int next = (t->key_head + 1) % KEY_BUF_SZ;
    if (next != t->key_tail) { t->key_buf[t->key_head] = c; t->key_head = next; }
}
static int key_pop(tty_inst_t *t) {
    if (key_empty(t)) return -1;
    char c = t->key_buf[t->key_tail];
    t->key_tail = (t->key_tail + 1) % KEY_BUF_SZ;
    return (int)(unsigned char)c;
}

/* -- Completed line queue -- */
static int lq_avail(tty_inst_t *t) {
    return (t->lq_head - t->lq_tail + LINE_QUEUE_SZ) % LINE_QUEUE_SZ;
}
static void lq_push(tty_inst_t *t, char c) {
    int next = (t->lq_head + 1) % LINE_QUEUE_SZ;
    if (next != t->lq_tail) { t->line_queue[t->lq_head] = c; t->lq_head = next; }
}
static int lq_pop(tty_inst_t *t) {
    if (t->lq_head == t->lq_tail) return -1;
    char c = t->line_queue[t->lq_tail];
    t->lq_tail = (t->lq_tail + 1) % LINE_QUEUE_SZ;
    return (int)(unsigned char)c;
}

/* Apply derived state from termios flags */
static void termios_sync(tty_inst_t *t) {
    t->mode  = (t->lflag & T_ICANON) ? TTY_MODE_COOKED : TTY_MODE_RAW;
    t->echo  = (t->lflag & T_ECHO) ? 1 : 0;
    t->isig  = (t->lflag & T_ISIG) ? 1 : 0;
    t->icrnl = (t->iflag & T_ICRNL) ? 1 : 0;
}

/* Initialize an instance to the default console termios. */
static void tty_inst_init(tty_inst_t *t) {
    t->key_head = t->key_tail = 0;
    t->line_len = 0;
    t->lq_head = t->lq_tail = 0;
    t->iflag = T_ICRNL;
    t->oflag = T_OPOST | T_ONLCR;
    t->cflag = T_CS8 | T_CREAD | T_CLOCAL | T_B9600;
    t->lflag = T_ECHO | T_ICANON | T_ISIG;
    for (int i = 0; i < 20; i++) t->cc[i] = 0;
    t->cc[T_VINTR]   = 0x03;
    t->cc[T_VQUIT]   = 0x1C;
    t->cc[T_VERASE]  = 0x7F;
    t->cc[T_VKILL]   = 0x15;
    t->cc[T_VEOF]    = 0x04;
    t->cc[T_VMIN]    = 1;
    t->cc[T_VTIME]   = 0;
    t->cc[T_VSUSP]   = 0x1A;
    t->cc[T_VWERASE] = 0x17;
    termios_sync(t);
}

/* -- Output: mini-UART (seL4_DebugPutChar) always, plus the HDMI fb_console via the
 * root-task display server when its endpoint was passed (argv[1]). This is what makes
 * the shell visible on HDMI for standalone USB-keyboard use. Instance 0 (serial
 * console) only -- PTY instances route output to a master ring in a later step. -- */
static seL4_CPtr disp_ep = 0;   /* display_server endpoint (DISP_CONSOLE), or 0 */
static seL4_CPtr pipe_ep = 0;   /* v0.4.296: pipe_server endpoint -- PTY VINTR -> SIGINT */

/* v0.4.296: deliver SIGINT to the foreground process (PTY VINTR in cooked+ISIG).
 * pipe_server resolves kill(0,sig) -> its global fg_pid (the most-recently-exec'd
 * interactive process), so the tty_server need not track the fg pid itself. The
 * caller has already been Replied to, so the IPC MRs are free scratch here. */
static void pty_send_sigint(void) {
    if (!pipe_ep) return;
    seL4_SetMR(0, 0);            /* pid 0 -> fg_pid */
    seL4_SetMR(1, (seL4_Word)SIG_INT);
    seL4_Call(pipe_ep, seL4_MessageInfo_new(PIPE_SIGNAL, 0, 0, 2));
}

/* Send a run of chars to the HDMI console (chars packed 8-per-word from MR1).
 * NOTE: this clobbers the IPC message registers, so callers that still need the
 * incoming message's MRs must copy them out BEFORE calling any output helper. */
static void disp_write(const char *buf, int len) {
    if (!disp_ep || len <= 0) return;
    while (len > 0) {
        int n = len > 120 ? 120 : len;            /* fits in MR0(len) + 15 data MRs */
        seL4_SetMR(0, (seL4_Word)n);
        for (int i = 0; i < n; i += 8) {
            seL4_Word w = 0;
            for (int j = 0; j < 8 && i + j < n; j++)
                w |= (seL4_Word)(unsigned char)buf[i + j] << (j * 8);
            seL4_SetMR(1 + i / 8, w);
        }
        seL4_Call(disp_ep, seL4_MessageInfo_new(DISP_CONSOLE, 0, 0, 1 + (n + 7) / 8));
        buf += n; len -= n;
    }
}

/* Write a buffer to BOTH outputs (serial char-by-char, HDMI batched). */
static void tty_write_buf(const char *buf, int len) {
    for (int i = 0; i < len; i++) seL4_DebugPutChar(buf[i]);
    disp_write(buf, len);
}

static void tty_putc(char c) {
    tty_write_buf(&c, 1);
}

static void tty_puts(const char *s) {
    int n = 0; while (s[n]) n++;
    tty_write_buf(s, n);
}

/* -- PTY master-output ring (is_pty instances only) -- */
static void mo_push(tty_inst_t *t, char c) {
    int next = (t->mo_head + 1) % MASTER_OUT_SZ;
    if (next != t->mo_tail) { t->master_out[t->mo_head] = c; t->mo_head = next; }
}
static int mo_avail(tty_inst_t *t) {
    return (t->mo_head - t->mo_tail + MASTER_OUT_SZ) % MASTER_OUT_SZ;
}
static int mo_pop(tty_inst_t *t) {
    if (t->mo_head == t->mo_tail) return -1;
    char c = t->master_out[t->mo_tail];
    t->mo_tail = (t->mo_tail + 1) % MASTER_OUT_SZ;
    return (int)(unsigned char)c;
}

/* Instance-aware output: a PTY routes to its master ring (the sshd drains it); the
 * serial console (instance 0) goes to the mini-UART + HDMI as before. For a PTY we
 * apply output post-processing (OPOST + ONLCR: \n -> \r\n) HERE, so a raw-mode
 * full-screen app (vi/less clears OPOST) gets an unmangled byte stream while a
 * cooked shell still gets CR+LF. This replaces the sshd relay's old blanket
 * \n->\r\n (which would mangle raw output). */
static void inst_out(tty_inst_t *t, const char *buf, int len) {
    if (t->is_pty) {
        int onlcr = (t->oflag & T_OPOST) && (t->oflag & T_ONLCR);
        for (int i = 0; i < len; i++) {
            if (onlcr && buf[i] == '\n') mo_push(t, '\r');
            mo_push(t, buf[i]);
        }
    } else tty_write_buf(buf, len);
}
static void inst_putc(tty_inst_t *t, char c) { inst_out(t, &c, 1); }
static void inst_puts(tty_inst_t *t, const char *s) { int n = 0; while (s[n]) n++; inst_out(t, s, n); }

/* -- Line discipline: process one input character for instance t -- */
static void line_discipline(tty_inst_t *t, char c) {
    /* CR -> NL conversion */
    if (t->icrnl && c == '\r') c = '\n';

    if (t->mode == TTY_MODE_RAW) {
        /* Raw: all chars go to key_buf for single-char reads */
        key_push(t, c);
        return;
    }

    /* Signal generation (only when ISIG is set) */
    if (t->isig) {
        if (c == (char)t->cc[T_VINTR]) {
            if (t->echo) inst_puts(t, "^C\n");
            t->line_len = 0;
            if (t->is_pty) {
                /* Real tty semantics: VINTR raises SIGINT at the fg process and
                 * DISCARDS the partial line -- the blocked slave read() returns
                 * EINTR (its libc yield-loop observes the pending signal), not an
                 * empty line. Serial console (instance 0) keeps the legacy
                 * empty-line behaviour below (no signal path wired there). */
                pty_send_sigint();
            } else {
                lq_push(t, '\n');
                key_push(t, c);
            }
            return;
        }
    }

    /* Cooked mode */
    switch (c) {
    case 0x04: /* Ctrl-D (EOF) */
        if (t->line_len == 0) {
            lq_push(t, 0x04);
        } else {
            for (int i = 0; i < t->line_len; i++) lq_push(t, t->line_buf[i]);
            t->line_len = 0;
        }
        key_push(t, c);
        break;

    case 0x15: /* Ctrl-U -- kill line */
        if (t->echo) {
            for (int i = 0; i < t->line_len; i++) inst_puts(t, "\b \b");
        }
        t->line_len = 0;
        break;

    case 0x17: /* Ctrl-W -- kill word */
        if (t->echo) {
            while (t->line_len > 0 && t->line_buf[t->line_len - 1] == ' ') {
                t->line_len--;
                inst_puts(t, "\b \b");
            }
            while (t->line_len > 0 && t->line_buf[t->line_len - 1] != ' ') {
                t->line_len--;
                inst_puts(t, "\b \b");
            }
        } else {
            while (t->line_len > 0 && t->line_buf[t->line_len - 1] == ' ') t->line_len--;
            while (t->line_len > 0 && t->line_buf[t->line_len - 1] != ' ') t->line_len--;
        }
        break;

    case 127:   /* DEL */
    case '\b':  /* Backspace */
        if (t->line_len > 0) {
            t->line_len--;
            if (t->echo) inst_puts(t, "\b \b");
        }
        key_push(t, c);
        break;

    case '\n':  /* Enter -- complete line */
        if (t->echo) inst_putc(t, '\n');
        for (int i = 0; i < t->line_len; i++) lq_push(t, t->line_buf[i]);
        lq_push(t, '\n');
        t->line_len = 0;
        key_push(t, '\n');
        break;

    default:
        if (c >= 0x20 && c < 0x7F && t->line_len < LINE_BUF_SZ - 1) {
            t->line_buf[t->line_len++] = c;
            if (t->echo) inst_putc(t, c);
        }
        key_push(t, c);
        break;
    }
}

/* Pack termios into MRs: 4 words (iflag,oflag,cflag,lflag) + 3 words (cc[0..19] packed) */
static void termios_pack_reply(tty_inst_t *t) {
    seL4_SetMR(0, (seL4_Word)t->iflag);
    seL4_SetMR(1, (seL4_Word)t->oflag);
    seL4_SetMR(2, (seL4_Word)t->cflag);
    seL4_SetMR(3, (seL4_Word)t->lflag);
    /* Pack cc[0..19] into 3 MRs (8 bytes per MR on aarch64) */
    for (int m = 0; m < 3; m++) {
        seL4_Word w = 0;
        for (int b = 0; b < 8 && (m * 8 + b) < 20; b++) {
            w |= ((seL4_Word)t->cc[m * 8 + b]) << (b * 8);
        }
        seL4_SetMR(4 + m, w);
    }
}

/* Unpack termios from MRs (MR1.. -- MR0 may carry an instance id later) */
static void termios_unpack(tty_inst_t *t) {
    t->iflag = (uint32_t)seL4_GetMR(1);
    t->oflag = (uint32_t)seL4_GetMR(2);
    t->cflag = (uint32_t)seL4_GetMR(3);
    t->lflag = (uint32_t)seL4_GetMR(4);
    for (int m = 0; m < 3; m++) {
        seL4_Word w = seL4_GetMR(5 + m);
        for (int b = 0; b < 8 && (m * 8 + b) < 20; b++) {
            t->cc[m * 8 + b] = (uint8_t)((w >> (b * 8)) & 0xFF);
        }
    }
    termios_sync(t);
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
    /* v0.4.296: cap argv layout = [0]=main EP, [1]=pipe_server EP (PTY SIGINT;
     * always present), [2]=display_server EP (HDMI mirror; optional). */
    if (argc > 0) ep = (seL4_CPtr)parse_num(argv[0]);
    if (argc > 1) pipe_ep = (seL4_CPtr)parse_num(argv[1]);
    if (argc > 2) disp_ep = (seL4_CPtr)parse_num(argv[2]);   /* HDMI fb_console mirror */

    /* Instance 0 = the serial/keyboard console (is_pty=0 -> output to UART+HDMI). */
    tty_inst_init(&tty_inst[0]);
    tty_inst[0].used = 1;
    tty_inst_t *con = &tty_inst[0];

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
            int c = key_pop(con);
            seL4_SetMR(0, (seL4_Word)c);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }

        case SER_PUTS: {
            /* Copy chars OUT of the MRs before output -- disp_write clobbers them. */
            char sbuf[128];
            int n = (int)len; if (n > 128) n = 128;
            for (int i = 0; i < n; i++) sbuf[i] = (char)seL4_GetMR(i);
            tty_write_buf(sbuf, n);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;
        }

        /* v0.4.203: reply BEFORE the line discipline. The caller is the USB
         * driver (or the root UART RX path); holding it through the echo chain
         * (polled-UART putchar + a DISP_CONSOLE Call into the display server)
         * stalls the driver thread per keystroke, drains its interrupt ring
         * under load, and drops keys at fast typing rates. The echo still runs
         * here, after the caller is released; per-caller order is preserved by
         * the endpoint. */
        case SER_KEY_PUSH:
        /* -- New TTY protocol -- */
        case TTY_INPUT: {
            char in_c = (char)seL4_GetMR(0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            line_discipline(con, in_c);
            break;
        }

        case TTY_WRITE: {
            /* Copy the payload OUT of the MRs before output -- disp_write reuses the
             * MRs for the HDMI mirror, so reading them mid-loop would corrupt output. */
            int wlen = (int)seL4_GetMR(0);
            char wbuf[1024];
            if (wlen > 1024) wlen = 1024;
            for (int i = 0; i < wlen; i++)
                wbuf[i] = (char)((seL4_GetMR(1 + i / 8) >> ((i % 8) * 8)) & 0xFF);
            tty_write_buf(wbuf, wlen);
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

            if (con->mode == TTY_MODE_RAW) {
                /* Raw: return whatever is in key_buf */
                int avail = key_count(con);
                rlen = avail < max_len ? avail : max_len;
                seL4_SetMR(0, (seL4_Word)rlen);
                if (rlen > 0) {
                    int mr = 1;
                    seL4_Word w = 0;
                    for (int i = 0; i < rlen; i++) {
                        int c = key_pop(con);
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
                int avail = lq_avail(con);
                rlen = avail < max_len ? avail : max_len;
                seL4_SetMR(0, (seL4_Word)rlen);
                if (rlen > 0) {
                    int mr = 1;
                    seL4_Word w = 0;
                    for (int i = 0; i < rlen; i++) {
                        int c = lq_pop(con);
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
            if (con->mode == TTY_MODE_RAW) {
                avail = key_count(con);
            } else {
                avail = lq_avail(con);
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
                con->lflag &= ~T_ICANON;
                termios_sync(con);
                con->lq_head = con->lq_tail = 0;
                con->key_head = con->key_tail = 0;
                con->line_len = 0;
                break;
            case TTY_IOCTL_SET_COOKED:
                con->lflag |= T_ICANON;
                termios_sync(con);
                con->key_head = con->key_tail = 0;
                con->line_len = 0;
                break;
            case TTY_IOCTL_ECHO_ON:
                con->lflag |= T_ECHO;
                termios_sync(con);
                break;
            case TTY_IOCTL_ECHO_OFF:
                con->lflag &= ~T_ECHO;
                termios_sync(con);
                break;
            case TTY_IOCTL_GET_MODE:
                result = con->mode;
                break;
            case TTY_IOCTL_TCGETS:
                /* v0.4.99: return full termios via MRs */
                termios_pack_reply(con);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 7));
                goto next_msg;
            case TTY_IOCTL_TCSETS:
            case TTY_IOCTL_TCSETSW:
            case TTY_IOCTL_TCSETSF:
                /* v0.4.99: receive full termios via MRs */
                termios_unpack(con);
                if (op == TTY_IOCTL_TCSETSF) {
                    /* Flush input */
                    con->lq_head = con->lq_tail = 0;
                    con->key_head = con->key_tail = 0;
                    con->line_len = 0;
                }
                break;
            }
            seL4_SetMR(0, (seL4_Word)result);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }

        /* -- PTY master-side ops (v0.4.295 step 2); instance id in MR0 -- */
        case TTY_PTY_ALLOC: {
            int id = -1;
            for (int i = 1; i < MAX_TTY; i++)
                if (!tty_inst[i].used) { id = i; break; }
            if (id >= 0) {
                tty_inst_init(&tty_inst[id]);
                tty_inst[id].used = 1;
                tty_inst[id].is_pty = 1;
                tty_inst[id].mo_head = tty_inst[id].mo_tail = 0;
                tty_inst[id].ws_row = 24; tty_inst[id].ws_col = 80;
            }
            seL4_SetMR(0, (seL4_Word)id);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }

        case TTY_PTY_INPUT: {
            int id = (int)seL4_GetMR(0);
            int ilen = (int)seL4_GetMR(1);
            char ibuf[1024];
            if (ilen > 1024) ilen = 1024;
            for (int i = 0; i < ilen; i++)
                ibuf[i] = (char)((seL4_GetMR(2 + i / 8) >> ((i % 8) * 8)) & 0xFF);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            if (id > 0 && id < MAX_TTY && tty_inst[id].used && tty_inst[id].is_pty)
                for (int i = 0; i < ilen; i++) line_discipline(&tty_inst[id], ibuf[i]);
            break;
        }

        case TTY_PTY_MASTER_READ: {
            int id = (int)seL4_GetMR(0);
            int max = (int)seL4_GetMR(1);
            int max_mr_bytes = ((int)seL4_MsgMaxLength - 1) * 8;
            if (max > max_mr_bytes) max = max_mr_bytes;
            int rlen = 0, mr = 1;
            if (id > 0 && id < MAX_TTY && tty_inst[id].used && tty_inst[id].is_pty) {
                int avail = mo_avail(&tty_inst[id]);
                rlen = avail < max ? avail : max;
                seL4_Word w = 0;
                for (int i = 0; i < rlen; i++) {
                    int c = mo_pop(&tty_inst[id]);
                    if (c < 0) { rlen = i; break; }
                    w |= ((seL4_Word)(uint8_t)c) << ((i % 8) * 8);
                    if (i % 8 == 7 || i == rlen - 1) { seL4_SetMR(mr++, w); w = 0; }
                }
            }
            seL4_SetMR(0, (seL4_Word)rlen);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, mr));
            break;
        }

        case TTY_PTY_WINSZ: {
            int id = (int)seL4_GetMR(0);
            if (id > 0 && id < MAX_TTY && tty_inst[id].used) {
                tty_inst[id].ws_row = (uint16_t)seL4_GetMR(1);
                tty_inst[id].ws_col = (uint16_t)seL4_GetMR(2);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;
        }

        case TTY_PTY_FREE: {
            int id = (int)seL4_GetMR(0);
            if (id > 0 && id < MAX_TTY) { tty_inst[id].used = 0; tty_inst[id].is_pty = 0; }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            break;
        }

        /* -- PTY slave-side ops (v0.4.296 step 2b); instance id in MR0. The shell's
         * libc routes fd 0/1/2 here when its controlling PTY is instance>0. These
         * mirror the serial-console TTY_READ/WRITE/POLL/IOCTL but on instance N. -- */
        case TTY_PTY_SLAVE_READ: {
            int id = (int)seL4_GetMR(0);
            int max_len = (int)seL4_GetMR(1);
            int max_mr_bytes = ((int)seL4_MsgMaxLength - 1) * 8;
            if (max_len > max_mr_bytes) max_len = max_mr_bytes;
            int rlen = 0, mr = 1;
            if (id > 0 && id < MAX_TTY && tty_inst[id].used && tty_inst[id].is_pty) {
                tty_inst_t *t = &tty_inst[id];
                int raw = (t->mode == TTY_MODE_RAW);
                int avail = raw ? key_count(t) : lq_avail(t);
                rlen = avail < max_len ? avail : max_len;
                seL4_Word w = 0;
                for (int i = 0; i < rlen; i++) {
                    int c = raw ? key_pop(t) : lq_pop(t);
                    if (c < 0) { rlen = i; break; }
                    w |= ((seL4_Word)(uint8_t)c) << ((i % 8) * 8);
                    if (i % 8 == 7 || i == rlen - 1) { seL4_SetMR(mr++, w); w = 0; }
                }
            }
            seL4_SetMR(0, (seL4_Word)rlen);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, mr));
            break;
        }

        case TTY_PTY_SLAVE_WRITE: {
            int id = (int)seL4_GetMR(0);
            int wlen = (int)seL4_GetMR(1);
            char wbuf[1024];
            if (wlen > 1024) wlen = 1024;
            for (int i = 0; i < wlen; i++)
                wbuf[i] = (char)((seL4_GetMR(2 + i / 8) >> ((i % 8) * 8)) & 0xFF);
            if (id > 0 && id < MAX_TTY && tty_inst[id].used && tty_inst[id].is_pty)
                inst_out(&tty_inst[id], wbuf, wlen);
            seL4_SetMR(0, (seL4_Word)wlen);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }

        case TTY_PTY_SLAVE_POLL: {
            int id = (int)seL4_GetMR(0);
            int avail = 0;
            if (id > 0 && id < MAX_TTY && tty_inst[id].used && tty_inst[id].is_pty) {
                tty_inst_t *t = &tty_inst[id];
                avail = (t->mode == TTY_MODE_RAW) ? key_count(t) : lq_avail(t);
            }
            seL4_SetMR(0, (seL4_Word)avail);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            break;
        }

        case TTY_PTY_SLAVE_IOCTL: {
            int id = (int)seL4_GetMR(0);
            int op = (int)seL4_GetMR(1);
            if (!(id > 0 && id < MAX_TTY && tty_inst[id].used && tty_inst[id].is_pty)) {
                seL4_SetMR(0, (seL4_Word)0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            tty_inst_t *t = &tty_inst[id];
            int result = 0;
            switch (op) {
            case TTY_IOCTL_SET_RAW:
                t->lflag &= ~T_ICANON; termios_sync(t);
                t->lq_head = t->lq_tail = 0; t->key_head = t->key_tail = 0; t->line_len = 0;
                break;
            case TTY_IOCTL_SET_COOKED:
                t->lflag |= T_ICANON; termios_sync(t);
                t->key_head = t->key_tail = 0; t->line_len = 0;
                break;
            case TTY_IOCTL_ECHO_ON:  t->lflag |= T_ECHO;  termios_sync(t); break;
            case TTY_IOCTL_ECHO_OFF: t->lflag &= ~T_ECHO; termios_sync(t); break;
            case TTY_IOCTL_GET_MODE: result = t->mode; break;
            case TTY_IOCTL_GET_WINSZ:
                seL4_SetMR(0, (seL4_Word)t->ws_row);
                seL4_SetMR(1, (seL4_Word)t->ws_col);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
                goto next_msg;
            case TTY_IOCTL_TCGETS:
                /* termios MRs start at MR0 (no instance shift on the reply) */
                termios_pack_reply(t);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 7));
                goto next_msg;
            case TTY_IOCTL_TCSETS:
            case TTY_IOCTL_TCSETSW:
            case TTY_IOCTL_TCSETSF: {
                /* args shifted by one MR vs TTY_IOCTL (MR0=inst, MR1=op),
                 * so termios words are MR2..MR5 and cc packs at MR6..MR8. */
                t->iflag = (uint32_t)seL4_GetMR(2);
                t->oflag = (uint32_t)seL4_GetMR(3);
                t->cflag = (uint32_t)seL4_GetMR(4);
                t->lflag = (uint32_t)seL4_GetMR(5);
                for (int m = 0; m < 3; m++) {
                    seL4_Word w = seL4_GetMR(6 + m);
                    for (int b = 0; b < 8 && (m * 8 + b) < 20; b++)
                        t->cc[m * 8 + b] = (uint8_t)((w >> (b * 8)) & 0xFF);
                }
                termios_sync(t);
                if (op == TTY_IOCTL_TCSETSF) {
                    t->lq_head = t->lq_tail = 0; t->key_head = t->key_tail = 0; t->line_len = 0;
                }
                break;
            }
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
