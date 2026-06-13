#ifndef AIOS_NETD_STATS_H
#define AIOS_NETD_STATS_H
/*
 * netd_stats.h -- the /proc/net stats page (DESIGN_NETD s6).
 *
 * ONE cacheable-both single-writer frame: netd writes it once per event-loop
 * iteration; root reads it IPC-free (the fs thread for /proc/net, serverstats for
 * the SRV_NET row). Because root never Calls netd to read liveness, this works
 * even when netd is WEDGED -- it is the only hung-netd detector (non-MCS Calls
 * cannot time out) and the HW-soak liveness instrument.
 *
 * Map BOTH ends with the SAME memory type (cacheable) per the v0.4.165 pipe-SHM
 * cache-coherency rule; root + netd both run on core 0, so a plain volatile read
 * of the heartbeat is coherent. netd is the SOLE writer; root only reads.
 */
#include <stdint.h>

#define NETD_STATS_SOCKS  8   /* mirrors MAX_NET_SOCKETS */

struct netd_stats {
    volatile uint64_t heartbeat;      /* netd bumps once per event-loop iteration */
    volatile uint32_t dev_init_done;  /* 1 after plat_net_init succeeded */
    volatile uint32_t dhcp_bound;     /* 1 once a lease is held */
    volatile uint32_t dhcp_lease_secs;
    volatile uint32_t dhcp_renews;
    volatile uint32_t cleanup_lost;   /* reserved: op-98 ring overflow count */
    volatile int32_t  last_err;       /* last fatal/notable error code (0 = none) */
    volatile uint8_t  mac[6];
    volatile uint8_t  ip[4];
    volatile uint8_t  gw[4];
    volatile uint8_t  mask[4];
    struct {
        volatile uint8_t in_use;
        volatile uint8_t type;        /* 1=STREAM, 2=DGRAM */
        volatile uint8_t state;       /* TCP_* */
        volatile int32_t owner_pid;
    } sock[NETD_STATS_SOCKS];
};

#endif /* AIOS_NETD_STATS_H */
