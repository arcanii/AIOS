/* ── IPC Log Message Handler ────────────────────────────── */

static void handle_log_message(volatile uint8_t *io_base) {
    uint32_t cmd = RD32(io_base, 0x000);
    if (cmd != CMD_LOG) return;

    /* Copy message to local buffer to avoid volatile issues */
    char buf[LOG_MSG_MAX + 1];
    volatile char *src = (volatile char *)(io_base + LOG_MSG_OFF);
    int i;
    for (i = 0; i < LOG_MSG_MAX && src[i]; i++) buf[i] = src[i];
    buf[i] = '\0';

    ser_puts(buf);

    /* Clear the command */
    WR32(io_base, 0x000, 0);
}
