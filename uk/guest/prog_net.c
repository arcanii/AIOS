/* prog_net.c -- an AIOS network CLIENT (system layer: networking, increment 1). It opens a TCP socket,
 * connects to <host> <port> (argv), sends a message, reads the reply, and verifies the round-trip. The
 * socket is an ordinary AIOS fd, so write()/read()/close() work on it. Driven by test/net_client.c (a
 * host server that echoes) so the whole thing is self-contained -- no external network. An AIOS-ABI
 * program (libaios); the kernel services socket/connect via the PAL (host-passthrough).
 *
 * Exit 0 iff the reply matches what we sent. */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int
main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <host> <port>\n", argv[0]); return 2; }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("prog_net: socket"); return 1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((unsigned short)atoi(argv[2]));
    sa.sin_addr.s_addr = inet_addr(argv[1]);

    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("prog_net: connect"); return 1; }

    const char *msg = "AIOS-NET-PING";
    if (write(fd, msg, strlen(msg)) < 0) { perror("prog_net: write"); return 1; }

    char buf[256];
    int n = (int)read(fd, buf, sizeof buf - 1);
    if (n < 0) { perror("prog_net: read"); return 1; }
    buf[n] = '\0';
    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';

    int ok = (strcmp(buf, msg) == 0);
    printf("prog_net: connected %s:%s, sent \"%s\", received \"%s\" -> %s\n",
           argv[1], argv[2], msg, buf, ok ? "round-trip OK" : "MISMATCH");
    close(fd);
    return ok ? 0 : 1;
}
