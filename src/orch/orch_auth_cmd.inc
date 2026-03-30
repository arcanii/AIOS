/* Auth server helpers and user commands
 * Auto-extracted from orchestrator.c
 * Lines 447-804
 */

/* ── Auth server helpers (PPC via CH_AUTH) ──────────── */
static int auth_login_sync(const char *username, const char *password) {
    volatile char *u = (volatile char *)(auth_io + AUTH_USERNAME);
    volatile char *p = (volatile char *)(auth_io + AUTH_PASSWORD);
    int i = 0;
    while (username[i] && i < 31) { u[i] = username[i]; i++; }
    u[i] = '\0';
    i = 0;
    while (password[i] && i < 63) { p[i] = password[i]; i++; }
    p[i] = '\0';
    WR32(auth_io, AUTH_CMD, AUTH_CMD_LOGIN);
    microkit_ppcall(CH_AUTH, microkit_msginfo_new(0, 0));
    int status = (int)(int32_t)RD32(auth_io, AUTH_STATUS);
    if (status == 0) {
        current_session = RD32(auth_io, AUTH_SESSION);
        current_uid = RD32(auth_io, AUTH_UID);
        current_gid = RD32(auth_io, AUTH_GID);
        current_is_root = (current_uid == 0) ? 1 : 0;
        /* Copy username */
        int j = 0;
        while (username[j] && j < 31) { current_username[j] = username[j]; j++; }
        current_username[j] = '\0';
    }
    return status;
}

static void print_prompt(void);

static int auth_check_file_sync(const char *path, uint32_t mode) {
    /* First get file permissions from filesystem */
    uint32_t fuid = 0, fgid = 0, fmode = 0;
    fs_stat_ex_sync(path, 0, &fuid, &fgid, &fmode);

    volatile char *dst = (volatile char *)(auth_io + AUTH_PATH);
    int i = 0;
    while (path[i] && i < 63) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    WR32(auth_io, AUTH_SESSION, current_session);
    WR32(auth_io, AUTH_MODE, mode);
    /* Pass file's uid/gid/mode to auth for permission check */
    WR32(auth_io, AUTH_UID, fuid);
    WR32(auth_io, AUTH_GID, fgid);
    /* Encode file perms in upper 16 bits of MODE */
    WR32(auth_io, AUTH_MODE, mode | (fmode << 16));
    WR32(auth_io, AUTH_CMD, AUTH_CMD_CHECK_FILE);
    microkit_ppcall(CH_AUTH, microkit_msginfo_new(0, 0));
    return (int)(int32_t)RD32(auth_io, AUTH_STATUS);
}

static int __attribute__((unused)) auth_check_priv_sync(void) {
    WR32(auth_io, AUTH_SESSION, current_session);
    WR32(auth_io, AUTH_CMD, AUTH_CMD_CHECK_PRIV);
    microkit_ppcall(CH_AUTH, microkit_msginfo_new(0, 0));
    return (int)(int32_t)RD32(auth_io, AUTH_STATUS);
}

static int __attribute__((unused)) auth_check_kill_sync(uint32_t target_uid) {
    WR32(auth_io, AUTH_SESSION, current_session);
    WR32(auth_io, AUTH_UID, target_uid);
    WR32(auth_io, AUTH_CMD, AUTH_CMD_CHECK_KILL);
    microkit_ppcall(CH_AUTH, microkit_msginfo_new(0, 0));
    return (int)(int32_t)RD32(auth_io, AUTH_STATUS);
}

static void auth_logout_sync(void) {
    WR32(auth_io, AUTH_SESSION, current_session);
    WR32(auth_io, AUTH_CMD, AUTH_CMD_LOGOUT);
    microkit_ppcall(CH_AUTH, microkit_msginfo_new(0, 0));
    current_session = 0;
    current_uid = 0;
    current_gid = 0;
    current_is_root = 0;
    current_username[0] = '\0';
}

static void __attribute__((unused)) auth_whoami_sync(void) {
    WR32(auth_io, AUTH_SESSION, current_session);
    WR32(auth_io, AUTH_CMD, AUTH_CMD_WHOAMI);
    microkit_ppcall(CH_AUTH, microkit_msginfo_new(0, 0));
}


/* ── Command: useradd <username> <password> <uid> ──── */
static void cmd_useradd(const char *args) {
    if (auth_check_priv_sync() != 0) {
        ser_puts("Permission denied: root required\n");
        ser_flush();
        return;
    }
    /* Parse: username password uid */
    char uname[32], passwd[64];
    uint32_t uid = 0;
    int i = 0, j;
    while (args[i] == ' ') i++;
    j = 0;
    while (args[i] && args[i] != ' ' && j < 31) uname[j++] = args[i++];
    uname[j] = '\0';
    while (args[i] == ' ') i++;
    j = 0;
    while (args[i] && args[i] != ' ' && j < 63) passwd[j++] = args[i++];
    passwd[j] = '\0';
    while (args[i] == ' ') i++;
    while (args[i] >= '0' && args[i] <= '9') { uid = uid * 10 + (args[i] - '0'); i++; }

    if (uname[0] == '\0' || passwd[0] == '\0') {
        ser_puts("Usage: useradd <username> <password> <uid>\n");
        ser_flush();
        return;
    }
    if (uid == 0) uid = 1001; /* default uid */

    /* Send to auth_server */
    volatile char *u = (volatile char *)(auth_io + AUTH_USERNAME);
    volatile char *p = (volatile char *)(auth_io + AUTH_PASSWORD);
    j = 0; while (uname[j]) { u[j] = uname[j]; j++; } u[j] = '\0';
    j = 0; while (passwd[j]) { p[j] = passwd[j]; j++; } p[j] = '\0';
    WR32(auth_io, AUTH_SESSION, current_session);
    WR32(auth_io, AUTH_UID, uid);
    WR32(auth_io, AUTH_GID, uid);
    WR32(auth_io, AUTH_CMD, AUTH_CMD_USERADD);
    microkit_ppcall(CH_AUTH, microkit_msginfo_new(0, 0));
    int rc = (int)(int32_t)RD32(auth_io, AUTH_STATUS);
    if (rc == 0) {
        ser_puts("User '");
        ser_puts(uname);
        ser_puts("' added (uid=");
        ser_put_dec(uid);
        ser_puts(")\n");
    } else if (rc == (int)(int32_t)(uint32_t)-3) {
        ser_puts("Error: user already exists\n");
    } else {
        ser_puts("Error adding user\n");
    }
    ser_flush();
}

/* ── Command: su [username] ────────────────────────── */
static void cmd_su(const char *args) {
    char target[32];
    int i = 0;
    while (args[i] == ' ') i++;
    int j = 0;
    while (args[i] && args[i] != ' ' && j < 31) target[j++] = args[i++];
    target[j] = '\0';

    if (target[0] == '\0') {
        /* Default: su to root */
        target[0] = 'r'; target[1] = 'o'; target[2] = 'o'; target[3] = 't'; target[4] = '\0';
    }

    char supass[64];
    int sp = 0;
    supass[0] = '\0';

    /* Root doesn't need password */
    if (!current_is_root) {
        ser_puts("Password: ");
        ser_flush();
        ring_buf_t *rx = (ring_buf_t *)rx_buf;
        char c;
        for (;;) {
            while (!ring_get(rx, &c)) { asm volatile("wfe"); }
            if (c == '\r' || c == '\n') break;
            if ((c == 127 || c == '\b') && sp > 0) {
                sp--;
                ser_puts("\b \b");
                ser_flush();
            } else if (sp < 63) {
                supass[sp++] = c;
                ser_puts("*");
                ser_flush();
            }
        }
        supass[sp] = '\0';
        ser_puts("\n");
    }

    /* Send to auth_server */
    volatile char *u = (volatile char *)(auth_io + AUTH_USERNAME);
    volatile char *p = (volatile char *)(auth_io + AUTH_PASSWORD);
    j = 0; while (target[j]) { u[j] = target[j]; j++; } u[j] = '\0';
    j = 0; while (supass[j]) { p[j] = supass[j]; j++; } p[j] = '\0';
    WR32(auth_io, AUTH_SESSION, current_session);
    WR32(auth_io, AUTH_CMD, AUTH_CMD_SU);
    microkit_ppcall(CH_AUTH, microkit_msginfo_new(0, 0));
    int rc = (int)(int32_t)RD32(auth_io, AUTH_STATUS);

    /* Clear password */
    for (int k = 0; k < 64; k++) supass[k] = 0;
    for (int k = 0; k < 64; k++) p[k] = 0;

    if (rc == 0) {
        current_uid = RD32(auth_io, AUTH_UID);
        current_gid = RD32(auth_io, AUTH_GID);
        current_is_root = (current_uid == 0) ? 1 : 0;
        j = 0;
        while (target[j] && j < 31) { current_username[j] = target[j]; j++; }
        current_username[j] = '\0';
        ser_puts("Switched to ");
        ser_puts(current_username);
        ser_puts("\n");
    } else {
        ser_puts("su: authentication failure\n");
    }
    ser_flush();
}

/* ── Command: passwd [username] ────────────────────── */
static void cmd_passwd(const char *args) {
    char target[32];
    int i = 0;
    while (args[i] == ' ') i++;
    int j = 0;
    while (args[i] && args[i] != ' ' && j < 31) target[j++] = args[i++];
    target[j] = '\0';

    /* If no target, change own password */
    if (target[0] == '\0') {
        j = 0;
        while (current_username[j] && j < 31) { target[j] = current_username[j]; j++; }
        target[j] = '\0';
    }

    /* Non-root can only change own password */
    if (!current_is_root && my_strcmp(target, current_username) != 0) {
        ser_puts("Permission denied\n");
        ser_flush();
        return;
    }

    ser_puts("New password: ");
    ser_flush();
    /* Read password (blocking) */
    char newpass[64];
    int np = 0;
    ring_buf_t *rx = (ring_buf_t *)rx_buf;
    char c;
    for (;;) {
        while (!ring_get(rx, &c)) { asm volatile("wfe"); }
        if (c == '\r' || c == '\n') break;
        if ((c == 127 || c == '\b') && np > 0) {
            np--;
            ser_puts("\b \b");
            ser_flush();
        } else if (np < 63) {
            newpass[np++] = c;
            ser_puts("*");
            ser_flush();
        }
    }
    newpass[np] = '\0';
    ser_puts("\n");

    /* Send to auth_server */
    volatile char *u = (volatile char *)(auth_io + AUTH_USERNAME);
    volatile char *p = (volatile char *)(auth_io + AUTH_PASSWORD);
    j = 0; while (target[j]) { u[j] = target[j]; j++; } u[j] = '\0';
    j = 0; while (newpass[j]) { p[j] = newpass[j]; j++; } p[j] = '\0';
    WR32(auth_io, AUTH_SESSION, current_session);
    WR32(auth_io, AUTH_CMD, AUTH_CMD_PASSWD);
    microkit_ppcall(CH_AUTH, microkit_msginfo_new(0, 0));
    int rc = (int)(int32_t)RD32(auth_io, AUTH_STATUS);
    if (rc == 0) {
        ser_puts("Password changed for ");
        ser_puts(target);
        ser_puts("\n");
    } else {
        ser_puts("Error changing password\n");
    }
    /* Clear password */
    for (int k = 0; k < 64; k++) newpass[k] = 0;
    ser_flush();
}


/* ── Command: chmod <mode> <file> ──────────────────── */
static void cmd_chmod(const char *args) {
    /* Parse octal mode and filename */
    int i = 0;
    uint32_t mode = 0;
    while (args[i] == ' ') i++;
    while (args[i] >= '0' && args[i] <= '7') {
        mode = (mode << 3) | (args[i] - '0');
        i++;
    }
    while (args[i] == ' ') i++;
    char fname[64];
    int j = 0;
    while (args[i] && j < 63) fname[j++] = args[i++];
    fname[j] = '\0';

    if (fname[0] == '\0' || mode == 0) {
        ser_puts("Usage: chmod <octal_mode> <file>\n");
        ser_flush();
        return;
    }

    /* Only owner or root can chmod */
    if (!current_is_root) {
        uint32_t fuid = 0;
        fs_stat_ex_sync(fname, 0, &fuid, 0, 0);
        if (fuid != current_uid) {
            ser_puts("Permission denied\n");
            ser_flush();
            return;
        }
    }
    int rc = fs_chmod_sync(fname, mode);
    if (rc == 0) {
        ser_puts("Mode changed\n");
    } else {
        ser_puts("chmod failed\n");
    }
    ser_flush();
}

/* ── Command: chown <uid>:<gid> <file> ─────────────── */
static void cmd_chown(const char *args) {
    if (!current_is_root) {
        ser_puts("Permission denied: root required\n");
        ser_flush();
        return;
    }
    int i = 0;
    uint32_t uid = 0, gid = 0;
    while (args[i] == ' ') i++;
    while (args[i] >= '0' && args[i] <= '9') {
        uid = uid * 10 + (args[i] - '0');
        i++;
    }
    if (args[i] == ':') {
        i++;
        while (args[i] >= '0' && args[i] <= '9') {
            gid = gid * 10 + (args[i] - '0');
            i++;
        }
    } else {
        gid = uid;
    }
    while (args[i] == ' ') i++;
    char fname[64];
    int j = 0;
    while (args[i] && j < 63) fname[j++] = args[i++];
    fname[j] = '\0';

    if (fname[0] == '\0') {
        ser_puts("Usage: chown <uid>[:<gid>] <file>\n");
        ser_flush();
        return;
    }
    int rc = fs_chown_sync(fname, uid, gid);
    if (rc == 0) {
        ser_puts("Owner changed\n");
    } else {
        ser_puts("chown failed\n");
    }
    ser_flush();
}
