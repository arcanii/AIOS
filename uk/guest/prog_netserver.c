/* prog_netserver.c -- an AIOS network SERVER (system layer: networking, increment 2). It opens a TCP
 * socket, sets SO_REUSEADDR, binds 127.0.0.1:0 (an ephemeral port), listens, learns its port via
 * getsockname(), prints "PORT <n>" so the driver can connect, accepts ONE connection, echoes the one
 * message it receives, and exits 0 iff it echoed something. The listening + accepted sockets are
 * ordinary AIOS fds, so bind/listen/accept and read/write/close all go through the AIOS kernel. Driven
 * by test/net_server.c (a host client that reads the port, connects, and checks the echo) so the whole
 * thing is self-contained -- no external network. An AIOS-ABI program (libaios); the kernel services
 * the socket calls via the PAL (host-passthrough).
 *
 * Exit 0 iff a connection arrived and its message was echoed back. */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

int
main(void)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("netserver: socket"); return 1; }

    int one = 1;
    if (setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0) {
        perror("netserver: setsockopt"); return 1;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0;                       /* ephemeral -- the kernel picks the port */

    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("netserver: bind"); return 1; }
    if (listen(srv, 1) != 0)                               { perror("netserver: listen"); return 1; }

    struct sockaddr_in bound;
    socklen_t bl = sizeof bound;
    if (getsockname(srv, (struct sockaddr *)&bound, &bl) != 0) { perror("netserver: getsockname"); return 1; }

    /* announce the port so the driver can connect (flush before the blocking accept) */
    printf("PORT %d\n", ntohs(bound.sin_port));
    fflush(stdout);

    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    int c = accept(srv, (struct sockaddr *)&peer, &pl);
    if (c < 0) { perror("netserver: accept"); return 1; }

    char buf[256];
    int n = (int)read(c, buf, sizeof buf);
    if (n > 0) {
        if (write(c, buf, (size_t)n) < 0) { perror("netserver: write"); n = -1; }
    }
    close(c);
    close(srv);

    unsigned int ip = ntohl(peer.sin_addr.s_addr);    /* dotted quad, no inet_ntoa in libaios */
    printf("netserver: accepted a connection from %u.%u.%u.%u:%d, echoed %d bytes\n",
           (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff,
           ntohs(peer.sin_port), n > 0 ? n : 0);
    return (n > 0) ? 0 : 1;
}
