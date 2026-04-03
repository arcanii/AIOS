/*
 * AIOS Log — ring buffer with serial echo
 *
 * All root-side threads share this buffer (same VSpace).
 * /proc/log reads it; serial gets real-time echo.
 */
#include "aios/aios_log.h"
#include <stdio.h>
#include <stdint.h>

static char aios_log_ring[AIOS_LOG_RING_SIZE];
static int  aios_log_head = 0;   /* next write position */
static int  aios_log_count = 0;  /* bytes in ring (max AIOS_LOG_RING_SIZE) */

void aios_log_init(void) {
    aios_log_head = 0;
    aios_log_count = 0;
}

/* Write one char to ring buffer */
static void ring_putc(char c) {
    aios_log_ring[aios_log_head] = c;
    aios_log_head = (aios_log_head + 1) % AIOS_LOG_RING_SIZE;
    if (aios_log_count < AIOS_LOG_RING_SIZE) aios_log_count++;
}

/* Write string to ring buffer */
static void ring_puts(const char *s) {
    while (*s) ring_putc(*s++);
}

/* Write unsigned decimal to ring buffer */
static void ring_put_dec(unsigned long n) {
    char tmp[20];
    int i = 0;
    if (n == 0) { ring_putc('0'); return; }
    while (n) { tmp[i++] = '0' + (n % 10); n /= 10; }
    while (i-- > 0) ring_putc(tmp[i]);
}

/* Timestamp: HH:MM:SS from ARM generic timer */
static void ring_timestamp(void) {
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    unsigned long total_secs = (unsigned long)(cnt / freq);
    unsigned long hrs  = (total_secs / 3600) % 24;
    unsigned long mins = (total_secs / 60) % 60;
    unsigned long secs = total_secs % 60;
    char buf[10];
    buf[0] = '[';
    buf[1] = '0' + (hrs / 10);
    buf[2] = '0' + (hrs % 10);
    buf[3] = ':';
    buf[4] = '0' + (mins / 10);
    buf[5] = '0' + (mins % 10);
    buf[6] = ':';
    buf[7] = '0' + (secs / 10);
    buf[8] = '0' + (secs % 10);
    buf[9] = '\0';
    ring_puts(buf);
    ring_puts("] ");
}

static const char *level_tag(int level) {
    switch (level) {
    case LOG_LEVEL_ERROR: return "ERR";
    case LOG_LEVEL_WARN:  return "WRN";
    case LOG_LEVEL_INFO:  return "INF";
    case LOG_LEVEL_DEBUG: return "DBG";
    default:              return "???";
    }
}

void aios_log_write(int level, const char *module, const char *msg) {
    /* Ring buffer */
    ring_timestamp();
    ring_puts(level_tag(level));
    ring_putc(' ');
    ring_puts(module);
    ring_puts(": ");
    ring_puts(msg);
    ring_putc('\n');

    /* Serial echo */
    printf("[%s] %s: %s\n", level_tag(level), module, msg);
}

void aios_log_write_v(int level, const char *module, const char *msg,
                   unsigned long val) {
    /* Ring buffer */
    ring_timestamp();
    ring_puts(level_tag(level));
    ring_putc(' ');
    ring_puts(module);
    ring_puts(": ");
    ring_puts(msg);
    ring_put_dec(val);
    ring_putc('\n');

    /* Serial echo */
    printf("[%s] %s: %s%lu\n", level_tag(level), module, msg, val);
}

int aios_log_read(char *buf, int bufsize) {
    if (aios_log_count == 0) return 0;
    int start;
    if (aios_log_count < AIOS_LOG_RING_SIZE)
        start = 0;
    else
        start = aios_log_head;  /* oldest byte is at head (wrapped) */
    int n = aios_log_count < bufsize - 1 ? aios_log_count : bufsize - 1;
    for (int i = 0; i < n; i++) {
        buf[i] = aios_log_ring[(start + i) % AIOS_LOG_RING_SIZE];
    }
    buf[n] = '\0';
    return n;
}

int aios_log_len(void) {
    return aios_log_count;
}
