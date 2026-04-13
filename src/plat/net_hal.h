/*
 * net_hal.h -- Network device Hardware Abstraction Layer
 *
 * Platform-agnostic interface for Ethernet frame TX/RX.
 * RX: driver thread writes to net_rx_ring (SPSC).
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

/* Driver thread main loop.
 * Waits on hardware IRQ, drains RX into net_rx_ring,
 * signals net_server notification.
 * Signature matches seL4 thread entry. */
void plat_net_driver_fn(void *arg0, void *arg1, void *ipc_buf);

/* Get hardware MAC address (6 bytes). */
void plat_net_get_mac(uint8_t mac[6]);

#endif /* AIOS_NET_HAL_H */
