/*
 * dns_server.c -- a HOST test driver + DNS stub for AIOS networking increment 4b (the resolver). It
 * binds a UDP socket on 127.0.0.1:<ephemeral>, points the AIOS resolver at it via $AIOS_DNS_SERVER,
 * launches the AIOS client (argv[1..] = e.g. `./aios-uk ./prog_dns`, to which it appends the name +
 * expected IP), answers the DNS A query with a fixed record, and PASSes iff it served a query AND the
 * AIOS resolver exited 0 (its own name==IP check). Self-contained -- no external network. HOST code; cc.
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <aios-uk> <prog_dns> [args...]\n", argv[0]); return 2; }

    const char *name   = "aios.test";
    const char *expect = "93.184.216.34";

    int srv = socket(AF_INET, SOCK_DGRAM, 0);
    if (srv < 0) { perror("socket"); return 2; }
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("bind"); return 2; }
    socklen_t sl = sizeof sa; getsockname(srv, (struct sockaddr *)&sa, &sl);
    int port = ntohs(sa.sin_port);

    char env[64]; snprintf(env, sizeof env, "AIOS_DNS_SERVER=127.0.0.1:%d", port);
    putenv(strdup(env));                              /* the forked AIOS child inherits it */

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }
    if (pid == 0) {
        char *nv[16]; int i = 0;
        for (int j = 1; j < argc && i < 12; j++) nv[i++] = argv[j];
        nv[i++] = (char *)name; nv[i++] = (char *)expect; nv[i] = NULL;
        execvp(nv[0], nv);
        _exit(127);
    }

    /* Answer EVERY query the client sends (prog_dns does two: gethostbyname + getaddrinfo), each from a
     * fresh ephemeral socket, until the child exits. A 1s recvfrom timeout lets us poll the child. */
    struct timeval rtv = { 1, 0 };
    setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof rtv);
    int served = 0, st = 0, done = 0;
    while (!done) {
        unsigned char q[1500];
        struct sockaddr_in from; socklen_t fl = sizeof from;
        ssize_t n = recvfrom(srv, q, sizeof q, 0, (struct sockaddr *)&from, &fl);
        if (n >= 12) {
            /* find the end of the question (skip QNAME + qtype + qclass) */
            int p = 12;
            while (p < n && q[p] != 0) { if ((q[p] & 0xC0) == 0xC0) { p += 2; break; } p += 1 + q[p]; }
            if (p < n && q[p] == 0) p++;
            p += 4;
            if (p <= n) {
                unsigned char r[1500]; int rl = p;
                memcpy(r, q, p);
                r[2] = 0x81; r[3] = 0x80;             /* QR=1, RD=1, RA=1, rcode=0 */
                r[6] = 0; r[7] = 1;                   /* ancount = 1 */
                r[8] = r[9] = r[10] = r[11] = 0;      /* ns/ar = 0 */
                r[rl++] = 0xC0; r[rl++] = 0x0C;       /* name = pointer to the question (offset 12) */
                r[rl++] = 0; r[rl++] = 1;             /* type  A  */
                r[rl++] = 0; r[rl++] = 1;             /* class IN */
                r[rl++] = 0; r[rl++] = 0; r[rl++] = 0; r[rl++] = 60;   /* ttl */
                r[rl++] = 0; r[rl++] = 4;             /* rdlength */
                unsigned int ip = inet_addr(expect);  /* network order */
                memcpy(r + rl, &ip, 4); rl += 4;
                sendto(srv, r, (size_t)rl, 0, (struct sockaddr *)&from, fl);
                served++;
            }
        }
        if (waitpid(pid, &st, WNOHANG) == pid) done = 1;   /* child exited -> stop serving */
    }
    close(srv);

    int client_ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("    DNS stub served a query: %s ; AIOS resolver exit: %s\n", served ? "yes" : "NO", client_ok ? "yes" : "NO");
    printf("  dns_server: %s\n", (served && client_ok) ? "PASS" : "FAIL");
    return (served && client_ok) ? 0 : 1;
}
