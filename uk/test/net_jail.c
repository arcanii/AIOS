/*
 * net_jail.c -- a HOST red-team driver for AIOS network-access confinement (inc 5). It binds a TCP
 * listener on 127.0.0.1:<P> (backlog only -- an allowed non-blocking connect completes via the accept
 * queue, so no accept() call is needed), then runs the AIOS red-team guest (argv[1..] = e.g.
 * `./aios-uk ./prog_netjail`) once per case, each under a different AIOS_NET_ALLOW policy, and appends
 * `127.0.0.1 <P> allow|deny`. Each guest self-verifies (allow => connect succeeds; deny => connect
 * fails with EACCES). PASS iff EVERY case matches -- proving the allow-list permits in-list endpoints
 * AND refuses everything else, INCLUDING malformed rules which must FAIL CLOSED (never silently allow-
 * all). Self-contained; HOST code, cc.
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

static int run_case(char **argv, int argc, int port, const char *allowfmt, const char *expect)
{
    char allow[96]; snprintf(allow, sizeof allow, allowfmt, port);
    char portstr[16]; snprintf(portstr, sizeof portstr, "%d", port);
    pid_t pid = fork();
    if (pid == 0) {
        putenv(strdup(allow));                       /* the AIOS child inherits this AIOS_NET_ALLOW */
        char *nv[64]; int i = 0;
        for (int j = 1; j < argc && i < 58; j++) nv[i++] = argv[j];
        nv[i++] = "127.0.0.1"; nv[i++] = portstr; nv[i++] = (char *)expect; nv[i] = NULL;
        execvp(nv[0], nv);
        _exit(127);
    }
    int st = 0; waitpid(pid, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;  /* the guest confirmed the expected outcome */
    printf("    [%-28s %-5s] -> %s\n", allow, expect, ok ? "ok" : "MISMATCH");
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <aios-uk> <prog_netjail>\n", argv[0]); return 2; }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 2; }
    int one = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("bind"); return 2; }
    if (listen(srv, 16) < 0) { perror("listen"); return 2; }
    socklen_t sl = sizeof sa; getsockname(srv, (struct sockaddr *)&sa, &sl);
    int port = ntohs(sa.sin_port);

    struct { const char *allowfmt; const char *expect; } cases[] = {
        { "AIOS_NET_ALLOW=127.0.0.1:%d",   "allow" },  /* exact host:port in the list */
        { "AIOS_NET_ALLOW=127.0.0.0/24",   "allow" },  /* CIDR covers 127.0.0.1, any port */
        { "AIOS_NET_ALLOW=127.0.0.1:1",    "deny"  },  /* right host, WRONG port */
        { "AIOS_NET_ALLOW=10.0.0.0/8:*",   "deny"  },  /* different subnet */
        { "AIOS_NET_ALLOW=127.0.0.1/:80",  "deny"  },  /* malformed prefix must FAIL CLOSED (not allow-all) */
        { "AIOS_NET_ALLOW=127.0.0.1:abc",  "deny"  },  /* malformed port must FAIL CLOSED (not any-port) */
        { "AIOS_NET_ALLOW=",               "deny"  },  /* SET but EMPTY = deny all (engaged, no endpoints) */
    };
    int n = (int)(sizeof cases / sizeof cases[0]), ok = 1;
    for (int i = 0; i < n; i++)
        if (!run_case(argv, argc, port, cases[i].allowfmt, cases[i].expect)) ok = 0;
    close(srv);

    printf("  net_jail: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
