/* netconsole.c -- AIOS plaintext TCP remote command shell (LAN only) -- v2
 *
 * A bare, UNENCRYPTED, UNAUTHENTICATED remote command runner for driving a
 * Raspberry Pi 4 over a private LAN during development. NO crypto, NO login --
 * anyone who can reach the port runs root commands. Trusted dev network only.
 *
 * --- v2: non-blocking MULTI-SESSION event loop (docs/DESIGN_NETCONSOLE_V2.md) ---
 *   ONE process, ONE thread, NO blocking socket calls anywhere. The listener and
 *   every client socket are O_NONBLOCK; a small array of session structs each run
 *   a state machine (PROMPT / READLINE / RUNCMD / PUT / GET) with a per-operation
 *   deadline reset on progress. The loop polls accept() (now EAGAIN-capable, see
 *   net_server v0.4.173) and steps every active session a bounded amount, so:
 *     - no client can wedge or stall another (the v1 single-client model dropped
 *       backlogged SYNs while serving one client -> "connect times out" on HW);
 *     - reconnects and concurrent sessions just work;
 *     - a silent/stalled peer fails to reset its deadline and is reaped by the
 *       loop itself (no unbounded blocking read exists to hang on).
 *   getty supervises + respawns netconsole if it ever dies (getty.c).
 *
 * Protocol (unchanged from v1 so scripts/pi_filexfer.py works as-is): per line,
 *   run "dash -c <line>" and stream stdout+stderr back, then re-prompt "aios# ".
 *   "__put <path> <len>" then <len> raw bytes writes a file; "__get <path>"
 *   replies "__get ok <len>\n" then streams <len> raw bytes. "exit"/"quit" close.
 *   Command-per-line: shell state does NOT persist; use absolute paths.
 *
 * EOF discipline (post-v0.4.143): a pipe read end EOFs only when the REGISTERED
 *   writer (the exec'd dash) exits, so the parent drops all its pipe ends right
 *   after fork and still gets a precise EOF when dash exits, output or not.
 *
 * Build:  ./scripts/aios-cc src/apps/netconsole.c -o build-04/sbase/netconsole
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>

#define NETCON_PORT       2323
#define NETCON_SHELL      "/bin/dash"
#define NETCON_LINE       1024            /* max command length            */
#define NETCON_IO         900             /* AIOS socket/pipe MR I/O cap   */
#define NETCON_O_NONBLOCK 0x800           /* AIOS O_NONBLOCK bit           */
#define MAX_SESSIONS      6               /* 8 net sockets - listener - margin */
#define IDLE_NAP_NS       (5 * 1000 * 1000)   /* 5ms idle nap (only when no work) */

/* Per-operation deadlines in milliseconds, reset on any progress. */
#define MS_IDLE   120000   /* waiting for the next command line / slow flush */
#define MS_CMD     30000   /* command output                                 */
#define MS_XFER    30000   /* file transfer stall                            */

static const char NETCON_BANNER[] =
    "AIOS netconsole -- one command per line. Type exit to disconnect.\n";
static const char NETCON_PROMPT[] = "aios# ";

typedef enum { S_FREE = 0, S_PROMPT, S_READLINE, S_RUNCMD, S_PUT, S_GET } sstate_t;

typedef struct {
    sstate_t state;
    int      cfd;            /* client socket (O_NONBLOCK); -1 if free        */
    uint64_t deadline;       /* now_ms() deadline; reset on progress          */
    /* pending socket write (backpressure): produce new output only when empty */
    char     wbuf[NETCON_IO];
    int      wlen, woff;
    /* command line accumulation */
    char     line[NETCON_LINE];
    int      line_len;
    /* running command */
    int      pid;            /* forked dash, 0 if none                        */
    int      out_rd;         /* child output pipe (O_NONBLOCK), -1 if none    */
    /* file transfer */
    int      file_fd;        /* -1 if none                                    */
    long     remaining;      /* bytes left to transfer                        */
    long     xfer_total;     /* declared __put length (for the ok reply)      */
} session_t;

static session_t sess[MAX_SESSIONS];
static char      io_buf[NETCON_IO];   /* scratch for __put socket->file copy  */
static int       g_lfd = -1;          /* listener, so forked children can close it */

/* Monotonic milliseconds from the ARM generic timer (EL0-readable), same source
 * getty uses. Independent of libc clock support. */
static uint64_t now_ms(void)
{
    uint64_t freq, cnt;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    if (!freq) return 0;
    return (cnt * 1000ull) / freq;
}

static void nap(void)
{
    struct timespec ts = { 0, IDLE_NAP_NS };
    nanosleep(&ts, (void *)0);
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | NETCON_O_NONBLOCK);
}

static int append_num(char *msg, int m, long v)
{
    char num[24];
    int ni = 0;
    if (v == 0) num[ni++] = '0';
    while (v > 0) { num[ni++] = (char)('0' + (v % 10)); v /= 10; }
    while (ni > 0) msg[m++] = num[--ni];
    return m;
}

/* --- session lifecycle --- */

static void session_close(session_t *s)
{
    if (s->pid > 0) { kill(s->pid, 9); waitpid(s->pid, (void *)0, 0); s->pid = 0; }
    if (s->out_rd >= 0) { close(s->out_rd); s->out_rd = -1; }
    if (s->file_fd >= 0) { close(s->file_fd); s->file_fd = -1; }
    if (s->cfd >= 0) { close(s->cfd); s->cfd = -1; }
    s->state = S_FREE;
    s->wlen = s->woff = 0;
    s->line_len = 0;
    s->remaining = s->xfer_total = 0;
}

static int session_open(int cfd)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sess[i].state == S_FREE) {
            session_t *s = &sess[i];
            memset(s, 0, sizeof(*s));
            s->cfd = cfd; s->out_rd = -1; s->file_fd = -1; s->pid = 0;
            /* queue the banner; S_PROMPT appends the first prompt */
            int n = (int)sizeof(NETCON_BANNER) - 1;
            memcpy(s->wbuf, NETCON_BANNER, n);
            s->wlen = n; s->woff = 0;
            s->state = S_PROMPT;
            s->deadline = now_ms() + MS_IDLE;
            return 1;
        }
    }
    return 0;   /* table full */
}

/* Drain pending wbuf to the socket, non-blocking. Returns -1 if the peer is
 * gone, else 0 (sess_pending() may still be true on EAGAIN). */
static int sess_flush(session_t *s)
{
    while (s->woff < s->wlen) {
        int w = (int)write(s->cfd, s->wbuf + s->woff, s->wlen - s->woff);
        if (w > 0) s->woff += w;
        else if (w == 0) return -1;            /* peer closed */
        else break;                            /* EAGAIN: retry next step */
    }
    if (s->woff >= s->wlen) { s->wlen = s->woff = 0; }
    return 0;
}
static int sess_pending(session_t *s) { return s->wlen > s->woff; }

/* Queue a short message (<= NETCON_IO) and try to send it. Precondition: wbuf
 * empty (callers only queue at state transitions, when it is). */
static int sess_queue(session_t *s, const char *p, int len)
{
    if (len > NETCON_IO) len = NETCON_IO;
    memcpy(s->wbuf, p, len);
    s->wlen = len; s->woff = 0;
    return sess_flush(s);
}

/* Fork "dash -c <line>"; parent keeps a non-blocking output read end. */
static void start_command(session_t *s)
{
    int in_pipe[2], out_pipe[2];
    if (pipe2(in_pipe, 0) < 0) {
        sess_queue(s, "[netcon: stdin pipe failed]\n", 28); s->state = S_PROMPT; return;
    }
    if (pipe2(out_pipe, 0) < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        sess_queue(s, "[netcon: stdout pipe failed]\n", 29); s->state = S_PROMPT; return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
        sess_queue(s, "[netcon: fork failed]\n", 22); s->state = S_PROMPT; return;
    }

    if (pid == 0) {
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        dup2(out_pipe[1], 2);
        /* Close everything the child must NOT inherit: the originals, the
         * listener, and every session socket / sibling output pipe. An
         * inherited pipe read-end would keep a sibling's command from EOF-ing. */
        close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
        if (g_lfd >= 0) close(g_lfd);
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (sess[i].cfd >= 0) close(sess[i].cfd);
            if (sess[i].out_rd >= 0) close(sess[i].out_rd);
        }
        char *argv[] = { (char *)"dash", (char *)"-c", s->line, (void *)0 };
        execv(NETCON_SHELL, argv);
        _exit(127);
    }

    /* parent: keep only the non-blocking output read end */
    close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[1]);
    s->out_rd = out_pipe[0];
    set_nonblock(s->out_rd);
    s->pid = pid;
    s->state = S_RUNCMD;
    s->deadline = now_ms() + MS_CMD;
}

/* Parse a completed command line and transition the session. */
static void dispatch_line(session_t *s)
{
    char *line = s->line;
    if (line[0] == 0) { s->state = S_PROMPT; return; }                 /* blank */
    if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) { session_close(s); return; }

    if (strncmp(line, "__put ", 6) == 0) {
        char *args = line + 6;
        char *sp = args;
        while (*sp && *sp != ' ') sp++;
        long len = 0; int have = 0;
        if (*sp == ' ') {
            *sp = 0;
            for (char *p = sp + 1; *p >= '0' && *p <= '9'; p++) { len = len * 10 + (*p - '0'); have = 1; }
        }
        if (!have || len < 0) { sess_queue(s, "__put err badargs\n", 18); s->state = S_PROMPT; return; }
        int fd = open(args, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { sess_queue(s, "__put err open\n", 15); s->state = S_PROMPT; return; }
        s->file_fd = fd; s->remaining = len; s->xfer_total = len;
        s->state = S_PUT; s->deadline = now_ms() + MS_XFER;
        return;
    }
    if (strncmp(line, "__get ", 6) == 0) {
        const char *path = line + 6;
        struct stat st;
        if (stat(path, &st) < 0) { sess_queue(s, "__get err stat\n", 15); s->state = S_PROMPT; return; }
        int fd = open(path, O_RDONLY);
        if (fd < 0) { sess_queue(s, "__get err open\n", 15); s->state = S_PROMPT; return; }
        char hdr[40]; int m = 0; const char *ok = "__get ok ";
        while (ok[m]) { hdr[m] = ok[m]; m++; }
        m = append_num(hdr, m, (long)st.st_size);
        hdr[m++] = '\n';
        s->file_fd = fd; s->remaining = (long)st.st_size;
        s->state = S_GET; s->deadline = now_ms() + MS_XFER;
        sess_queue(s, hdr, m);   /* header first; S_GET streams the bytes */
        return;
    }
    start_command(s);
}

/* Advance one session by a bounded amount. Returns 1 if it did any work. */
static int session_step(session_t *s)
{
    uint64_t now = now_ms();

    /* 1. Flush any pending socket write first (backpressure: do not produce
     * more output until the client drains what we already have). */
    if (sess_pending(s)) {
        if (sess_flush(s) < 0) { session_close(s); return 1; }
        if (sess_pending(s)) {
            if (now > s->deadline) { session_close(s); return 1; }  /* stuck client */
            return 0;
        }
        return 1;
    }

    switch (s->state) {
    case S_PROMPT:
        if (sess_queue(s, NETCON_PROMPT, (int)sizeof(NETCON_PROMPT) - 1) < 0) { session_close(s); return 1; }
        s->state = S_READLINE;
        s->deadline = now + MS_IDLE;
        return 1;

    case S_READLINE: {
        char c;
        int n = (int)read(s->cfd, &c, 1);
        if (n > 0) {
            s->deadline = now + MS_IDLE;
            if (c == '\r') return 1;
            if (c == '\n') { s->line[s->line_len] = 0; s->line_len = 0; dispatch_line(s); return 1; }
            if (s->line_len < NETCON_LINE - 1) s->line[s->line_len++] = c;
            return 1;
        }
        if (n == 0) { session_close(s); return 1; }       /* peer FIN */
        if (now > s->deadline) { session_close(s); return 1; }  /* idle timeout */
        return 0;                                          /* EAGAIN */
    }

    case S_RUNCMD: {
        int n = (int)read(s->out_rd, s->wbuf, NETCON_IO);
        if (n > 0) {
            s->wlen = n; s->woff = 0;
            if (sess_flush(s) < 0) { session_close(s); return 1; }
            s->deadline = now + MS_CMD;
            return 1;
        }
        if (n == 0) {                                      /* dash exited: EOF */
            close(s->out_rd); s->out_rd = -1;
            waitpid(s->pid, (void *)0, 0); s->pid = 0;
            s->state = S_PROMPT;
            return 1;
        }
        if (now > s->deadline) {                           /* output timeout */
            kill(s->pid, 9); waitpid(s->pid, (void *)0, 0); s->pid = 0;
            close(s->out_rd); s->out_rd = -1;
            sess_queue(s, "\n[netcon: command timed out, killed]\n", 37);
            s->state = S_PROMPT;
            return 1;
        }
        return 0;                                          /* EAGAIN */
    }

    case S_PUT: {
        if (s->remaining <= 0) {                           /* done */
            close(s->file_fd); s->file_fd = -1;
            char msg[40]; int m = 0; const char *ok = "__put ok ";
            while (ok[m]) { msg[m] = ok[m]; m++; }
            m = append_num(msg, m, s->xfer_total);
            msg[m++] = '\n';
            sess_queue(s, msg, m);
            s->state = S_PROMPT;
            return 1;
        }
        int want = s->remaining < NETCON_IO ? (int)s->remaining : NETCON_IO;
        int n = (int)read(s->cfd, io_buf, want);
        if (n > 0) {
            int off = 0;
            while (off < n) { int w = (int)write(s->file_fd, io_buf + off, n - off); if (w <= 0) break; off += w; }
            s->remaining -= n;
            s->deadline = now + MS_XFER;
            return 1;
        }
        if (n == 0) { session_close(s); return 1; }        /* peer closed mid-xfer */
        if (now > s->deadline) {                           /* stall: drop (framing desync) */
            close(s->file_fd); s->file_fd = -1;
            session_close(s);
            return 1;
        }
        return 0;                                          /* EAGAIN */
    }

    case S_GET: {
        if (s->remaining <= 0) {                           /* done streaming */
            close(s->file_fd); s->file_fd = -1;
            s->state = S_PROMPT;
            return 1;
        }
        int want = s->remaining < NETCON_IO ? (int)s->remaining : NETCON_IO;
        int n = (int)read(s->file_fd, s->wbuf, want);
        if (n <= 0) { close(s->file_fd); s->file_fd = -1; s->state = S_PROMPT; return 1; }  /* short/EOF */
        s->wlen = n; s->woff = 0;
        if (sess_flush(s) < 0) { session_close(s); return 1; }
        s->remaining -= n;
        s->deadline = now + MS_XFER;
        return 1;
    }

    default:
        return 0;
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    signal(SIGINT, SIG_IGN);   /* a stray Ctrl-C must not kill the server */

    for (int i = 0; i < MAX_SESSIONS; i++) {
        sess[i].state = S_FREE; sess[i].cfd = -1; sess[i].out_rd = -1; sess[i].file_fd = -1;
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { printf("[netcon] socket() failed\n"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(NETCON_PORT);
    addr.sin_addr.s_addr = 0;

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[netcon] bind(%d) failed\n", NETCON_PORT); close(lfd); return 1;
    }
    if (listen(lfd, MAX_SESSIONS) < 0) {
        printf("[netcon] listen() failed\n"); close(lfd); return 1;
    }
    set_nonblock(lfd);   /* v0.4.173: non-blocking accept (net_server EAGAIN) */
    g_lfd = lfd;

    printf("[netcon] v2 listening on port %d (non-blocking multi-session, LAN only)\n",
           NETCON_PORT);

    for (;;) {
        int c = accept(lfd, (void *)0, (void *)0);
        if (c >= 0) {
            set_nonblock(c);
            if (!session_open(c)) close(c);    /* table full -> reject politely */
        }
        int did_work = 0;
        for (int i = 0; i < MAX_SESSIONS; i++)
            if (sess[i].state != S_FREE) did_work |= session_step(&sess[i]);
        if (c < 0 && !did_work) nap();         /* idle: avoid a hot spin */
    }

    close(lfd);
    return 0;
}
