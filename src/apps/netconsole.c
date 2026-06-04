/* netconsole.c -- AIOS plaintext TCP remote command shell (LAN only)
 *
 * A bare, UNENCRYPTED, UNAUTHENTICATED remote command runner for driving a
 * Raspberry Pi 4 over a private LAN during development. It listens on a TCP
 * port, accepts one client at a time, and runs ONE command per input line:
 * read a line from the socket, run it via "dash -c <line>", stream the
 * command output back to the socket, then prompt for the next line. There is
 * NO crypto and NO login -- anyone who can reach the port gets to run root
 * commands. Use only on a trusted dev network. For anything else, use sshd.
 *
 * Why command-per-connection (v2) instead of an interactive "dash -i" relay:
 *   An interactive shell over a plain pipe pair has no tty / line discipline,
 *   so dash -i over a socket relay never flowed I/O on hardware (the v1 design).
 *   "dash -c <line>" RUNS then EXITS, which flushes its stdout and closes the
 *   pipe -- no tty, no buffering games. Each line is a fresh, self-contained
 *   command. The trade-off: shell state does NOT persist between lines (no
 *   carried cwd, env, or variables; a "cd" only affects that one line). Drive
 *   the Pi with absolute paths, e.g. "cat /proc/genet.ip", "ls /bin".
 *
 * EOF discipline (the crux -- AIOS pipe semantics, post-v0.4.143):
 *   In AIOS a pipe read end reports EOF only when the pipe server latches
 *   write_closed, and write_closed latches in exactly ONE place: the exit of
 *   the REGISTERED writer (the process whose stdout was bound to that pipe at
 *   PIPE_EXEC -- here, the dash we exec). A plain close() of an inherited
 *   write-end copy NEVER latches EOF (see pipe_server.c PIPE_CLOSE_WRITE). So
 *   the parent can drop ALL its pipe ends immediately after fork: the output
 *   read end still gets a clean EOF precisely when dash exits -- whether the
 *   command printed anything or not. (This is why v1 / ssh_channel.c deferred
 *   the parent stdout-write close until "first byte"; that dance was for the
 *   OLD pre-143 close-latches-EOF behaviour and is no longer needed.)
 *
 * Why an O_NONBLOCK poll on the output pipe instead of a blocking read:
 *   A blocking read would wedge the whole server if a command never exits --
 *   e.g. a bare "cat" or "read" that reads stdin. The child stdin is a pipe
 *   with no writer (we never forward stdin), so such a read blocks forever.
 *   We poll the output pipe non-blocking with a 10ms idle sleep and a per-
 *   command timeout; on timeout we SIGKILL the child and move on, keeping the
 *   server alive for the next command.
 *
 * Why a stdin PIPE for the child (not /dev/null, not "no redirect"):
 *   /dev/null is libc-level state and does not survive execv into dash. With
 *   NO stdin pipe, a process falls back to TTY_READ on fd 0 -- it would steal
 *   the serial console we use to launch and watch netconsole. A writer-less
 *   pipe keeps a stray stdin read isolated (it blocks on the pipe, bounded by
 *   the command timeout) instead of stealing the console.
 *
 * Build:  ./scripts/aios-cc src/apps/netconsole.c -o build-04/sbase/netconsole
 * Run:    netconsole &
 * Use:    nc <pi-ip> 2323      (from another host on the LAN; keep stdin open)
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>

#define NETCON_PORT     2323
#define NETCON_SHELL    "/bin/dash"
#define NETCON_LINE     1024              /* max command length            */
#define NETCON_BUF      1024              /* output read buffer            */
#define NETCON_IO       900               /* AIOS socket/pipe I/O cap      */
#define NETCON_POLL_NS  (10 * 1000 * 1000)        /* 10ms idle poll        */
#define NETCON_TIMEOUT_TICKS  3000        /* ~30s of idle -> kill child    */

#define NETCON_O_NONBLOCK  0x800          /* AIOS O_NONBLOCK bit           */

static const char NETCON_BANNER[] =
    "AIOS netconsole -- one command per line. Type exit to disconnect.\n";
static const char NETCON_PROMPT[] = "aios# ";

/* Write all len bytes to fd, looping over short writes. Returns 0 on
 * success, -1 if the peer went away (so the caller can drop the client). */
static int write_all(int fd, const char *p, int len)
{
    int off = 0;
    while (off < len) {
        int w = (int)write(fd, p + off, len - off);
        if (w <= 0)
            return -1;
        off += w;
    }
    return 0;
}

/* Read one line (up to '\n') from the socket, blocking. Stores up to
 * max-1 bytes plus a NUL terminator in line, swallowing a trailing CR so
 * CRLF clients (telnet, some nc builds) are handled. Bytes past max-1 are
 * dropped but the line is still drained to its newline. Returns the line
 * length (>= 0), or -1 if the client closed the connection or errored. */
static int read_line(int cfd, char *line, int max)
{
    int len = 0;
    for (;;) {
        char c;
        int n = (int)read(cfd, &c, 1);
        if (n <= 0)
            return -1;                    /* client closed or error        */
        if (c == '\n')
            break;
        if (c == '\r')
            continue;                     /* swallow CR (CRLF clients)      */
        if (len < max - 1)
            line[len++] = c;
        /* else: overflow -- keep draining to the newline, drop the excess */
    }
    line[len] = 0;
    return len;
}

/* Run a single command line via "dash -c <line>", streaming its stdout and
 * stderr back to the socket until the shell exits. Sets *client_gone if the
 * socket write fails (peer disconnected) so the caller stops the session. */
static void run_command(int cfd, const char *line, int *client_gone)
{
    int in_pipe[2], out_pipe[2];

    if (pipe2(in_pipe, 0) < 0) {
        write_all(cfd, "[netcon: stdin pipe failed]\n", 28);
        return;
    }
    if (pipe2(out_pipe, 0) < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        write_all(cfd, "[netcon: stdout pipe failed]\n", 29);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        write_all(cfd, "[netcon: fork failed]\n", 22);
        return;
    }

    if (pid == 0) {
        /* Child: stdin <- in_pipe read end, stdout/stderr -> out_pipe write
         * end, then exec dash -c. Do NOT close any pipe end here: in AIOS a
         * close() on a pipe end notifies the pipe server, and exec replaces
         * the process image via PIPE_EXEC anyway. */
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        dup2(out_pipe[1], 2);
        char *argv[] = { (char *)"dash", (char *)"-c", (char *)line, (void *)0 };
        execv(NETCON_SHELL, argv);
        _exit(127);
    }

    /* Parent: drop every pipe end we do not read. Post-v0.4.143 none of these
     * closes latch EOF -- the output read end still reports EOF only when dash
     * (the registered writer) exits. We never feed the command stdin, so the
     * child stdin pipe stays writer-less (a stray stdin read blocks, bounded
     * by the timeout below). */
    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[1]);

    int out_rd = out_pipe[0];
    int fl = fcntl(out_rd, F_GETFL, 0);
    fcntl(out_rd, F_SETFL, fl | NETCON_O_NONBLOCK);

    char buf[NETCON_BUF];
    int idle = 0;
    int timed_out = 0;

    for (;;) {
        int n = (int)read(out_rd, buf, NETCON_IO);
        if (n > 0) {
            idle = 0;
            if (!*client_gone && write_all(cfd, buf, n) < 0)
                *client_gone = 1;
        } else if (n == 0) {
            break;                        /* dash exited -- authoritative EOF */
        } else {
            /* EAGAIN: command still running (or stuck reading stdin). */
            if (++idle > NETCON_TIMEOUT_TICKS) {
                kill(pid, 9);             /* SIGKILL a wedged child           */
                timed_out = 1;
                break;
            }
            struct timespec ts = { 0, NETCON_POLL_NS };
            nanosleep(&ts, (void *)0);
        }
    }

    close(out_rd);
    waitpid(pid, (void *)0, 0);           /* reap (already exited / killed)   */

    if (timed_out && !*client_gone)
        write_all(cfd, "\n[netcon: command timed out, killed]\n", 37);
}

/* Serve one connected client: prompt, read a command line, run it, repeat
 * until the client types exit/quit or drops the connection. */
static void serve_client(int cfd)
{
    int client_gone = 0;

    write_all(cfd, NETCON_BANNER, (int)sizeof(NETCON_BANNER) - 1);

    while (!client_gone) {
        if (write_all(cfd, NETCON_PROMPT, (int)sizeof(NETCON_PROMPT) - 1) < 0)
            break;

        char line[NETCON_LINE];
        int len = read_line(cfd, line, sizeof(line));
        if (len < 0)
            break;                        /* client closed                    */
        if (len == 0)
            continue;                     /* blank line -- re-prompt          */
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0)
            break;

        run_command(cfd, line, &client_gone);
    }

    close(cfd);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    signal(SIGINT, SIG_IGN);  /* a stray Ctrl-C must not kill the server */

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        printf("[netcon] socket() failed\n");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(NETCON_PORT);
    addr.sin_addr.s_addr = 0;

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[netcon] bind(%d) failed\n", NETCON_PORT);
        close(lfd);
        return 1;
    }

    if (listen(lfd, 1) < 0) {
        printf("[netcon] listen() failed\n");
        close(lfd);
        return 1;
    }

    printf("[netcon] listening on port %d (plaintext command shell, LAN only)\n",
           NETCON_PORT);

    for (;;) {
        int cfd = accept(lfd, (void *)0, (void *)0);
        if (cfd < 0) {
            printf("[netcon] accept() failed\n");
            continue;
        }
        printf("[netcon] client connected (fd %d)\n", cfd);
        serve_client(cfd);
        printf("[netcon] client disconnected\n");
    }

    close(lfd);
    return 0;
}
