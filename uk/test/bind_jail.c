/*
 * bind_jail.c -- a HOST red-team driver for AIOS BIND/LISTEN network-access confinement (finishing the
 * net arc). It reserves a free ephemeral TCP port P (bind + getsockname + close), then runs the AIOS
 * red-team guest (argv[1..] = e.g. `./aios-uk ./prog_bindjail`) once per case, each under a different
 * AIOS_NET_BIND_ALLOW policy + op, and self-verifies (allow => the op succeeds; deny => it fails with
 * EACCES). PASS iff EVERY case matches -- proving the bind allow-list permits an in-list local addr:port
 * AND refuses everything else (wrong port, any-interface bind when only loopback is allowed, a different
 * subnet, MALFORMED rules that must FAIL CLOSED), AND that listen() cannot auto-bind an ephemeral port
 * past the gate (the auto-bind guard), while a permissive "*" policy still allows it. Self-contained;
 * HOST code, cc.
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

static int run_case(char **argv, int argc, const char *host, int gport, int P,
                    const char *allowfmt, const char *op, const char *expect)
{
    char allow[96]; snprintf(allow, sizeof allow, allowfmt, P);   /* %d in the policy -> the reserved port */
    char portstr[16]; snprintf(portstr, sizeof portstr, "%d", gport);
    pid_t pid = fork();
    if (pid == 0) {
        putenv(strdup(allow));                       /* the AIOS child inherits this AIOS_NET_BIND_ALLOW */
        char *nv[64]; int i = 0;
        for (int j = 1; j < argc && i < 56; j++) nv[i++] = argv[j];
        nv[i++] = (char *)host; nv[i++] = portstr; nv[i++] = (char *)expect; nv[i++] = (char *)op; nv[i] = NULL;
        execvp(nv[0], nv);
        _exit(127);
    }
    int st = 0; waitpid(pid, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;  /* the guest confirmed the expected outcome */
    printf("    [%-26s %-9s %-5s] -> %s\n", allow, op, expect, ok ? "ok" : "MISMATCH");
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <aios-uk> <prog_bindjail>\n", argv[0]); return 2; }

    /* reserve a free ephemeral port, then release it so the guest can bind it (used only by the two
     * port-granularity cases; the deny case never actually binds, so only the allow case races reuse
     * -- a sub-millisecond window on an idle box). */
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    if (probe < 0) { perror("socket"); return 2; }
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    if (bind(probe, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("bind"); return 2; }
    socklen_t sl = sizeof sa; getsockname(probe, (struct sockaddr *)&sa, &sl);
    int P = ntohs(sa.sin_port);
    close(probe);                                    /* free it for the guest */

    /* useP: guest binds the reserved port P (port-granularity cases); else binds an ephemeral port (0). */
    struct { const char *host; int useP; const char *allowfmt; const char *op; const char *expect; } cases[] = {
        { "127.0.0.1", 0, "AIOS_NET_BIND_ALLOW=127.0.0.1",     "bind",      "allow" }, /* loopback, any port */
        { "127.0.0.1", 0, "AIOS_NET_BIND_ALLOW=127.0.0.1",     "listen",    "allow" }, /* bind + listen both ok */
        { "127.0.0.1", 1, "AIOS_NET_BIND_ALLOW=127.0.0.1:%d",  "bind",      "allow" }, /* exact local addr:port */
        { "127.0.0.1", 1, "AIOS_NET_BIND_ALLOW=127.0.0.1:1",   "bind",      "deny"  }, /* right addr, WRONG port */
        { "0.0.0.0",   0, "AIOS_NET_BIND_ALLOW=127.0.0.1",     "bind",      "deny"  }, /* any-iface bind, only loopback allowed */
        { "127.0.0.1", 0, "AIOS_NET_BIND_ALLOW=10.0.0.0/8",    "bind",      "deny"  }, /* different subnet */
        { "127.0.0.1", 0, "AIOS_NET_BIND_ALLOW=127.0.0.1/:80", "bind",      "deny"  }, /* malformed prefix -> FAIL CLOSED */
        { "127.0.0.1", 0, "AIOS_NET_BIND_ALLOW=127.0.0.1:xyz", "bind",      "deny"  }, /* malformed port -> FAIL CLOSED */
        { "127.0.0.1", 0, "AIOS_NET_BIND_ALLOW=",              "bind",      "deny"  }, /* SET but EMPTY = deny all */
        { "127.0.0.1", 0, "AIOS_NET_BIND_ALLOW=127.0.0.1",     "listenraw", "deny"  }, /* auto-bind guard: unbound listen refused */
        { "0.0.0.0",   0, "AIOS_NET_BIND_ALLOW=*",             "listenraw", "allow" }, /* policy allows all -> auto-bind ok */
    };
    int n = (int)(sizeof cases / sizeof cases[0]), ok = 1;
    for (int i = 0; i < n; i++) {
        int gport = cases[i].useP ? P : 0;
        if (!run_case(argv, argc, cases[i].host, gport, P, cases[i].allowfmt, cases[i].op, cases[i].expect)) ok = 0;
    }

    printf("  bind_jail: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
