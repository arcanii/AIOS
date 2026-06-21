/* netconsole.c -- AIOS plaintext TCP remote command shell (LAN only) -- v2
 *
 * A bare, UNENCRYPTED, UNAUTHENTICATED remote command runner for driving a
 * Raspberry Pi 4 over a private LAN during development. It listens on a TCP
 * port, accepts one client at a time, and runs ONE command per input line:
 * read a line from the socket, run it via "dash -c <line>", stream the
 * command output back, then prompt for the next line. There is NO crypto and
 * NO login -- anyone who can reach the port gets to run root commands. Use only
 * on a trusted dev network. For anything else, use sshd.
 *
 * --- v2 robustness (see docs/DESIGN_NETCONSOLE_V2.md) ---
 *   ALL client-socket I/O is NON-BLOCKING with per-operation deadlines, so no
 *   single client can wedge the server. v1's __put receive loop did a BLOCKING
 *   read and treated any short read as "client gone" with no timeout: a large
 *   or aborted push (the host stops mid-stream without a FIN) parked netconsole
 *   forever inside the kernel recv, killing port 2323 for EVERY future client.
 *   v2: the client socket is O_NONBLOCK; every read/write loops on EAGAIN with a
 *   10ms nap and a deadline. A stalled transfer aborts and DROPS that one
 *   connection (so leftover unframed bytes can never be run as a command); the
 *   accept loop survives for the next client. getty supervises + respawns
 *   netconsole if it ever dies (getty.c).
 *
 * Why command-per-connection instead of an interactive "dash -i" relay:
 *   An interactive shell over a plain pipe pair has no tty / line discipline,
 *   so dash -i over a socket relay never flowed I/O on hardware. "dash -c <line>"
 *   RUNS then EXITS, flushing stdout and closing the pipe -- no tty, no buffering
 *   games. Trade-off: shell state does NOT persist between lines (cwd, env, vars).
 *   Drive the Pi with absolute paths, e.g. "cat /proc/genet.ip", "ls /bin".
 *
 * EOF discipline (AIOS pipe semantics, post-v0.4.143): a pipe read end reports
 *   EOF only when the REGISTERED writer (the exec'd dash) exits, never on a
 *   plain close() of an inherited write-end copy. So the parent can drop ALL its
 *   pipe ends right after fork; the output read end still EOFs precisely when
 *   dash exits, whether the command printed anything or not.
 *
 * Why a writer-less stdin PIPE for the child (not /dev/null, not absent): a
 *   pipe with no writer keeps a stray stdin read isolated (it blocks on the
 *   pipe, bounded by the command timeout) instead of stealing the serial
 *   console (an absent fd 0 falls back to TTY_READ).
 *
 * Build:  ./scripts/aios-cc src/apps/netconsole.c -o build-04/sbase/netconsole
 * Use:    nc <pi-ip> 2323   (keep stdin open), or scripts/pi_filexfer.py
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>

#define NETCON_PORT     2323
#define NETCON_SHELL    "/bin/dash"
#define NETCON_LINE     1024              /* max command length            */
#define NETCON_BUF      1024              /* I/O buffer                    */
#define NETCON_IO       900               /* AIOS socket/pipe I/O cap      */
#define NETCON_POLL_NS  (10 * 1000 * 1000)        /* 10ms nap on EAGAIN    */
/* v0.4.264: settle between consecutive client connections. Rapid back-to-back
 * connects (each forks a dash -c) can hit an SMP/allocator race on the core-0
 * servers and wedge the whole box (2026-06-11 HW: ~5 instant connects = net +
 * HDMI + keyboard all dead). This paces the accept loop so per-connection
 * fork+teardown bursts are serialized. 200ms is FAR below the ~8s idle threshold
 * of the idle-teardown TLBI stall, so it cannot trigger that freeze; and it only
 * costs reconnect-per-command clients ~200ms/cmd -- a HELD-connection client
 * (scripts/aios_nc.py) pays it once at disconnect, i.e. never in practice. */
#define NETCON_ACCEPT_PACE_NS (200 * 1000 * 1000) /* 200ms between connections */
/* v0.4.290 mitigation polish (seed lead #5): a BLOCKING accept() returns <0 only on a
 * REAL error (it blocks while idle), so repeated failures mean the listening socket /
 * net-stack slot got corrupted under reconnect churn -- the "netconsole wedges under
 * churn, power-cycle to recover" mode. After this many CONSECUTIVE failures, rebuild the
 * listener (close + re-socket/bind/listen) to self-heal without a power-cycle; if the
 * rebuild also fails, exit so getty's supervisor respawns a fresh netconsole. This also
 * fixes a latent busy-spin: the old `if (cfd<0) continue;` looped with NO nap, pinning
 * core 0 at 100% (+ log spam) on a persistent accept error. */
#define NETCON_ACCEPT_FAIL_MAX  8

/* Per-operation deadlines, in 10ms ticks (reset on any progress). NOTE: AIOS
 * nanosleep granularity is ~10ms, so a sub-10ms nap rounds up and buys nothing;
 * the transfer speed lever is the 32KB rx ring (net_server.c), not the poll. */
#define TICKS_IDLE      12000             /* 120s waiting for next command  */
#define TICKS_STALL     1000              /* 10s no progress on a transfer  */
#define TICKS_WRITE     1000              /* 10s to flush a socket write    */
#define TICKS_CMD       3000              /* 30s command output timeout     */

#define NETCON_O_NONBLOCK  0x800          /* AIOS O_NONBLOCK bit           */

static const char NETCON_BANNER[] =
    "AIOS netconsole -- one command per line. Type exit to disconnect.\n";
static const char NETCON_PROMPT[] = "aios# ";

static void nap(void)
{
    struct timespec ts = { 0, NETCON_POLL_NS };
    nanosleep(&ts, (void *)0);
}

/* Non-blocking write-all with a flush deadline. The client socket is
 * O_NONBLOCK, so write() returns <0 (EAGAIN) when the send buffer is full;
 * we nap and retry, bounded by TICKS_WRITE. Returns 0 on success, -1 if the
 * peer went away or the flush stalled past the deadline. */
static int nb_write(int fd, const char *p, int len)
{
    int off = 0, idle = 0;
    while (off < len) {
        int w = (int)write(fd, p + off, len - off);
        if (w > 0) { off += w; idle = 0; }
        else if (w == 0) return -1;                 /* peer closed            */
        else { if (++idle > TICKS_WRITE) return -1; nap(); }   /* EAGAIN      */
    }
    return 0;
}

/* Non-blocking read of one line (up to '\n') with an idle deadline. Swallows a
 * trailing CR (CRLF clients). Bytes past max-1 are dropped but the line is
 * drained to its newline. Returns the line length (>=0), or -1 if the client
 * closed or went idle past TICKS_IDLE. */
static int nb_read_line(int cfd, char *line, int max)
{
    int len = 0, idle = 0;
    for (;;) {
        char c;
        int n = (int)read(cfd, &c, 1);
        if (n > 0) {
            idle = 0;
            if (c == '\n') break;
            if (c == '\r') continue;
            if (len < max - 1) line[len++] = c;
        } else if (n == 0) {
            return -1;                              /* client closed (FIN)    */
        } else {
            if (++idle > TICKS_IDLE) return -1;     /* idle too long          */
            nap();
        }
    }
    line[len] = 0;
    return len;
}

/* Run one command line via "dash -c <line>", streaming stdout+stderr back to
 * the socket until the shell exits. Sets *client_gone if a socket write fails.
 * The output pipe is polled O_NONBLOCK with a per-command timeout so a command
 * that never exits (e.g. one that reads its writer-less stdin) is SIGKILLed and
 * the server moves on. */
static void run_command(int cfd, const char *line, int *client_gone)
{
    int in_pipe[2], out_pipe[2];

    if (pipe2(in_pipe, 0) < 0) {
        nb_write(cfd, "[netcon: stdin pipe failed]\n", 28);
        return;
    }
    if (pipe2(out_pipe, 0) < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        nb_write(cfd, "[netcon: stdout pipe failed]\n", 29);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        nb_write(cfd, "[netcon: fork failed]\n", 22);
        return;
    }

    if (pid == 0) {
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        dup2(out_pipe[1], 2);
        char *argv[] = { (char *)"dash", (char *)"-c", (char *)line, (void *)0 };
        execv(NETCON_SHELL, argv);
        _exit(127);
    }

    /* Parent: drop every pipe end we do not read (none latch EOF post-v0.4.143). */
    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[1]);

    int out_rd = out_pipe[0];
    int fl = fcntl(out_rd, F_GETFL, 0);
    fcntl(out_rd, F_SETFL, fl | NETCON_O_NONBLOCK);

    char buf[NETCON_BUF];
    int idle = 0, timed_out = 0;

    for (;;) {
        int n = (int)read(out_rd, buf, NETCON_IO);
        if (n > 0) {
            idle = 0;
            if (!*client_gone && nb_write(cfd, buf, n) < 0)
                *client_gone = 1;
        } else if (n == 0) {
            break;                          /* dash exited -- authoritative EOF */
        } else {
            if (++idle > TICKS_CMD) {       /* EAGAIN: still running / stuck    */
                kill(pid, 9);
                timed_out = 1;
                break;
            }
            nap();
        }
    }

    close(out_rd);
    waitpid(pid, (void *)0, 0);

    if (timed_out && !*client_gone)
        nb_write(cfd, "\n[netcon: command timed out, killed]\n", 37);
}

/* Append a decimal number to msg at offset m, return the new offset. */
static int append_num(char *msg, int m, long v)
{
    char num[24];
    int ni = 0;
    if (v == 0) num[ni++] = '0';
    while (v > 0) { num[ni++] = (char)('0' + (v % 10)); v /= 10; }
    while (ni > 0) msg[m++] = num[--ni];
    return m;
}

/* Receive a file: "__put <path> <len>" then exactly <len> raw bytes. v2: the
 * socket is O_NONBLOCK, so the receive loop naps on EAGAIN with a STALL
 * deadline -- a host that stops mid-stream (the v1 wedge) no longer parks the
 * server; the transfer aborts and the connection is DROPPED (so the unconsumed
 * tail can never be misread as a command). Replies "__put ok <len>" on success,
 * "__put err <reason>" otherwise. Integrity is verified host-side via sha256. */
static void handle_put(int cfd, char *args, int *client_gone)
{
    char *sp = args;
    while (*sp && *sp != ' ') sp++;
    long len = 0;
    int have_len = 0;
    if (*sp == ' ') {
        *sp = 0;
        for (char *p = sp + 1; *p >= '0' && *p <= '9'; p++) {
            len = len * 10 + (*p - '0');
            have_len = 1;
        }
    }
    if (!have_len || len < 0) {
        nb_write(cfd, "__put err badargs\n", 18);
        return;
    }

    int fd = open(args, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char buf[NETCON_BUF];
    long remaining = len;
    int werr = 0, idle = 0, stalled = 0;

    while (remaining > 0) {
        int want = remaining < NETCON_IO ? (int)remaining : NETCON_IO;
        int n = (int)read(cfd, buf, want);
        if (n > 0) {
            idle = 0;
            if (fd >= 0 && !werr) {
                int off = 0;
                while (off < n) {
                    int w = (int)write(fd, buf + off, n - off);
                    if (w <= 0) { werr = 1; break; }
                    off += w;
                }
            }
            remaining -= n;
        } else if (n == 0) {
            *client_gone = 1; break;        /* peer closed mid-transfer        */
        } else {
            if (++idle > TICKS_STALL) { stalled = 1; break; }   /* EAGAIN stall */
            nap();
        }
    }
    if (fd >= 0) close(fd);
    if (*client_gone) return;

    if (stalled) {
        /* Framing is now desynced (the declared length was not fully consumed):
         * report and DROP the connection rather than risk running the tail. */
        nb_write(cfd, "__put err timeout\n", 18);
        *client_gone = 1;
        return;
    }
    if (fd < 0)       nb_write(cfd, "__put err open\n", 15);
    else if (werr)    nb_write(cfd, "__put err write\n", 16);
    else {
        char msg[40];
        int m = 0;
        const char *ok = "__put ok ";
        while (ok[m]) { msg[m] = ok[m]; m++; }
        m = append_num(msg, m, len);
        msg[m++] = '\n';
        nb_write(cfd, msg, m);
    }
}

/* Send a file: "__get <path>". We stat for the size, then read the file in big
 * chunks and stream it straight to the socket (bypassing dash -c cat). v2: the
 * socket writes go through nb_write, which handles back-pressure (EAGAIN) with a
 * deadline, so a slow reader cannot wedge the server. Reply "__get ok <len>\n"
 * then exactly <len> raw bytes, or "__get err <reason>\n". */
static void handle_get(int cfd, const char *path, int *client_gone)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        nb_write(cfd, "__get err stat\n", 15);
        return;
    }
    long size = (long)st.st_size;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        nb_write(cfd, "__get err open\n", 15);
        return;
    }

    char hdr[40];
    int m = 0;
    const char *ok = "__get ok ";
    while (ok[m]) { hdr[m] = ok[m]; m++; }
    m = append_num(hdr, m, size);
    hdr[m++] = '\n';
    if (nb_write(cfd, hdr, m) < 0) { *client_gone = 1; close(fd); return; }

    char buf[NETCON_BUF];
    long remaining = size;
    while (remaining > 0) {
        int want = remaining < NETCON_IO ? (int)remaining : NETCON_IO;
        int n = (int)read(fd, buf, want);
        if (n <= 0) break;                  /* short/early EOF -- host sees it via len */
        if (nb_write(cfd, buf, n) < 0) { *client_gone = 1; break; }
        remaining -= n;
    }
    close(fd);
}

/* Serve one connected client until it disconnects, goes idle, or types exit. */
static void serve_client(int cfd)
{
    int client_gone = 0;

    /* Client socket is non-blocking for the whole session (v2). */
    int fl = fcntl(cfd, F_GETFL, 0);
    fcntl(cfd, F_SETFL, fl | NETCON_O_NONBLOCK);

    nb_write(cfd, NETCON_BANNER, (int)sizeof(NETCON_BANNER) - 1);

    while (!client_gone) {
        if (nb_write(cfd, NETCON_PROMPT, (int)sizeof(NETCON_PROMPT) - 1) < 0)
            break;

        char line[NETCON_LINE];
        int len = nb_read_line(cfd, line, sizeof(line));
        if (len < 0) break;                 /* closed or idle timeout           */
        if (len == 0) continue;             /* blank line -- re-prompt          */
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;
        if (strncmp(line, "__put ", 6) == 0) { handle_put(cfd, line + 6, &client_gone); continue; }
        if (strncmp(line, "__get ", 6) == 0) { handle_get(cfd, line + 6, &client_gone); continue; }

        run_command(cfd, line, &client_gone);
    }

    close(cfd);
}

/* Open (or re-open) the listening socket. Returns the fd, or -1. Factored out so the
 * accept loop can REBUILD it to self-heal a churn-corrupted listener (lead #5). */
static int open_listener(void)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { printf("[netcon] socket() failed\n"); return -1; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(NETCON_PORT);
    addr.sin_addr.s_addr = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[netcon] bind(%d) failed\n", NETCON_PORT);
        close(lfd); return -1;
    }
    if (listen(lfd, 1) < 0) {
        printf("[netcon] listen() failed\n");
        close(lfd); return -1;
    }
    return lfd;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    signal(SIGINT, SIG_IGN);  /* a stray Ctrl-C must not kill the server */

    int lfd = open_listener();
    if (lfd < 0) { printf("[netcon] initial listener open failed\n"); return 1; }

    /* v0.4.253-fix: the startup banner was REMOVED. netconsole is getty-spawned
     * with fd1=tty (its relay REQUIRES fd1=tty -- see netconsole-redirect-fd-bug,
     * so we cannot redirect it), and this printf raced the getty "AIOS login:" on
     * the shared HDMI fb_console, producing a garbled interleaved line. A working
     * netconsole is self-evident from connections; no startup banner is needed. */

    /* Listening socket stays BLOCKING: accept() blocks while idle (no busy
     * spin); only the accepted client socket is made non-blocking. */
    int accept_fails = 0;
    for (;;) {
        int cfd = accept(lfd, (void *)0, (void *)0);
        if (cfd < 0) {
            /* lead #5 self-recovery: a BLOCKING accept fails only on a real listener/
             * net-stack error (it blocks while idle, never returns <0 for "no client").
             * Nap first (fixes the old busy-spin: the previous `continue` looped with no
             * sleep, pinning core 0 at 100% on a persistent error). After N consecutive
             * failures the listening socket is corrupted (churn wedge) -> rebuild it; if
             * the rebuild also fails, exit so getty's supervisor respawns netconsole. */
            struct timespec nap = { 0, NETCON_POLL_NS };
            nanosleep(&nap, (void *)0);
            if (++accept_fails >= NETCON_ACCEPT_FAIL_MAX) {
                printf("[netcon] %d consecutive accept() failures -- rebuilding listener\n", accept_fails);
                close(lfd);
                struct timespec settle = { 0, NETCON_ACCEPT_PACE_NS };
                nanosleep(&settle, (void *)0);     /* let the net-stack slot/cap free */
                lfd = open_listener();
                if (lfd < 0) {
                    printf("[netcon] listener rebuild failed -- exiting (getty respawns)\n");
                    return 1;
                }
                accept_fails = 0;
            }
            continue;
        }
        accept_fails = 0;                          /* a good accept clears the streak */
        serve_client(cfd);
        /* v0.4.264: pace consecutive connections (see NETCON_ACCEPT_PACE_NS) so a
         * rapid-reconnect storm cannot wedge the box. The serve above already
         * reaped its dash (waitpid); this lets the net_server socket free + the
         * VKA watermark refill settle before the next accept. */
        struct timespec pace = { 0, NETCON_ACCEPT_PACE_NS };
        nanosleep(&pace, (void *)0);
    }

    close(lfd);
    return 0;
}
