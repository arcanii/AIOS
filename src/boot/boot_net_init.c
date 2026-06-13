/*
 * boot_net_init.c -- Network initialization (platform-agnostic)
 *
 * Calls platform HAL to init network hardware.
 * All platform-specific code moved to src/plat/qemu-virt/net_virtio.c.
 *
 * netd Stage 3 (DESIGN_NETD s8): under AIOS_NETD the net stack runs in the
 * isolated netd process, so root only PROVISIONS the NIC here (DMA / IRQ / MAC /
 * frame caps) and records net_hw_present; netd (spawned in boot_services) programs
 * the device and publishes net_available on DEVD_READY. boot_net_init runs before
 * the boot banner, so net_hw_present is meaningful at banner time. Flag-OFF keeps
 * the in-root device init exactly as before.
 */
#define LOG_MODULE "net"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "aios/aios_log.h"
#include "aios/root_shared.h"
#include <stdio.h>
#include "plat/net_hal.h"

void boot_net_init(void) {
#ifdef AIOS_NETD
    /* Provision only (root side). netd does the device-register init. */
    netd_prov();
#else
    if (plat_net_init() == 0) {
        AIOS_LOG_INFO("Network initialized");
    }
    /* The banner reads net_hw_present; in the monolithic build it equals
     * net_available (the in-root driver set it on a successful init). */
    net_hw_present = net_available;
#endif
}
