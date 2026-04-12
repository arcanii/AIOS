/* tcp_connect.c -- test client-side TCP connect()
 *
 * v0.4.86: Tests connect() by attempting a TCP connection
 * to the QEMU gateway (10.0.2.2) on port 80.
 *
 * Usage: tcp_connect [ip] [port]
 *   defaults: 10.0.2.2 port 80
 *
 * Expected results:
 *   - If port open: CONNECTED, sends HTTP GET, prints response
 *   - If port closed: ECONNREFUSED (RST) -- still proves connect works
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int parse_ip(const char *s, uint8_t *out) {
    int a, b, c, d;
    if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return -1;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b;
    out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t ip[4] = { 10, 0, 2, 2 };
    int port = 80;

    if (argc >= 2 && parse_ip(argv[1], ip) < 0) {
        printf("Bad IP: %s\n", argv[1]);
        return 1;
    }
    if (argc >= 3) {
        port = 0;
        const char *p = argv[2];
        while (*p >= '0' && *p <= '9')
            port = port * 10 + (*p++ - '0');
    }

    printf("[tcp_connect] target %d.%d.%d.%d:%d\n",
           ip[0], ip[1], ip[2], ip[3], port);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("FAIL: socket() returned %d\n", fd);
        return 1;
    }
    printf("[tcp_connect] socket fd=%d\n", fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(
        ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
        ((uint32_t)ip[2] << 8) | ip[3]);

    printf("[tcp_connect] connecting...\n");
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0) {
        int e = errno;
        printf("[tcp_connect] connect() failed, errno=%d\n", e);
        if (e == 111)
            printf("ECONNREFUSED -- port closed (connect mechanism works)\n");
        else if (e == 101)
            printf("ENETUNREACH -- no ARP route\n");
        else
            printf("FAIL: unexpected errno %d\n", e);
        close(fd);
        return (e == 111) ? 0 : 1;
    }

    printf("[tcp_connect] CONNECTED\n");

    /* Send a simple HTTP GET */
    const char *req = "GET / HTTP/1.0\r\nHost: test\r\n\r\n";
    int wlen = (int)strlen(req);
    int w = (int)write(fd, req, wlen);
    printf("[tcp_connect] sent %d/%d bytes\n", w, wlen);

    /* Read response (up to 900 bytes, MR limit) */
    char buf[901];
    int n = (int)read(fd, buf, 900);
    if (n > 0) {
        buf[n] = '\0';
        /* Print first line only */
        char *nl = buf;
        while (*nl && *nl != '\n') nl++;
        if (*nl) *nl = '\0';
        printf("[tcp_connect] response: %s\n", buf);
    } else {
        printf("[tcp_connect] read returned %d\n", n);
    }

    close(fd);
    printf("[tcp_connect] OK\n");
    return 0;
}
