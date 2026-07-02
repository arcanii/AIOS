/* prog_dns.c -- proof for networking increment 4b: the from-scratch DNS resolver. It resolves a
 * hostname to an IPv4 address with gethostbyname() AND getaddrinfo() (both implemented in libaios as a
 * UDP DNS client over the AIOS socket ABI + SO_RCVTIMEO), and prints the address. Driven by
 * test/dns_server.c, a host DNS stub that answers on 127.0.0.1 (pointed to via $AIOS_DNS_SERVER) with a
 * fixed A record -- so the whole thing is self-contained (no external network) and exercises the real
 * DNS wire format. An AIOS-ABI program (libaios); the kernel services the UDP socket via the PAL.
 *
 * usage: prog_dns <name> [expected-ip]   -- exit 0 iff resolved (and, if given, == expected-ip). */

#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <string.h>
#include <stdio.h>

int
main(int argc, char **argv)
{
    const char *name   = argc > 1 ? argv[1] : "aios.test";
    const char *expect = argc > 2 ? argv[2] : 0;
    int ok = 1;

    struct hostent *he = gethostbyname(name);
    if (!he || !he->h_addr_list[0]) { fprintf(stderr, "prog_dns: cannot resolve %s\n", name); return 1; }
    struct in_addr a;
    memcpy(&a, he->h_addr_list[0], 4);
    const char *ip = inet_ntoa(a);
    printf("prog_dns: gethostbyname(%s) -> %s\n", name, ip);
    if (expect && strcmp(ip, expect) != 0) ok = 0;

    /* also exercise the getaddrinfo path (node + numeric service -> a sockaddr_in) */
    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(name, "80", &hints, &res) == 0 && res) {
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
        const char *ip2 = inet_ntoa(sin->sin_addr);
        printf("prog_dns: getaddrinfo(%s,80) -> %s:%d\n", name, ip2, ntohs(sin->sin_port));
        if (expect && strcmp(ip2, expect) != 0) ok = 0;
        if (ntohs(sin->sin_port) != 80) ok = 0;
        freeaddrinfo(res);
    } else {
        fprintf(stderr, "prog_dns: getaddrinfo failed\n");
        ok = 0;
    }
    return ok ? 0 : 1;
}
