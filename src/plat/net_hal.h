/*
 * net_hal.h -- Network device Hardware Abstraction Layer
 *
 * Platform-agnostic interface for Ethernet frame TX/RX.
 * RX: net_server drains the HW ring into net_rx_ring via plat_net_drain()
 *     (v0.4.230 merged the old dedicated driver thread into net_server).
 * TX: protocol stack calls plat_net_tx().
 */
#ifndef AIOS_NET_HAL_H
#define AIOS_NET_HAL_H

#include <stdint.h>

/* Initialize network hardware.
 * Sets up DMA, IRQ, MAC address.
 * Returns 0 on success. */
int plat_net_init(void);

/* Transmit a raw Ethernet frame (no transport header prepended).
 * Returns 0 on success, -1 on error. */
int plat_net_tx(const uint8_t *frame, uint32_t len);

/* Drain the hardware RX ring into net_rx_ring (SPSC), then re-check + re-drain
 * after acking the IRQ (closes the completion-during-ack window). Called by
 * net_server at its loop top and by dhcp_poll_rx during bring-up; the merged
 * net_server's bound IRQ notification wakes its seL4_Recv, so there is no longer
 * a dedicated driver thread or cross-thread signal. v0.4.230 (netd Stage 1). */
void plat_net_drain(void);

/* Get hardware MAC address (6 bytes). */
void plat_net_get_mac(uint8_t mac[6]);

/* netd Stage 3 prov/dev split (DESIGN_NETD s3/s7). The driver compiles in one of
 * three modes selected by two defines:
 *   - neither (flag-OFF monolithic): plat_net_init() runs both halves, unchanged.
 *   - NETD_PROV (flag-ON root):  only the prov half; root calls plat_net_prov().
 *   - NETD_BUILD (flag-ON netd):  only the dev half; netd calls
 *       plat_net_dev_attach() then plat_net_init(). */
#ifndef NETD_BUILD
/* Root-side provisioning (DESIGN_NETD s3/s7): resolve the device, allocate DMA,
 * bind the IRQ, and fill *ho with the frame caps / vaddrs / paddr / IRQ caps /
 * MAC that spawn_netd hands to netd. Also sets the driver file statics so the
 * monolithic plat_net_init() shares one alloc path. Returns 0 on success.
 * driver_handoff defined in aios/netd_handoff.h. */
struct driver_handoff;
int plat_net_prov(struct driver_handoff *ho);
#endif

#ifdef NETD_BUILD
#include <sel4/sel4.h>
/* netd latches the root-provisioned MMIO/DMA/IRQ/MAC (received via argv) before
 * plat_net_init() programs the device. */
void plat_net_dev_attach(uintptr_t mmio_vaddr, int slot, uintptr_t dma_vaddr,
                         uint64_t dma_paddr, seL4_CPtr irq_handler,
                         const uint8_t mac[6]);
#endif

/* Live diagnostic hook, driven from /proc/genet (RPi4 GENET only).
 * args is the path text after "genet" ("" = dump, ".cmd[.hexargs]" = command).
 * Writes a human-readable report into buf; returns bytes, or -1 on bad command. */
int genet_diag_cmd(const char *args, char *buf, int bufsize);

#endif /* AIOS_NET_HAL_H */
