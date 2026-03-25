/*
 * AIOS – Lock-free single-producer single-consumer ring buffer
 *
 * Used for UART RX/TX between serial_driver and orchestrator.
 * Layout must match in both PDs.
 */
#ifndef AIOS_RING_H
#define AIOS_RING_H

#include <stdint.h>

#define RING_OFFSET  64
#define RING_SIZE    (0x1000 - RING_OFFSET)

typedef struct {
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    uint8_t _pad[RING_OFFSET - 8];
    volatile uint8_t data[RING_SIZE];
} ring_buf_t;

static inline void ring_put(ring_buf_t *ring, uint8_t ch) {
    uint32_t wi = ring->write_idx;
    uint32_t next = (wi + 1) % RING_SIZE;
    if (next == ring->read_idx) return;   /* full */
    ring->data[wi] = ch;
    ring->write_idx = next;
}

static inline int ring_get(ring_buf_t *ring, char *out) {
    uint32_t ri = ring->read_idx;
    if (ri == ring->write_idx) return 0;  /* empty */
    *out = (char)ring->data[ri];
    ring->read_idx = (ri + 1) % RING_SIZE;
    return 1;
}

#endif /* AIOS_RING_H */

