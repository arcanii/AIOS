/*
 * display_hal.h -- Display device Hardware Abstraction Layer
 *
 * Platform-agnostic interface for framebuffer allocation.
 * Font rendering and splash are in boot_display_init.c (generic).
 */
#ifndef AIOS_DISPLAY_HAL_H
#define AIOS_DISPLAY_HAL_H

#include <stdint.h>

/* Initialize display hardware and allocate framebuffer.
 * Returns 0 on success, -1 if no display available (serial-only). */
int plat_display_init(uint32_t width, uint32_t height);

/* Get framebuffer base address.
 * Returns NULL if display not initialized.
 * Writes stride (bytes per row) to *stride_out. */
uint32_t *plat_display_get_fb(uint32_t *stride_out);

#endif /* AIOS_DISPLAY_HAL_H */
