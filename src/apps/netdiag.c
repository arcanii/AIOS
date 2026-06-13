/*
 * netdiag.c -- userland network diagnostic (DESIGN_NETD s6/s11).
 *
 * The userland home for the NET_DIAG (label 103) active ops. Under AIOS_NETD the
 * net stack lives in the MMU-isolated netd process; the active device pokes
 * (poke / mdio / tx / reinit / irq / mac) MUST be issued by a process Calling
 * net_ep, never by the fs thread serving /proc -- a hung netd would otherwise
 * wedge every /proc read. netdiag is that SACRIFICIAL caller: if netd is wedged,
 * only netdiag blocks (Ctrl-C it); /proc and the rest of the system stay alive.
 * /proc/genet is now a read-only, UMAC/MDIO-free root view; the live ops are here.
 *
 *   netdiag                       -- net liveness probe (socket round-trip)
 *   netdiag peek  OFF             -- read a device register
 *   netdiag poke  OFF VAL         -- write a device register, read back
 *   netdiag mr    PHY REG         -- MDIO read  (GENET only)
 *   netdiag mw    PHY REG VAL     -- MDIO write (GENET only)
 *   netdiag tx                    -- send one broadcast test frame
 *   netdiag reinit                -- re-run ring_init   (GENET only)
 *   netdiag irqon | irqoff        -- toggle IRQ-driven RX (GENET only)
 *   netdiag mac                   -- print the device MAC
 *
 * All numbers hex. Args go to netd over NET_DIAG; the reply carries a status and
 * up to two result words (DESIGN_NETD s6). -2 = op not supported on this NIC.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "aios/netd_ctrl.h"   /* NETD_DIAG_* + aios_net_diag() */

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

static void usage(void) {
    printf("usage: netdiag [peek OFF | poke OFF VAL | mr PHY REG | mw PHY REG VAL |\n"
           "                tx | reinit | irqon | irqoff | mac]\n"
           "       (no args = net liveness probe).  all numbers hex.\n");
}

int main(int argc, char **argv) {
    if (argc <= 1) {
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

    const char *cmd = argv[1];
    uint32_t a = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 16) : 0;
    uint32_t b = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 16) : 0;
    uint32_t c = (argc > 4) ? (uint32_t)strtoul(argv[4], NULL, 16) : 0;

    int op;
    if      (!strcmp(cmd, "peek"))   op = NETD_DIAG_PEEK;
    else if (!strcmp(cmd, "poke"))   op = NETD_DIAG_POKE;
    else if (!strcmp(cmd, "mr"))     op = NETD_DIAG_MR;
    else if (!strcmp(cmd, "mw"))     op = NETD_DIAG_MW;
    else if (!strcmp(cmd, "tx"))     op = NETD_DIAG_TX;
    else if (!strcmp(cmd, "reinit")) op = NETD_DIAG_REINIT;
    else if (!strcmp(cmd, "irqon"))  op = NETD_DIAG_IRQON;
    else if (!strcmp(cmd, "irqoff")) op = NETD_DIAG_IRQOFF;
    else if (!strcmp(cmd, "mac"))    op = NETD_DIAG_MAC;
    else { printf("netdiag: unknown cmd '%s'\n", cmd); usage(); return 2; }

    uint32_t out[2] = { 0, 0 };
    int ret = aios_net_diag(op, a, b, c, out);
    if (ret == -ENOTSUP) {
        printf("netdiag: net unavailable (no net server)\n");
        return 1;
    }

    switch (op) {
    case NETD_DIAG_PEEK:
        printf("[%05x] = %08x  (ret %d)\n", a & ~3u, out[0], ret); break;
    case NETD_DIAG_POKE:
        printf("[%05x] <= %08x  readback %08x  (ret %d)\n", a & ~3u, b, out[0], ret); break;
    case NETD_DIAG_MR:
        printf("mdio phy %x reg %x = %04x  (ret %d)\n", a, b, out[0] & 0xFFFF, ret); break;
    case NETD_DIAG_MW:
        printf("mdio phy %x reg %x <= %04x  readback %04x  (ret %d)\n",
               a, b, c & 0xFFFF, out[0] & 0xFFFF, ret); break;
    case NETD_DIAG_TX:
        printf("tx ret=%d  txprod=%x txcons=%x\n", ret, out[0], out[1]); break;
    case NETD_DIAG_REINIT:
        printf("reinit ret=%d\n", ret); break;
    case NETD_DIAG_IRQON:
        printf("RX IRQ-driven ON (INTRL2 unmasked)  ret=%d\n", ret); break;
    case NETD_DIAG_IRQOFF:
        printf("RX IRQ masked + net_server kicked  ret=%d\n", ret); break;
    case NETD_DIAG_MAC:
        printf("mac = %02x:%02x:%02x:%02x:%02x:%02x  (ret %d)\n",
               (out[0] >> 24) & 0xFF, (out[0] >> 16) & 0xFF,
               (out[0] >> 8) & 0xFF, out[0] & 0xFF,
               (out[1] >> 8) & 0xFF, out[1] & 0xFF, ret); break;
    }
    if (ret == -2)
        printf("  (op not supported on this NIC -- GENET only)\n");
    return ret < 0 ? 1 : 0;
}
