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

/* Live diagnostic hook, driven from /proc/genet (RPi4 GENET only).
 * args is the path text after "genet" ("" = dump, ".cmd[.hexargs]" = command).
 * Writes a human-readable report into buf; returns bytes, or -1 on bad command. */
int genet_diag_cmd(const char *args, char *buf, int bufsize);

#endif /* AIOS_NET_HAL_H */
