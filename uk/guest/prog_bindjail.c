/* prog_bindjail.c -- the red-team guest for BIND/LISTEN network-access confinement (finishing the net
 * arc). It performs ONE operation on a fresh TCP socket and self-verifies against an expected outcome
 * (argv[3]):
 *   op "bind"      : bind(host:port)               -- allow => bind() succeeds; deny => EACCES.
 *   op "listen"    : bind(host:port) then listen() -- allow => both succeed; deny => the failing call
 *                    returns EACCES (proves an ALLOWED bind + listen works end to end).
 *   op "listenraw" : listen() with NO bind         -- exercises the auto-bind guard: allow => listen()
 *                    succeeds (policy permits 0.0.0.0:0); deny => listen() refused with EACCES.
 * Exit 0 iff the outcome matches. Driven by test/bind_jail.c (which sets AIOS_NET_BIND_ALLOW per case,
 * including malformed rules that must FAIL CLOSED). An AIOS-ABI program (libaios); the PAL enforces the
 * allow-list in pal_host_bind / pal_host_listen. */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int
main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: %s <host> <port> allow|deny bind|listen|listenraw\n", argv[0]); return 2; }
    int want_deny = (strcmp(argv[3], "deny") == 0);
    const char *op = argv[4];

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("bindjail: socket"); return 2; }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons((unsigned short)atoi(argv[2]));
    sa.sin_addr.s_addr = inet_addr(argv[1]);

    errno = 0;
    int r;
    if (strcmp(op, "listenraw") == 0) {
        r = listen(fd, 1);                            /* no bind -> exercises the auto-bind guard */
    } else if (strcmp(op, "listen") == 0) {
        r = bind(fd, (struct sockaddr *)&sa, sizeof sa);
        if (r == 0) { errno = 0; r = listen(fd, 1); } /* bind allowed -> now the listen gate */
    } else {                                          /* "bind" */
        r = bind(fd, (struct sockaddr *)&sa, sizeof sa);
    }
    int e = errno;
    close(fd);

    int ok = want_deny ? (r != 0 && e == EACCES)      /* denied specifically by the policy (EACCES) */
                       : (r == 0);                     /* allowed -> the operation succeeds */
    printf("bindjail: op=%s %s:%s expect=%s -> r=%d errno=%d -> %s\n",
           op, argv[1], argv[2], argv[3], r, e, ok ? "ok" : "BAD");
    return ok ? 0 : 1;
}
