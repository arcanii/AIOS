/*
 * netdiag.c -- userland network diagnostic (DESIGN_NETD).
 *
 * Shipped NOW (Stage 2) so it is on the disk BEFORE the Stage-3 HW cutover: if
 * Stage 3 breaks networking on the Pi, netdiag cannot be pushed afterward. It
 * works against the in-root net path today and is the home for the Stage-3/4
 * NET_DIAG (label 103) subcommands (dump / poke / mac / irqon/irqoff / reinit),
 * which the design routes through a userland tool so a wedged netd cannot freeze
 * an in-root /proc reader.
 *
 * Stage 2 scope: a liveness probe that exercises the full client->net-server IPC
 * round-trip (socket() = NET_SOCKET, close() = NET_CLOSE_SOCK). If the net
 * server is reachable, both return cleanly; if net is unavailable (e.g. no
 * DEVD_READY), socket() returns -ENOTSUP and we say so.
 *
 *   netdiag            -- probe net liveness and print status
 *   netdiag <args...>  -- reserved for Stage-3 NET_DIAG subcommands
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int probe(const char *what, int type) {
    int fd = socket(AF_INET, type, 0);
    if (fd < 0) {
        printf("  %-4s socket() = %d (errno %d) -- net unavailable\n",
               what, fd, errno);
        return -1;
    }
    printf("  %-4s socket() = fd %d  OK\n", what, fd);
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        /* Stage 3/4: parse NET_DIAG subcommands here (dump/poke/mac/irq/reinit). */
        printf("netdiag: subcommand '%s' not implemented until Stage 3\n", argv[1]);
        return 2;
    }

    printf("netdiag: net liveness probe (client <-> net server IPC)\n");
    int rc = 0;
    rc |= probe("udp", SOCK_DGRAM);
    rc |= probe("tcp", SOCK_STREAM);

    if (rc == 0)
        printf("netdiag: net server REACHABLE\n");
    else
        printf("netdiag: net server UNREACHABLE (socket path failed)\n");
    return rc ? 1 : 0;
}
