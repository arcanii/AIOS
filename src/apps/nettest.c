/* nettest.c -- guest-side socket regression driver for net_socket_qemu_test.py
 *
 * v0.4.229 (netd Stage 0). Each subcommand exercises one server-side socket
 * path that previously had NO automated coverage (the existing net scripts are
 * all listen/accept-side) and prints a single deterministic result line the
 * host test greps. Defaults to the QEMU SLIRP host alias 10.0.2.2.
 *
 *   nettest connect  <ip> <port>   TCP connect + echo round-trip -> PASS/FAIL
 *   nettest refused  <ip> <port>   TCP connect to a closed port  -> expect ECONNREFUSED
 *   nettest udp      <ip> <port>   UDP send + echo recv          -> PASS/FAIL
 *   nettest recvrst  <ip> <port>   park in recv, peer sends RST  -> expect ECONNRESET
 *   nettest dblclose <ip> <port>   close() the same fd twice     -> PASS/FAIL
 *   nettest count                  open up to 8 sockets, close   -> "free=N"
 *   nettest hog                    open up to 8 sockets, EXIT    -> "opened=N" (op-98)
 *
 * recvrst is the key regression for the RST reply-slot poisoning fix: without
 * it the parked recv is never woken and this process hangs forever (the host
 * test then times out); with it, read() returns ECONNRESET (errno 104).
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAXS 8

static int parse_ip(const char *s, uint8_t *o) {
    int a, b, c, d;
    if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return -1;
    o[0] = (uint8_t)a; o[1] = (uint8_t)b; o[2] = (uint8_t)c; o[3] = (uint8_t)d;
    return 0;
}

static void fill_addr(struct sockaddr_in *sa, const uint8_t *ip, int port) {
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    sa->sin_addr.s_addr = htonl(((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
                                ((uint32_t)ip[2] << 8) | ip[3]);
}

static int do_connect(const uint8_t *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("NETTEST connect FAIL socket errno=%d\n", errno); return 1; }
    struct sockaddr_in sa; fill_addr(&sa, ip, port);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("NETTEST connect FAIL connect errno=%d\n", errno); close(fd); return 1;
    }
    write(fd, "PING", 4);
    char buf[64];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    if (n == 4 && memcmp(buf, "PING", 4) == 0) printf("NETTEST connect PASS\n");
    else printf("NETTEST connect FAIL echo n=%d errno=%d\n", n, errno);
    close(fd);
    return 0;
}

static int do_refused(const uint8_t *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("NETTEST refused FAIL socket errno=%d\n", errno); return 1; }
    struct sockaddr_in sa; fill_addr(&sa, ip, port);
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno == 111) printf("NETTEST refused PASS\n");     /* ECONNREFUSED */
    else printf("NETTEST refused FAIL rc=%d errno=%d\n", rc, errno);
    close(fd);
    return 0;
}

static int do_udp(const uint8_t *ip, int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { printf("NETTEST udp FAIL socket errno=%d\n", errno); return 1; }
    uint8_t any[4] = {0, 0, 0, 0};
    struct sockaddr_in la; fill_addr(&la, any, 12345);   /* local port for the echo reply */
    bind(fd, (struct sockaddr *)&la, sizeof(la));
    struct sockaddr_in sa; fill_addr(&sa, ip, port);
    int w = (int)sendto(fd, "UDP123", 6, 0, (struct sockaddr *)&sa, sizeof(sa));
    if (w != 6) { printf("NETTEST udp FAIL sendto=%d errno=%d\n", w, errno); close(fd); return 1; }
    char buf[64];
    int n = (int)recvfrom(fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
    if (n == 6 && memcmp(buf, "UDP123", 6) == 0) printf("NETTEST udp PASS\n");
    else printf("NETTEST udp FAIL recv n=%d errno=%d\n", n, errno);
    close(fd);
    return 0;
}

static int do_recvrst(const uint8_t *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("NETTEST recvrst FAIL socket errno=%d\n", errno); return 1; }
    struct sockaddr_in sa; fill_addr(&sa, ip, port);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("NETTEST recvrst FAIL connect errno=%d\n", errno); close(fd); return 1;
    }
    /* Park in recv. The host accepts, waits for us to be blocked here, then
     * sends a RST (SO_LINGER 0 + close). With the Stage-0 fix the RST path
     * wakes us with ECONNRESET; without it this read() never returns. */
    printf("NETTEST recvrst connected, reading...\n");
    char buf[64];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    if (n < 0 && errno == 104) printf("NETTEST recvrst PASS\n");      /* ECONNRESET */
    else printf("NETTEST recvrst FAIL n=%d errno=%d\n", n, errno);
    close(fd);
    return 0;
}

static int do_dblclose(const uint8_t *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("NETTEST dblclose FAIL socket errno=%d\n", errno); return 1; }
    struct sockaddr_in sa; fill_addr(&sa, ip, port);
    connect(fd, (struct sockaddr *)&sa, sizeof(sa));   /* ESTAB if the echo port is up */
    int c1 = close(fd);
    int c2 = close(fd);            /* second close on the same fd must be harmless */
    /* Prove the server still serves after the double close. */
    int fd2 = socket(AF_INET, SOCK_STREAM, 0);
    if (fd2 >= 0) { close(fd2); printf("NETTEST dblclose PASS c1=%d c2=%d\n", c1, c2); return 0; }
    printf("NETTEST dblclose FAIL reopen errno=%d\n", errno);
    return 1;
}

static int do_bulk(const uint8_t *ip, int port) {
    /* Receive a large TCP stream and report the byte count. Stresses the RX
     * path / the merged-drain NAPI re-check (v0.4.230): frames pile into the HW
     * ring faster than one read() drains the socket buffer, so the ring fills
     * and the re-check must consume what completed during the IRQ-ack window.
     * TCP must still deliver every byte (peer retransmit covers any HW drop). */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("NETTEST bulk FAIL socket errno=%d\n", errno); return 1; }
    struct sockaddr_in sa; fill_addr(&sa, ip, port);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("NETTEST bulk FAIL connect errno=%d\n", errno); close(fd); return 1;
    }
    char buf[1024];
    long total = 0;
    int n;
    while ((n = (int)read(fd, buf, sizeof(buf))) > 0) total += n;
    printf("NETTEST bulk recv=%ld last=%d\n", total, n);
    close(fd);
    return 0;
}

static int do_closelinger(const uint8_t *ip, int port) {
    /* v0.4.253-fix loss test: connect, echo, close(), then STAY ALIVE ~14s so the
     * server-side socket LINGERS (no op-98 process-exit reap). This mirrors a
     * long-lived server (netconsole/sshd) whose per-connection socket outlives the
     * request: with the FIN dropped (/proc/netstat.findrop.N) the deferred close
     * cannot complete and the RTO give-up fires while we sleep -- the window the
     * host test reads /proc/netstat in. Run backgrounded ("nettest closelinger ... &").*/
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("NETTEST closelinger FAIL socket errno=%d\n", errno); return 1; }
    struct sockaddr_in sa; fill_addr(&sa, ip, port);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("NETTEST closelinger FAIL connect errno=%d\n", errno); close(fd); return 1;
    }
    write(fd, "PING", 4);
    char buf[64];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    int echo_ok = (n == 4 && memcmp(buf, "PING", 4) == 0);
    close(fd);                       /* deferred graceful close in net_server */
    struct timespec ts; ts.tv_sec = 14; ts.tv_nsec = 0;
    nanosleep(&ts, (void *)0);       /* keep the owner alive -> socket lingers */
    printf("NETTEST closelinger %s\n", echo_ok ? "PASS" : "FAIL");
    return 0;
}

static int do_count(void) {
    int fds[MAXS], n = 0;
    for (int i = 0; i < MAXS; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;
        fds[n++] = fd;
    }
    printf("NETTEST count free=%d\n", n);
    for (int i = 0; i < n; i++) close(fds[i]);
    return 0;
}

static int do_hog(void) {
    int n = 0;
    for (int i = 0; i < MAXS; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;
        n++;
    }
    printf("NETTEST hog opened=%d\n", n);
    /* Intentionally exit WITHOUT closing: op-98 (NET_CLEANUP_PID, driven by the
     * cleanup proxy on process exit) must reclaim these server slots. */
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("NETTEST FAIL no-subcommand\n"); return 2; }
    const char *cmd = argv[1];
    uint8_t ip[4] = {10, 0, 2, 2};
    int port = 0;
    if (argc >= 3 && parse_ip(argv[2], ip) < 0) { printf("NETTEST FAIL bad-ip\n"); return 2; }
    if (argc >= 4) {
        const char *p = argv[3];
        while (*p >= '0' && *p <= '9') port = port * 10 + (*p++ - '0');
    }

    if (!strcmp(cmd, "connect"))  return do_connect(ip, port);
    if (!strcmp(cmd, "refused"))  return do_refused(ip, port);
    if (!strcmp(cmd, "udp"))      return do_udp(ip, port);
    if (!strcmp(cmd, "recvrst"))  return do_recvrst(ip, port);
    if (!strcmp(cmd, "dblclose")) return do_dblclose(ip, port);
    if (!strcmp(cmd, "closelinger")) return do_closelinger(ip, port);
    if (!strcmp(cmd, "bulk"))     return do_bulk(ip, port);
    if (!strcmp(cmd, "count"))    return do_count();
    if (!strcmp(cmd, "hog"))      return do_hog();
    printf("NETTEST FAIL unknown-subcommand %s\n", cmd);
    return 2;
}
