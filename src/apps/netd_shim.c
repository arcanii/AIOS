/*
 * netd_shim.c -- netd-local definitions of the net-state globals that the root
 * task owns in the monolithic build.
 *
 * netd de-monolithization (DESIGN_NETD s3/s6). When the net stack
 * (net_server.c, src/net/*.c, the platform driver dev half) is compiled into the
 * isolated netd process (NETD_BUILD), it still references the net-state globals
 * that live in the root task (aios_root.c / config_parser.c) in the monolithic
 * build: net_mac, net_available, the endpoint / notification caps, the RX ring,
 * and the IP config. netd owns its OWN copies here; spawn_netd hands the real
 * values over via argv at startup (Stage 3) and netd fills them in.
 *
 * Globals defined INSIDE the net sources themselves (net_rx_stats, tcp_tx_window,
 * dhcp_*, net_fault_drop_nth, net_dhcp_pending) travel into netd with those files
 * and are NOT redefined here. The includes below pull in the canonical extern
 * declarations so every definition is type-checked against them.
 *
 * Compiled ONLY into netd (NETD_BUILD); see projects/aios/CMakeLists.txt. The
 * link of these into the netd binary is the isolation proof: if the net stack
 * referenced a root-only symbol the shim does not provide (e.g. the vka `vka`
 * global), netd would fail to link.
 */
#include <stdint.h>
#include <sel4/sel4.h>
#include "aios/root_shared.h"   /* net_mac, net_available, net_*_cap, net_rx_ring extern */
#include "aios/net.h"           /* full struct net_rx_ring definition */
#include "aios/config.h"        /* net_cfg_ip/gw/mask extern */
#include "aios/netd_stats.h"    /* struct netd_stats (the /proc/net page) */

/* --- root_shared.h net globals (root defines these in aios_root.c) --- */
uint8_t   net_mac[6];
int       net_available;
seL4_CPtr net_ep_cap;
seL4_CPtr net_drv_ntfn_cap;
seL4_CPtr net_kick_ntfn_cap;
struct net_rx_ring net_rx_ring;

/* --- config.h IP config (root defines these in config_parser.c) --- */
uint8_t net_cfg_ip[4];
uint8_t net_cfg_gw[4];
uint8_t net_cfg_mask[4];

/* --- netd reply-slot range base: 3*MAX_NET_SOCKETS slots reserved by spawn_netd
 *     past netd's cspace_next_free, addressed in netd's own CNode. Set from argv
 *     in netd main (Stage 3); net_server.c reads it to populate its reply-slot
 *     arrays instead of calling vka. --- */
seL4_CPtr netd_reply_slot_base;

/* --- netd /proc/net stats page (DESIGN_NETD s6): a cacheable-both single-writer
 *     frame root maps + copies to netd; netd writes the heartbeat + dhcp/ip/mac/
 *     socket state into it each event-loop iteration. NULL until set from argv in
 *     netd main; net_server.c writes through it via netd_stats_update(). --- */
struct netd_stats *netd_stats_page;
