/* Serial I/O
 * Auto-extracted from orchestrator.c
 * Lines 105-126
 */

/* ── Serial I/O ──────────────────────────────────────── */
static void ser_putc(char c) {
    ring_buf_t *tx = (ring_buf_t *)tx_buf;
    ring_put(tx, (uint8_t)c);
}
static void ser_puts(const char *s) {
    ring_buf_t *tx = (ring_buf_t *)tx_buf;
    while (*s) ring_put(tx, (uint8_t)*s++);
}
static void ser_put_dec(unsigned int n) {
    char buf[12]; int i = 0;
    if (n == 0) { ser_putc('0'); return; }
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i--) ser_putc(buf[i]);
}
static __attribute__((unused)) void ser_put_hex(unsigned long n) {
    char buf[17]; int i = 0;
    if (n == 0) { ser_puts("0"); return; }
    while (n) { int d = n & 0xf; buf[i++] = d < 10 ? '0'+d : 'a'+d-10; n >>= 4; }
    while (i--) ser_putc(buf[i]);
}
#define ser_flush() microkit_notify(CH_SERIAL)
