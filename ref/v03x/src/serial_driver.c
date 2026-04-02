/*
 * AIOS Serial Driver – PL011 UART
 *
 * Handles UART RX/TX via shared ring buffers with the orchestrator.
 * Receives hardware IRQs for incoming characters and flushes the
 * TX ring on every notification.
 *
 * Copyright (c) 2025 AIOS Project
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <microkit.h>
#include "aios/channels.h"
#include "aios/ring.h"
#define LOG_MODULE "SERIAL"
#define LOG_LEVEL  LOG_LEVEL_INFO
#include "aios/log.h"

/* Logging backend — serial driver uses direct UART output */
static void uart_put_char(uint8_t c);  /* forward decl */

void _log_puts(const char *s) {
    while (*s) uart_put_char((uint8_t)*s++);
}
void _log_put_dec(unsigned long n) {
    char buf[20]; int i = 0;
    if (n == 0) { uart_put_char('0'); return; }
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i--) uart_put_char((uint8_t)buf[i]);
}
void _log_flush(void) { /* direct UART — nothing to flush */ }
unsigned long _log_get_time(void) {
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    return (unsigned long)(cnt / freq);
}

/* ── Memory regions (set by Microkit loader via setvar) ── */
uintptr_t uart_base_vaddr;
uintptr_t rx_buf;
uintptr_t tx_buf;

/* ── PL011 register offsets ──────────────────────────── */
#define UART_DR     0x000   /* Data Register            */
#define UART_FR     0x018   /* Flag Register            */
#define UART_IBRD   0x024   /* Integer Baud Rate        */
#define UART_FBRD   0x028   /* Fractional Baud Rate     */
#define UART_LCR_H  0x02C   /* Line Control Register    */
#define UART_CR     0x030   /* Control Register         */
#define UART_IMSC   0x038   /* Interrupt Mask Set/Clear */
#define UART_ICR    0x044   /* Interrupt Clear Register */

/* Flag register bits */
#define FR_RXFE     (1 << 4)   /* RX FIFO empty  */
#define FR_TXFF     (1 << 5)   /* TX FIFO full   */

/* ── Register access macro ───────────────────────────── */
#define REG(off)    (*(volatile uint32_t *)(uart_base_vaddr + (off)))

/* ── UART helpers ────────────────────────────────────── */

static int uart_get_char(void) {
    if (REG(UART_FR) & FR_RXFE) return -1;
    return REG(UART_DR) & 0xFF;
}

static void uart_put_char(uint8_t c) {
    while (REG(UART_FR) & FR_TXFF)
        ;
    REG(UART_DR) = c;
}

static void uart_clear_irq(void) {
    REG(UART_ICR) = 0x7FF;
}

/* ── TX flush ────────────────────────────────────────── */

static void flush_tx(void) {
    ring_buf_t *tx = (ring_buf_t *)tx_buf;
    char c;
    while (ring_get(tx, &c)) {
        uart_put_char((uint8_t)c);
    }
}

/* ── Microkit entry points ───────────────────────────── */

void init(void) {
    /* Zero both ring buffers */
    uint8_t *p;
    p = (uint8_t *)rx_buf;
    for (int i = 0; i < 0x1000; i++) p[i] = 0;
    p = (uint8_t *)tx_buf;
    for (int i = 0; i < 0x1000; i++) p[i] = 0;

    /* Enable RX interrupt (bit 4 = RXIM) */
    REG(UART_IMSC) = (1 << 4);
    uart_clear_irq();

    LOG_INFO("ready");
}

void notified(microkit_channel ch) {
    switch (ch) {

    case CH_UART_IRQ: {
        /* Hardware IRQ: read all available characters into RX ring */
        ring_buf_t *rx = (ring_buf_t *)rx_buf;
        int got_char = 0;
        int c;
        while ((c = uart_get_char()) != -1) {
            ring_put(rx, (uint8_t)c);
            got_char = 1;
        }
        uart_clear_irq();
        microkit_irq_ack(ch);

        /* Notify orchestrator if we received input */
        if (got_char) {
            microkit_notify(CH_SERIAL);
        }

        /* Also flush any pending TX while we're here */
        flush_tx();
        break;
    }

    case CH_SERIAL:
        /* Orchestrator has data to send — flush TX ring */
        flush_tx();
        break;

    default:
        break;
    }
}
