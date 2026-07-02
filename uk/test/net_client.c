/*
 * net_client.c -- a HOST test driver for AIOS networking (increment 1). It stands up a TCP echo server
 * on 127.0.0.1:<ephemeral>, launches the AIOS client `<argv[1..]> 127.0.0.1 <port>` (i.e.
 * `./aios-uk ./prog_net 127.0.0.1 <port>`), accepts the connection, echoes the one message it receives,
 * and PASSes iff it saw a message AND the AIOS client exited 0 (its own round-trip check). Self-contained
 * -- no external network. HOST (Linux) code; compile with `cc`.
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
    if (argc < 2) { fprintf(stderr, "usage: %s <aios-uk> <prog_net> [args...]\n", argv[0]); return 2; }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 2; }
    int one = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("bind"); return 2; }
    if (listen(srv, 1) < 0) { perror("listen"); return 2; }

    socklen_t sl = sizeof sa; getsockname(srv, (struct sockaddr *)&sa, &sl);
    char port[16]; snprintf(port, sizeof port, "%d", ntohs(sa.sin_port));

    pid_t pid = fork();
    if (pid == 0) {
        char *nv[64]; int i = 0;
        for (int j = 1; j < argc && i < 60; j++) nv[i++] = argv[j];
        nv[i++] = "127.0.0.1"; nv[i++] = port; nv[i] = NULL;
        execvp(nv[0], nv);
        _exit(127);
    }

    int ran = 0;
    int c = accept(srv, NULL, NULL);
    if (c >= 0) {
        char buf[256];
        ssize_t n = read(c, buf, sizeof buf);
        if (n > 0) { (void)!write(c, buf, (size_t)n); ran = 1; }   /* echo it back */
        close(c);
    }
    close(srv);

    int st = 0; waitpid(pid, &st, 0);
    int client_ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;

    printf("    server accepted + echoed a message: %s ; AIOS client round-trip: %s\n",
           ran ? "yes" : "NO", client_ok ? "yes" : "NO");
    printf("  net_client: %s\n", (ran && client_ok) ? "PASS" : "FAIL");
    return (ran && client_ok) ? 0 : 1;
}
