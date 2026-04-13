/*
 * boot_net_init.c -- Network initialization (platform-agnostic)
 *
 * Calls platform HAL to init network hardware.
 * All platform-specific code moved to src/plat/qemu-virt/net_virtio.c.
 */
#include "aios/root_shared.h"
#include <stdio.h>
#include "plat/net_hal.h"

void boot_net_init(void) {
    if (plat_net_init() == 0) {
        printf("[boot] Network initialized\n");
    }
}
