/* Entry points: init, notified
 * Auto-extracted from orchestrator.c
 * Lines 2427-2644
 */

/* ── Microkit entry points ─────────────────────────── */
void init(void) {
    ser_puts("\n============ Open Aries ================\n");
    ser_puts("  " AIOS_VERSION_FULL "\n");
    ser_puts("  Kernel:  seL4 14.0.0 (Microkit 2.1.0)\n");
    ser_puts("  CPUs:    ");
    ser_put_dec(CONFIG_MAX_NUM_NODES);
    ser_puts(" cores (SMP)\n");
    ser_puts("  Sandbox: ");
    ser_put_dec(NUM_SANDBOXES);
    ser_puts(" process slots\n");
    const char *fsname = fs_fsinfo_sync();
    ser_puts("  FS:      ");
    ser_puts(fsname);
    ser_puts("\n");
    ser_puts("  Drivers: PL011 UART, virtio-blk, virtio-net\n");
    ser_puts("  LLM:     llm_server (llama2.c engine)\n");
    ser_puts("============ Open Aries ================\n\n");
    ser_flush();

    /* Initialize sandbox slot tables */
    sbx_io[0] = sbx0_io; sbx_code[0] = sbx0_code;
    sbx_io[1] = sbx1_io; sbx_code[1] = sbx1_code;
    sbx_io[2] = sbx2_io; sbx_code[2] = sbx2_code;
    sbx_io[3] = sbx3_io; sbx_code[3] = sbx3_code;
    sbx_io[4]   = sbx4_io;
    sbx_code[4] = sbx4_code;
    sbx_io[5]   = sbx5_io;
    sbx_code[5] = sbx5_code;
    sbx_io[6]   = sbx6_io;
    sbx_code[6] = sbx6_code;
    sbx_io[7]   = sbx7_io;
    sbx_code[7] = sbx7_code;
    sbx_heap[0] = sbx0_heap; sbx_heap[1] = sbx1_heap;
    sbx_heap[2] = sbx2_heap; sbx_heap[3] = sbx3_heap;
    sbx_heap[4] = sbx4_heap; sbx_heap[5] = sbx5_heap;
    sbx_heap[6] = sbx6_heap; sbx_heap[7] = sbx7_heap;
    sched_init(&sched);

    /* Boot: read license.txt synchronously */
    ser_puts("Boot: reading license.txt...\n");
    ser_flush();
    int st = fs_open_sync("license.txt");
    if (st == 0) {
        uint32_t fd   = RD32(fs_data, FS_FD);
        uint32_t size = RD32(fs_data, FS_FILESIZE);
        st = fs_read_sync(fd, 0, size < FS_DATA_MAX ? size : FS_DATA_MAX);
        if (st == 0 || st == 3) {
            uint32_t got = RD32(fs_data, FS_LENGTH);
            volatile char *data = (volatile char *)(fs_data + FS_DATA);
            ser_puts("  ");
            for (uint32_t i = 0; i < got; i++) ser_putc(data[i]);
            ser_puts("\n");
        }
        fs_close_sync(fd);
    } else {
        ser_puts("  license.txt not found\n");
    }
    ser_flush();

    /* Check for autoexec.bin */
    st = fs_open_sync("autoexec.bin");
    if (st == 0) {
        uint32_t fd = RD32(fs_data, FS_FD);
        fs_close_sync(fd);
        ser_puts("Boot: found autoexec.bin, executing...\n");
        ser_flush();
        cmd_exec("autoexec.bin");
        return;
    }


    /* Load /etc/passwd into auth_server */
    ser_puts("Boot: loading /etc/passwd...\n");
    ser_flush();
    st = fs_open_sync("/etc/passwd");
    if (st == 0) {
        uint32_t fd = RD32(fs_data, FS_FD);
        uint32_t sz = RD32(fs_data, FS_FILESIZE);
        if (sz > 3584) sz = 3584;
        fs_read_sync(fd, 0, sz);
        /* Copy file data to auth_io DATA area */
        volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
        volatile uint8_t *dst = (volatile uint8_t *)(auth_io + AUTH_DATA);
        for (uint32_t i = 0; i < sz; i++) dst[i] = src[i];
        dst[sz] = 0;
        fs_close_sync(fd);
        /* Tell auth_server to parse it */
        WR32(auth_io, AUTH_CMD, AUTH_CMD_LOAD_PASSWD);
        microkit_ppcall(CH_AUTH, microkit_msginfo_new(0, 0));
        ser_puts("  Loaded user database\n");
    } else {
        ser_puts("  /etc/passwd not found, using defaults\n");
    }
    ser_flush();

    /* Read /etc/hostname */
    st = fs_open_sync("/etc/hostname");
    if (st == 0) {
        uint32_t hfd = RD32(fs_data, FS_FD);
        uint32_t hsz = RD32(fs_data, FS_FILESIZE);
        if (hsz > 63) hsz = 63;
        fs_read_sync(hfd, 0, hsz);
        volatile char *hdata = (volatile char *)(fs_data + FS_DATA);
        int hi = 0;
        while (hi < (int)hsz && hdata[hi] && hdata[hi] != '\n') {
            hostname[hi] = hdata[hi]; hi++;
        }
        hostname[hi] = '\0';
        fs_close_sync(hfd);
    }

    /* Read and display /etc/motd */
    st = fs_open_sync("/etc/motd");
    if (st == 0) {
        uint32_t mfd = RD32(fs_data, FS_FD);
        uint32_t msz = RD32(fs_data, FS_FILESIZE);
        if (msz > 2048) msz = 2048;
        fs_read_sync(mfd, 0, msz);
        uint32_t mgot = RD32(fs_data, FS_LENGTH);
        volatile char *mdata = (volatile char *)(fs_data + FS_DATA);
        for (uint32_t mi = 0; mi < mgot; mi++) ser_putc(mdata[mi]);
        fs_close_sync(mfd);
    }
    ser_flush();

    LOG_INFO("system ready, awaiting login");
    ser_puts("\nLogin: ");
    ser_flush();
}

void notified(microkit_channel ch) {
    switch (ch) {
    case CH_SERIAL:
        if (auth_state != AUTH_AUTHENTICATED) {
            handle_login_input();
        } else if (orch_state == RUNNING) {
            handle_serial_input();
        } else if (orch_state == EXEC_RUNNING) {
            /* Scan for Ctrl-C without consuming other characters */
            ring_buf_t *rx_scan = (ring_buf_t *)rx_buf;
            uint32_t scan_ri = rx_scan->read_idx;
            uint32_t scan_wi = rx_scan->write_idx;
            while (scan_ri != scan_wi) {
                char cc = (char)rx_scan->data[scan_ri];
                if (cc == 0x03) { /* Ctrl-C found */
                    /* Drain buffer up to and including Ctrl-C */
                    rx_scan->read_idx = (scan_ri + 1) % RING_SIZE;
                    /* Kill foreground process */
                    for (int s = 0; s < NUM_SANDBOXES; s++) {
                        if (slot_in_use(s) && slot_proc(s)->foreground) {
                            microkit_pd_stop(s);
                            microkit_pd_restart(s, SBX_ENTRY_POINT);
                            ser_puts("^C\n");
                            ser_puts("  Interrupted PID ");
                            ser_put_dec(slot_proc(s)->pid);
                            ser_puts("\n");
                            ser_flush();
                            sched_release_slot(&sched, s);
        try_load_queued(s);
                            orch_state = RUNNING;
            LOG_INFO("user authenticated");
                            print_prompt();
                            break;
                        }
                    }
                    break;
                }
                scan_ri = (scan_ri + 1) % RING_SIZE;
            }
        }
        break;

    case CH_SBX0:
    case CH_SBX1:
    case CH_SBX2:
    case CH_SBX3:
    case CH_SBX4:
    case CH_SBX5:
    case CH_SBX6:
    case CH_SBX7: {
        int slot = -1;
        for (int i = 0; i < NUM_SANDBOXES; i++) {
            if (sbx_channel[i] == (int)ch) { slot = i; break; }
        }
        if (slot < 0) break;

        /* Check sandbox status — handle completion regardless of orch_state */
        {
            uintptr_t sio = sbx_io[slot];
            uint32_t status = RD32(sio, SBX_STATUS);

            if (status == SBX_ST_DONE) {
                /* Process exited */
                handle_exec_done(slot);
            } else if (status == SBX_ST_SUSPENDED) {
                /* Process suspended — slot already freed by SYS_SUSPENDED handler */
                /* Nothing to do here */
            } else if (orch_state == EXEC_RUNNING) {
                /* Legacy path: sandbox finished run_program normally */
                handle_exec_done(slot);
            }
        }
        break;
    }

    case CH_LLM:
        if (orch_state == LLM_INIT_MODEL) {
            if (RD32(llm_io, LLM_STATUS) != 0) {
                ser_puts("  Model init FAILED\n");
                ser_flush();
                orch_state = RUNNING;
            } else {
                ser_puts("  Model initialized\n");
                ser_flush();
                if (saved_tok_size > 0) {
                    WR32(llm_io, LLM_TOK_OFF, saved_tok_offset);
                    WR32(llm_io, LLM_TOK_SZ, saved_tok_size);
                    WR32(llm_io, LLM_CMD, LLM_CMD_LOAD_TOK);
                    orch_state = LLM_INIT_TOK;
                    microkit_notify(CH_LLM);
                } else {
                    llm_model_loaded = 1;
                    ser_puts("  LLM ready!\n");
                    print_prompt();
                    orch_state = RUNNING;
                }
            }
        } else if (orch_state == LLM_INIT_TOK) {
            if (RD32(llm_io, LLM_STATUS) != 0) {
                ser_puts("  Tokenizer init FAILED\n");
                ser_flush();
            } else {
                ser_puts("  Tokenizer initialized\n");
                ser_flush();
            }
            llm_model_loaded = 1;
            ser_puts("  LLM ready!\n");
            print_prompt();
            ser_flush();
            orch_state = RUNNING;
        } else if (orch_state == GEN_WAITING)
            handle_gen_reply();
        break;

    case CH_ECHO:
        handle_log_message((volatile uint8_t *)echo_io);
        break;

    default:
        break;
    }
}

seL4_Bool fault(microkit_child child, microkit_msginfo msginfo, microkit_msginfo *reply_msginfo) {
    (void)msginfo;
    (void)reply_msginfo;
    int slot = (int)child;
    if (slot >= 0 && slot < NUM_SANDBOXES) {
        {
            /* Save info before releasing slot */
            sched_proc_t *p = slot_proc(slot);
            uint32_t crashed_pid = p ? p->pid : 0;
            int was_fg = p ? p->foreground : 0;
            /* Log fault details (seL4 VMFault format) */
            uint64_t fault_ip   = seL4_GetMR(seL4_VMFault_IP);
            uint64_t fault_addr = seL4_GetMR(seL4_VMFault_Addr);
            uint64_t fault_type = seL4_GetMR(seL4_VMFault_PrefetchFault);
            uint64_t fault_fsr  = seL4_GetMR(seL4_VMFault_FSR);
            sched_release_slot(&sched, slot);
            microkit_pd_restart(slot, SBX_ENTRY_POINT);
            ser_puts("FAULT: sandbox ");
            ser_put_dec((unsigned int)slot);
            ser_puts(" crashed (PID ");
            ser_put_dec(crashed_pid);
            ser_puts("), slot freed\n");
            ser_puts("  IP=0x"); ser_put_hex((unsigned long)fault_ip);
            ser_puts(" ADDR=0x"); ser_put_hex((unsigned long)fault_addr);
            ser_puts(" FSR=0x"); ser_put_hex((unsigned long)fault_fsr);
            ser_puts(fault_type ? " (prefetch)" : " (data)");
            ser_puts("\n");
            ser_flush();
            if (was_fg) {
                orch_state = RUNNING;
                print_prompt();
                ser_flush();
            }
            try_load_queued(slot);
        }
    }
    return seL4_False;
}
