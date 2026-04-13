/*
 * net_genet.c -- BCM54213 GENET Ethernet stub (RPi4)
 *
 * Phase 1: returns -1 (no network). Phase 3 will implement
 * GENET MAC init, MDIO PHY, and DMA ring I/O.
 */
#include <stdio.h>
#include <stdint.h>
#include "plat/net_hal.h"

int plat_net_init(void) {
    printf("[net] RPi4 GENET driver not yet implemented\n");
    return -1;
}

int plat_net_tx(const uint8_t *frame, uint32_t len) {
    (void)frame; (void)len;
    return -1;
}

void plat_net_driver_fn(void *arg0, void *arg1, void *ipc_buf) {
    (void)arg0; (void)arg1; (void)ipc_buf;
}

void plat_net_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = 0;
}
