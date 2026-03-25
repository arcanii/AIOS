/*
 * AIOS – Serial output helpers (used by orchestrator)
 *
 * These write into the TX ring buffer and notify the serial driver.
 * Requires: tx_buf (global uintptr_t), CH_SERIAL, ring_buf_t.
 */
#ifndef AIOS_SERIAL_H
#define AIOS_SERIAL_H

#include <microkit.h>
#include "aios/ring.h"
#include "aios/channels.h"

extern uintptr_t tx_buf;

static inline void ser_putc(char c) {
    ring_buf_t *tx = (ring_buf_t *)tx_buf;
    if (c == '\n') ring_put(tx, '\r');
    ring_put(tx, (uint8_t)c);
}

static inline void ser_puts(const char *s) {
    while (*s) ser_putc(*s++);
    microkit_notify(CH_SERIAL);
}

static inline void ser_put_dec(uint32_t v) {
    char buf[12];
    int i = 0;
    if (v == 0) { ser_putc('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) ser_putc(buf[--i]);
}

#endif /* AIOS_SERIAL_H */

