/* LLM commands (load, gen, info, shutdown)
 * Auto-extracted from orchestrator.c
 * Lines 1295-1441
 */

/* ── Command: load <model> ───────────────────────────── */
static void cmd_load(const char *filename) {
    ser_puts("Loading model: ");
    ser_puts(filename);
    ser_puts("\n");
    ser_flush();

    int st = fs_open_sync(filename);
    if (st != 0) {
        ser_puts("  File not found.\n"); ser_flush();
        return;
    }
    uint32_t fd   = RD32(fs_data, FS_FD);
    uint32_t size = RD32(fs_data, FS_FILESIZE);
    ser_puts("  File size: ");
    ser_put_dec(size);
    ser_puts(" bytes (");
    ser_put_dec(size / 1024);
    ser_puts(" KiB)\n  Loading: [");
    ser_flush();

    /* Load into model_data region */
    volatile uint8_t *llm_dst = (volatile uint8_t *)model_data;
    uint32_t offset = 0;
    uint32_t last_pct = 0;
    while (offset < size) {
        uint32_t chunk = size - offset;
        if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
        st = fs_read_sync(fd, offset, chunk);
        if (st != 0 && st != 3) break;
        uint32_t got = RD32(fs_data, FS_LENGTH);
        if (got == 0) break;
        volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
        for (uint32_t i = 0; i < got; i++) llm_dst[offset + i] = src[i];
        offset += got;
        /* Progress bar: 50 '#' marks */
        uint32_t pct = (uint32_t)((uint64_t)offset * 50 / size);
        while (last_pct < pct) { ser_putc('#'); ser_flush(); last_pct++; }
    }
    ser_puts("] 100%\n");
    ser_flush();
    fs_close_sync(fd);

    ser_puts("  Loaded ");
    ser_put_dec(offset);
    ser_puts(" bytes (");
    ser_put_dec(offset / 1024);
    ser_puts(" KiB)\n");
    ser_flush();

    /* Tell LLM server model is loaded */
    WR32(llm_io, LLM_MODELSZ, offset);
    WR32(llm_io, LLM_CMD, LLM_CMD_LOAD_DONE);
    microkit_notify(CH_LLM);
    ser_puts("  Model weights loaded (");
    ser_put_dec(offset / 1024);
    ser_puts(" KiB)\n");
    ser_flush();

    /* Now load tokenizer (TOK.BIN) right after model in model_data */
    uint32_t tok_offset = offset;
    uint32_t tok_size = 0;
    st = fs_open_sync("TOK.BIN");
    if (st != 0) {
        ser_puts("  TOK.BIN not found, skipping tokenizer\n");
        ser_flush();
    } else {
        uint32_t tok_fd   = RD32(fs_data, FS_FD);
        tok_size = RD32(fs_data, FS_FILESIZE);
        ser_puts("  Loading tokenizer: ");
        ser_put_dec(tok_size);
        ser_puts(" bytes\n");
        ser_flush();

        uint32_t tok_off = 0;
        while (tok_off < tok_size) {
            uint32_t chunk = tok_size - tok_off;
            if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
            st = fs_read_sync(tok_fd, tok_off, chunk);
            if (st != 0) break;
            uint32_t got = RD32(fs_data, FS_LENGTH);
            if (got == 0) break;
            volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
            for (uint32_t i = 0; i < got; i++)
                llm_dst[tok_offset + tok_off + i] = src[i];
            tok_off += got;
        }
        fs_close_sync(tok_fd);

        ser_puts("  Tokenizer loaded\n");
        ser_flush();

    }

    saved_tok_offset = tok_offset;
    saved_tok_size = tok_size;

    /* Send model init command — response handled in notified() */
    WR32(llm_io, LLM_MODELSZ, tok_offset);
    WR32(llm_io, LLM_CMD, LLM_CMD_LOAD_DONE);
    orch_state = LLM_INIT_MODEL;
    microkit_notify(CH_LLM);
    ser_puts("  Initializing model...\n");
    ser_flush();
}

/* ── Command: gen <prompt> ───────────────────────────── */
static void cmd_gen(const char *prompt) {
    if (!llm_model_loaded) {
        ser_puts("  No model loaded. Use: load CODE25M.BIN\n");
        ser_flush();
        return;
    }
    /* Copy prompt to llm_io */
    volatile char *dst = (volatile char *)(llm_io + LLM_PROMPT);
    int i = 0;
    while (prompt[i] && i < 255) { dst[i] = prompt[i]; i++; }
    dst[i] = '\0';
    WR32(llm_io, LLM_CMD, LLM_CMD_GENERATE);
    orch_state = GEN_WAITING;
    microkit_notify(CH_LLM);
}

/* ── Command: info ───────────────────────────────────── */
static void cmd_info(void) {
    ser_puts("Open Aries System Information:\n");
    ser_puts("  Kernel:     seL4 14.0.0 (Microkit 2.1.0)\n");
    ser_puts("  Arch:       AArch64 (Cortex-A53)\n");
    ser_puts("  RAM:        2 GiB\n");
    ser_puts("  FS:         ");
    ser_puts(fs_fsinfo_sync());
    ser_puts("\n");
    ser_puts("  Drivers:    PL011 UART, virtio-blk\n");
    ser_puts("  Services:   fs_server, llm_server, sandbox\n");
    ser_puts("  LLM:        ");
    if (llm_model_loaded) ser_puts("loaded\n");
    else ser_puts("not loaded\n");
    ser_flush();
}

/* ── Command: shutdown ───────────────────────────────── */
static void cmd_shutdown(void) {
    ser_puts("Shutting down AIOS...\n");
    ser_puts("System halted. Press Ctrl-A then X to exit QEMU.\n");
    ser_flush();
    while (1) { /* halt */ }
}
