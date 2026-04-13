/*
 * display_vc.c -- VideoCore mailbox framebuffer stub (RPi4)
 *
 * Phase 1: returns -1 (no display). Phase 4 will implement
 * ARM-to-VC mailbox property interface for framebuffer allocation.
 */
#include <stdio.h>
#include <stdint.h>
#include "plat/display_hal.h"

int plat_display_init(uint32_t width, uint32_t height) {
    (void)width; (void)height;
    printf("[gpu] RPi4 VideoCore display not yet implemented\n");
    return -1;
}

uint32_t *plat_display_get_fb(uint32_t *stride_out) {
    (void)stride_out;
    return NULL;
}
