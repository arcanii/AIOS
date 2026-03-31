/* Command dispatch
 * Auto-extracted from orchestrator.c
 * Lines 1443-1569
 */

/* ── Command dispatch ────────────────────────────────── */
static void process_command(void) {
    input_line[input_pos] = '\0';
    if (input_pos == 0) goto prompt;

    if (my_strcmp(input_line, "help") == 0) {
        cmd_help();
    } else if (my_strcmp(input_line, "ls") == 0 ||
               my_strcmp(input_line, "dir") == 0) {
        cmd_ls("/");
    } else if (str_starts_with(input_line, "ls ")) {
        char *lp = &input_line[3];
        while (*lp == ' ') lp++;
        if (*lp == '/') cmd_ls(lp);
        else {
            char lpath[256];
            int li = 0;
            lpath[li++] = '/';
            while (*lp && li < 254) lpath[li++] = *lp++;
            lpath[li] = '\0';
            cmd_ls(lpath);
        }
    } else if (str_starts_with(input_line, "cat ")) {
        char *fn = &input_line[4];
        while (*fn == ' ') fn++;
        if (*fn) cmd_cat(fn);
        else { ser_puts("Usage: cat <file>\n"); ser_flush(); }
    } else if (str_starts_with(input_line, "write ")) {
        cmd_write(&input_line[6]);
    } else if (str_starts_with(input_line, "rm ")) {
        char *fn = &input_line[3];
        while (*fn == ' ') fn++;
        if (*fn) cmd_rm(fn);
        else { ser_puts("Usage: rm <file>\n"); ser_flush(); }
    } else if (str_starts_with(input_line, "exec ")) {
        char *p = &input_line[5];
        while (*p == ' ') p++;
        /* Check for trailing & (background) */
        int bgflag = 0;
        {
            int len = my_strlen(p);
            while (len > 0 && p[len-1] == ' ') len--;
            if (len > 0 && p[len-1] == '&') {
                p[len-1] = '\0';
                bgflag = 1;
                len--;
                while (len > 0 && p[len-1] == ' ') { p[len-1] = '\0'; len--; }
            }
        }
        /* Split "PROG.BIN arg1 arg2..." */
        char exec_file[64];
        int ei = 0;
        while (p[ei] && p[ei] != ' ' && ei < 63) { exec_file[ei] = p[ei]; ei++; }
        exec_file[ei] = '\0';
        /* Copy remaining as args */
        const char *astart = p + ei;
        while (*astart == ' ') astart++;
        int ai = 0;
        while (astart[ai] && ai < (int)(SBX_ARGS_MAX - 1)) { exec_args[ai] = astart[ai]; ai++; }
        exec_args[ai] = '\0';
        /* Append & back to filename if background */
        if (bgflag) {
            int fl = my_strlen(exec_file);
            if (fl < 62) { exec_file[fl] = ' '; exec_file[fl+1] = '&'; exec_file[fl+2] = '\0'; }
        }
        if (exec_file[0]) {
            cmd_exec(exec_file);
            input_pos = 0;
            return;
        }
        else { ser_puts("Usage: exec <file.bin> [args]\n"); ser_flush(); }
    } else if (str_starts_with(input_line, "load ")) {
        char *fn = &input_line[5];
        while (*fn == ' ') fn++;
        if (*fn) cmd_load(fn);
        else { ser_puts("Usage: load <file>\n"); ser_flush(); }
    } else if (str_starts_with(input_line, "gen ")) {
        char *p = &input_line[4];
        while (*p == ' ') p++;
        if (*p) {
            cmd_gen(p);
            input_pos = 0;
            return; /* Don't print prompt — waiting for LLM */
        }
        else { ser_puts("Usage: gen <prompt>\n"); ser_flush(); }
    } else if (my_strcmp(input_line, "info") == 0) {
        cmd_info();
    } else if (my_strcmp(input_line, "ps") == 0) {
        cmd_ps();
    } else if (my_strcmp(input_line, "whoami") == 0) {
        ser_puts(current_username);
        ser_puts(" (uid=");
        ser_put_dec(current_uid);
        ser_puts(")\n");
        ser_flush();
    } else if (str_starts_with(input_line, "chmod ")) {
        cmd_chmod(input_line + 6);
    } else if (str_starts_with(input_line, "chown ")) {
        cmd_chown(input_line + 6);
    } else if (str_starts_with(input_line, "stat ")) {
        cmd_stat_ex(input_line + 5);
    } else if (str_starts_with(input_line, "useradd ")) {
        cmd_useradd(input_line + 8);
    } else if (str_starts_with(input_line, "passwd")) {
        cmd_passwd(input_line + 6);
    } else if (str_starts_with(input_line, "su")) {
        cmd_su(input_line + 2);
    } else if (my_strcmp(input_line, "logout") == 0 || my_strcmp(input_line, "exit") == 0) {
        auth_logout_sync();
        auth_state = AUTH_LOGIN_USER;
        input_pos = 0;
        input_line[0] = '\0';
        ser_puts("Logged out.\n\nLogin: ");
        ser_flush();
        return;
    } else if (str_starts_with(input_line, "kill ")) {
        cmd_kill(input_line + 5);
    } else if (my_strcmp(input_line, "groups") == 0) {
        cmd_groups();
    } else if (str_starts_with(input_line, "usermod ")) {
        cmd_usermod(input_line + 8);
    } else if (my_strcmp(input_line, "shutdown") == 0) {
        if (auth_check_priv_sync() != 0) {
            ser_puts("Permission denied: root required\n");
            ser_flush();
        } else {
            cmd_shutdown();
        }
    } else {
        ser_puts("Unknown command: ");
        ser_puts(input_line);
        ser_puts("\nType 'help' for commands.\n");
        ser_flush();
    }

prompt:
    print_prompt();
    input_pos = 0;
}
