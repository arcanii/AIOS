/* Input handlers and exec completion
 * Auto-extracted from orchestrator.c
 * Lines 1571-1797
 */

/* ── Serial input handler ────────────────────────────── */
static void handle_exec_done(int slot);
/* ── Login input handler ───────────────────────────── */
static char login_buf[64];
static int  login_pos = 0;

static void handle_login_input(void) {
    ring_buf_t *rx = (ring_buf_t *)rx_buf;
    char c;
    while (ring_get(rx, &c)) {
        if (auth_state == AUTH_LOGIN_USER) {
            if (c == '\r' || c == '\n') {
                login_buf[login_pos] = '\0';
                ser_puts("\n");
                if (login_pos > 0) {
                    /* Trim leading spaces */
                    int si = 0;
                    while (login_buf[si] == ' ' && si < login_pos) si++;
                    int i = 0;
                    while (login_buf[si] && i < 31) {
                        login_username[i] = login_buf[si]; i++; si++;
                    }
                    login_username[i] = '\0';
                    auth_state = AUTH_LOGIN_PASS;
                    ser_puts("Password: ");
                    ser_flush();
                } else {
                    ser_puts("Login: ");
                    ser_flush();
                }
                login_pos = 0;
            } else if (c == 127 || c == '\b') {
                if (login_pos > 0) {
                    login_pos--;
                    ser_puts("\b \b");
                    ser_flush();
                }
            } else if (login_pos < 62) {
                login_buf[login_pos++] = c;
                ser_putc(c);  /* echo username */
                ser_flush();
            }
        } else if (auth_state == AUTH_LOGIN_PASS) {
            if (c == '\r' || c == '\n') {
                login_buf[login_pos] = '\0';
                ser_puts("\n");
                ser_flush();
                int rc = auth_login_sync(login_username, login_buf);
                /* Clear password buffer */
                for (int i = 0; i < 64; i++) login_buf[i] = 0;
                login_pos = 0;
                if (rc == 0) {
                    auth_state = AUTH_AUTHENTICATED;
                    ser_puts("\nWelcome, ");
                    ser_puts(current_username);
                    if (current_is_root) ser_puts(" (root)");
                    ser_puts("\n\n");
                    /* auto-launch user shell from /etc/passwd */
                    if (current_shell[0] != '\0' &&
                        my_strcmp(current_shell, "/bin/osh") != 0) {
                        /* Launch external shell program */
                        cmd_exec(current_shell);
                        /* Track which slot is the login shell */
                        for (int ss = 0; ss < NUM_SANDBOXES; ss++) {
                            if (slot_in_use(ss) && slot_proc(ss)->foreground) {
                                session_shell_slot = ss;
                                break;
                            }
                        }
                    } else {
                        /* Built-in orchestrator shell */
                        print_prompt();
                    }
                } else {
                    ser_puts("Login incorrect\n\n");
                    ser_puts("Login: ");
                    ser_flush();
                    auth_state = AUTH_LOGIN_USER;
                }
            } else if (c == 127 || c == '\b') {
                if (login_pos > 0) {
                    login_pos--;
                    ser_puts("\b \b");
                    ser_flush();
                }
            } else if (login_pos < 62) {
                login_buf[login_pos++] = c;
                ser_puts("*");  /* mask password */
                ser_flush();
            }
        }
    }
}


static void print_prompt(void) {
    ser_puts(current_username);
    ser_puts("@");
    ser_puts(hostname);
    if (current_is_root)
        ser_puts("# ");
    else
        ser_puts("$ ");
    ser_flush();
}

static void handle_serial_input(void) {
    ring_buf_t *rx = (ring_buf_t *)rx_buf;
    char c;
    while (ring_get(rx, &c)) {
        /* Ctrl-C: interrupt foreground process */
        if (c == 0x03) {
            if (orch_state == EXEC_RUNNING) {
                /* Find foreground process and kill it */
                for (int s = 0; s < NUM_SANDBOXES; s++) {
                    if (slot_in_use(s) && slot_proc(s)->foreground) {
                        microkit_pd_stop(s);
                        microkit_pd_restart(s, SBX_ENTRY_POINT);
                        ser_puts("^C\n");
                        ser_puts("  Interrupted PID ");
                        ser_put_dec(slot_proc(s)->pid);
                        ser_puts(" (");
                        ser_puts(slot_proc(s)->name);
                        ser_puts(")\n");
                        sched_release_slot(&sched, s);
        try_load_queued(s);
                        orch_state = RUNNING;
                        print_prompt();
                        break;
                    }
                }
            } else {
                /* Just print ^C and new prompt */
                ser_puts("^C\n");
                input_pos = 0;
                input_line[0] = '\0';
                print_prompt();
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            ser_puts("\n"); ser_flush();
            process_command();
        } else if (c == 0x7f || c == '\b') {
            if (input_pos > 0) {
                input_pos--;
                ser_puts("\b \b"); ser_flush();
            }
        } else if (input_pos < INPUT_MAX - 1) {
            input_line[input_pos++] = c;
            ser_putc(c); ser_flush();
        }
    }
}

/* ── Handle sandbox completion ───────────────────────── */
static void handle_exec_done(int slot) {
    if (!slot_in_use(slot)) return;  /* already cleaned up */
    sched_proc_t *proc = slot_proc(slot);
    if (!proc) return;
    uintptr_t sio = sbx_io[slot];
    WR32(sio, SBX_STATUS, SBX_ST_IDLE);  /* clear status immediately to prevent re-entry */
    uint32_t exit_code = RD32(sio, SBX_EXIT_CODE);
    uint32_t out_len = RD32(sio, SBX_OUTPUT_LEN);
    int is_fg = proc->foreground;

    if (is_fg) {
        if (out_len > 0) {
            ser_puts("--- program output ---\n");
            ser_flush();
            volatile char *out = (volatile char *)(sio + SBX_OUTPUT);
            for (uint32_t i = 0; i < out_len && i < 4096; i++) ser_putc(out[i]);
            ser_puts("\n--- end output ---\n");
            ser_flush();
        }
        ser_puts("  Exit code: ");
        ser_put_dec(exit_code);
        ser_puts("\n");
        ser_flush();
    }
    if (slot_proc(slot)->foreground) {
        if (slot == session_shell_slot) {
            /* Login shell exited — respawn login prompt */
            session_shell_slot = -1;
            orch_state = RUNNING;
            auth_logout_sync();
            auth_state = AUTH_LOGIN_USER;
            ser_puts("\n\nLogin: ");
            ser_flush();
        } else {
            orch_state = RUNNING;
            print_prompt();
        }
    } else {
        ser_puts("\n[");
        ser_put_dec((unsigned int)slot);
        ser_puts("] Done (PID ");
        ser_put_dec(slot_proc(slot)->pid);
        ser_puts(", exit ");
        ser_put_dec(exit_code);
        ser_puts(")\n");
    }
        /* Record exit for waitpid */
    for (int ei = 0; ei < MAX_EXIT_QUEUE; ei++) {
        if (!exit_queue[ei].active) {
            exit_queue[ei].active = 1;
            exit_queue[ei].pid = slot_proc(slot)->pid;
            exit_queue[ei].parent_pid = slot_proc(slot)->parent_pid;
            exit_queue[ei].exit_code = (int32_t)exit_code;
            break;
        }
    }
    /* Stop and restart sandbox PD so it re-enters notified() cleanly */
    microkit_pd_stop(slot);
    microkit_pd_restart(slot, SBX_ENTRY_POINT);
    sched_exit(&sched, slot, (int)exit_code);
    try_load_queued(slot);
}

/* ── Handle LLM generation reply ─────────────────────── */
static void handle_gen_reply(void) {
    uint32_t status = RD32(llm_io, LLM_STATUS);
    if (status == LLM_ST_TOKEN) {
        volatile char *tok = (volatile char *)(llm_io + LLM_OUTPUT);
        for (int i = 0; tok[i] && i < 256; i++) ser_putc(tok[i]);
        ser_flush();
        /* Request next token */
        WR32(llm_io, LLM_CMD, LLM_CMD_NEXT_TOK);
        microkit_notify(CH_LLM);
    } else if (status == LLM_ST_DONE) {
        ser_puts("\n[done]\n");
        ser_flush();
        orch_state = RUNNING;
        print_prompt();
    } else {
        ser_puts("\n[LLM error]\n");
        ser_flush();
        orch_state = RUNNING;
        print_prompt();
    }
}
