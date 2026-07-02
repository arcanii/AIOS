/* prog_netloop.c -- the increment-3 proof: TWO AIOS guests talk over TCP inside ONE aios-uk, which is
 * only possible because socket I/O no longer BLOCKS the single-threaded kernel (non-blocking + park/
 * wake). The parent forks a child SERVER (bind 127.0.0.1:0, listen, accept, echo) and acts as the
 * CLIENT (connect, send, recv). Under the OLD blocking model the child's accept() would freeze the
 * whole kernel before the parent ever ran -> deadlock; now the child PARKS on accept, the parent runs
 * and connects, and both make progress. The child hands its ephemeral port to the parent over an AIOS
 * pipe (which itself parks/wakes). Fully self-contained -- no host peer, no external network.
 *
 * Exit 0 iff the round-trip echo matches AND the child server exited 0. */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

/* the forked child: a one-shot TCP echo server. Returns 0 on a clean echo, else a small code. */
static int run_server(int port_w)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return 11;
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;                                  /* ephemeral */
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) != 0) return 12;
    if (listen(srv, 1) != 0) return 13;

    struct sockaddr_in b;
    socklen_t bl = sizeof b;
    if (getsockname(srv, (struct sockaddr *)&b, &bl) != 0) return 14;
    unsigned short port = ntohs(b.sin_port);
    if (write(port_w, &port, sizeof port) != (long)sizeof port) return 15;
    close(port_w);                                   /* parent now unblocks + connects */

    int c = accept(srv, (struct sockaddr *)0, (socklen_t *)0);   /* PARKS until the parent connects */
    if (c < 0) return 16;
    char buf[256];
    int n = (int)read(c, buf, sizeof buf);           /* PARKS until the parent sends */
    if (n > 0) (void)!write(c, buf, (size_t)n);      /* echo */
    close(c);
    close(srv);
    return (n > 0) ? 0 : 17;
}

int
main(void)
{
    int pp[2];
    if (pipe(pp) != 0) { perror("netloop: pipe"); return 1; }

    int pid = (int)fork();
    if (pid < 0) { perror("netloop: fork"); return 1; }
    if (pid == 0) { close(pp[0]); _exit(run_server(pp[1])); }

    /* parent = the client */
    close(pp[1]);
    unsigned short port = 0;
    if (read(pp[0], &port, sizeof port) != (long)sizeof port) { fprintf(stderr, "netloop: no port\n"); return 1; }
    close(pp[0]);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("netloop: socket"); return 1; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port);
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("netloop: connect"); return 1; }

    const char *msg = "AIOS-NETLOOP";
    if (write(s, msg, strlen(msg)) < 0) { perror("netloop: write"); return 1; }
    char buf[256];
    int n = (int)read(s, buf, sizeof buf - 1);
    if (n < 0) { perror("netloop: read"); return 1; }
    buf[n] = '\0';
    close(s);

    int st = 0;
    waitpid(pid, &st, 0);
    int server_ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    int ok = (strcmp(buf, msg) == 0) && server_ok;
    printf("prog_netloop: two AIOS guests round-tripped \"%s\" over TCP on port %d (server exit %d) -> %s\n",
           buf, (int)port, WIFEXITED(st) ? WEXITSTATUS(st) : -1, ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
