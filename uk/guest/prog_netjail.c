/* prog_netjail.c -- the red-team guest for network-access confinement (inc 5). It attempts ONE connect
 * to <host> <port> and self-verifies against an expected outcome (argv[3]): "allow" -> connect() must
 * SUCCEED (== 0); "deny" -> connect() must FAIL with errno == EACCES (refused by the AIOS_NET_ALLOW
 * policy, specifically -- not merely any failure). Exit 0 iff the outcome matches. Driven by
 * test/net_jail.c, which sets AIOS_NET_ALLOW per case (including malformed rules that must FAIL CLOSED).
 * An AIOS-ABI program (libaios); the PAL enforces the allow-list in pal_host_connect. */

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
    if (argc < 4) { fprintf(stderr, "usage: %s <host> <port> allow|deny\n", argv[0]); return 2; }
    int want_deny = (strcmp(argv[3], "deny") == 0);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("netjail: socket"); return 2; }
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons((unsigned short)atoi(argv[2]));
    sa.sin_addr.s_addr = inet_addr(argv[1]);

    errno = 0;
    int r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    int e = errno;
    close(fd);

    int ok = want_deny ? (r != 0 && e == EACCES)      /* denied specifically by the policy (EACCES) */
                       : (r == 0);                     /* allowed -> connect succeeds */
    printf("netjail: connect %s:%s expect=%s -> r=%d errno=%d -> %s\n",
           argv[1], argv[2], argv[3], r, e, ok ? "ok" : "BAD");
    return ok ? 0 : 1;
}
