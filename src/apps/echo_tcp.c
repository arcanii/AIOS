/*
 * echo_tcp.c -- TCP echo server (tests socket read/write)
 *
 * Listens on port 7777, accepts one connection at a time,
 * reads data via read() and echoes it back via write().
 * Tests the v0.4.81 socket fd routing in posix_file.c.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main(void) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        printf("socket: failed\n");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(7777);
    addr.sin_addr.s_addr = 0;

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("bind: failed\n");
        return 1;
    }
    if (listen(lfd, 1) < 0) {
        printf("listen: failed\n");
        return 1;
    }

    printf("[echo] Listening on port 7777\n");

    while (1) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            printf("[echo] accept failed\n");
            continue;
        }
        printf("[echo] Client connected (fd %d)\n", cfd);

        char buf[512];
        int n;
        while ((n = read(cfd, buf, sizeof(buf))) > 0) {
            write(cfd, buf, n);
            printf("[echo] Echoed %d bytes\n", n);
        }
        printf("[echo] Client disconnected\n");
        close(cfd);
    }
    return 0;
}
