/*
 * net_server.c -- a HOST test driver for AIOS networking (increment 2, the SERVER). It launches the
 * AIOS server (argv[1..] = e.g. `./aios-uk ./prog_netserver`), captures its stdout, reads the
 * "PORT <n>" line the server prints after it binds+listens, connects to 127.0.0.1:<n> as a CLIENT,
 * sends one message, reads the echo, and PASSes iff the echo matches AND the AIOS server exited 0.
 * Self-contained -- no external network. HOST (Linux) code; compile with `cc`.
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
    if (argc < 2) { fprintf(stderr, "usage: %s <aios-uk> <prog_netserver> [args...]\n", argv[0]); return 2; }

    int pfd[2];
    if (pipe(pfd) != 0) { perror("pipe"); return 2; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }
    if (pid == 0) {                                  /* child: AIOS server, stdout -> the pipe */
        dup2(pfd[1], 1);
        close(pfd[0]); close(pfd[1]);
        char *nv[64]; int i = 0;
        for (int j = 1; j < argc && i < 63; j++) nv[i++] = argv[j];
        nv[i] = NULL;
        execvp(nv[0], nv);
        _exit(127);
    }
    close(pfd[1]);

    /* read the "PORT <n>" line from the AIOS server's stdout */
    FILE *cf = fdopen(pfd[0], "r");
    int port = -1;
    char line[256];
    while (cf && fgets(line, sizeof line, cf)) {
        if (sscanf(line, "PORT %d", &port) == 1) break;
    }

    int ran = 0;
    if (port > 0) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons((unsigned short)port);
        if (s >= 0 && connect(s, (struct sockaddr *)&sa, sizeof sa) == 0) {
            const char *msg = "AIOS-SERVER-PING";
            if (write(s, msg, strlen(msg)) > 0) {
                char buf[256];
                ssize_t n = read(s, buf, sizeof buf - 1);
                if (n > 0) { buf[n] = '\0'; if (strcmp(buf, msg) == 0) ran = 1; }
            }
        }
        if (s >= 0) close(s);
    }

    int st = 0; waitpid(pid, &st, 0);                /* server writes its final line + exits */
    int server_ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (cf) fclose(cf); else close(pfd[0]);

    printf("    connected to the AIOS server on port %d, round-trip echo: %s ; AIOS server exit: %s\n",
           port, ran ? "yes" : "NO", server_ok ? "yes" : "NO");
    printf("  net_server: %s\n", (ran && server_ok) ? "PASS" : "FAIL");
    return (ran && server_ok) ? 0 : 1;
}
