/* Input handlers and exec completion
 * Auto-extracted from orchestrator.c
 * Lines 1571-1797
 */

/* ── Serial input handler ────────────────────────────── */
static char rx_getc_blocking(void) {
    ring_buf_t *rx = (ring_buf_t *)rx_buf;
    char c;
    while (!ring_get(rx, &c)) {
        asm volatile("wfe");
    }
    /* Ctrl-C: kill foreground process */
    if (c == 0x03 && orch_state == EXEC_RUNNING) {
        for (int s = 0; s < NUM_SANDBOXES; s++) {
            if (proctab[s].in_use && proctab[s].foreground) {
                microkit_pd_stop(s);
                microkit_pd_restart(s, SBX_ENTRY_POINT);
                ser_puts("^C\n");
                proctab[s].in_use = 0;
                orch_state = RUNNING;
                print_prompt();
                /* Return EOF-like value */
                return (char)-1;
            }
        }
    }
    return c;
}


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
                    print_prompt();
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
    if (current_is_root)
        ser_puts("@aios# ");
    else
        ser_puts("@aios$ ");
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
                    if (proctab[s].in_use && proctab[s].foreground) {
                        microkit_pd_stop(s);
                        microkit_pd_restart(s, SBX_ENTRY_POINT);
                        ser_puts("^C\n");
                        ser_puts("  Interrupted PID ");
                        ser_put_dec(proctab[s].pid);
                        ser_puts(" (");
                        ser_puts(proctab[s].name);
                        ser_puts(")\n");
                        proctab[s].in_use = 0;
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
    uintptr_t sio = sbx_io[slot];
    (void)RD32(sio, SBX_STATUS);
    uint32_t exit_code = RD32(sio, SBX_EXIT_CODE);
    uint32_t out_len = RD32(sio, SBX_OUTPUT_LEN);

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
    if (proctab[slot].foreground) {
        orch_state = RUNNING;
        print_prompt();
    } else {
        ser_puts("\n[");
        ser_put_dec((unsigned int)slot);
        ser_puts("] Done (PID ");
        ser_put_dec(proctab[slot].pid);
        ser_puts(", exit ");
        ser_put_dec(exit_code);
        ser_puts(")\n");
    }
        /* Record exit for waitpid */
    for (int ei = 0; ei < MAX_EXIT_QUEUE; ei++) {
        if (!exit_queue[ei].active) {
            exit_queue[ei].active = 1;
            exit_queue[ei].pid = proctab[slot].pid;
            exit_queue[ei].parent_pid = proctab[slot].parent_pid;
            exit_queue[ei].exit_code = (int32_t)exit_code;
            break;
        }
    }
    proctab[slot].in_use = 0;
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
