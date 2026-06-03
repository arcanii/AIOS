#ifndef AIOS_DEVICE_MAP_H
#define AIOS_DEVICE_MAP_H
#include <stdint.h>

/* v0.4.149: RPi4 device-MMIO pre-mapping.
 *
 * seL4 device untypeds allocate FORWARD-ONLY from a watermark, and all RPi4
 * peripherals share one 64MB device untyped ([0xFC000000, 0x100000000)). So
 * device frames must be claimed in ASCENDING physical-address order, or a
 * lower peripheral mapped after a higher one lands behind the watermark and
 * fails in _utspace_split_alloc. That is exactly why GENET (0xFD58_0000) and
 * the VC mailbox (0xFE00B_000) -- both BELOW the eMMC/UART/GPIO that were
 * mapped first -- were disabled in v0.4.98.
 *
 * prealloc_rpi4_devices() maps every peripheral MMIO region once, low->high,
 * right after the DTB parse, and stashes the vaddrs below. The drivers then
 * use these instead of calling sel4platsupport_alloc_frame_at themselves.
 * On QEMU it is a no-op (the globals stay NULL; the qemu-virt drivers self-map). */
void prealloc_rpi4_devices(void);

extern volatile uint32_t *dev_gpio_vaddr;    /* 0xFE200000, 1 page (ACT LED) */
extern volatile uint32_t *dev_uart_vaddr;    /* 0xFE215000, 1 page (mini UART) */
extern volatile uint32_t *dev_emmc_vaddr;    /* eMMC SDHCI, 1 page */
extern volatile uint32_t *dev_genet_vaddr;   /* GENET, 16 pages (64KB) */
extern volatile uint32_t *dev_vcmbox_vaddr;  /* VC mailbox, page base */
extern uint32_t dev_vcmbox_off;              /* mailbox reg byte offset in page */

#endif /* AIOS_DEVICE_MAP_H */
