/* Filesystem commands (stat, help, ls, cat, write, rm)
 * Auto-extracted from orchestrator.c
 * Lines 806-1095
 */

/* ── Command: stat <file> — show extended info ─────── */
static void cmd_stat_ex(const char *fname) {
    uint32_t size = 0, uid = 0, gid = 0, mode = 0;
    int rc = fs_stat_ex_sync(fname, &size, &uid, &gid, &mode, 0);
    if (rc != 0) {
        ser_puts("  File not found\n");
        ser_flush();
        return;
    }
    /* Print permissions like -rwxrwxrwx */
    char perms[11];
    perms[0] = (mode & 0x4000) ? 'd' : '-';
    perms[1] = (mode & 0x0100) ? 'r' : '-';
    perms[2] = (mode & 0x0080) ? 'w' : '-';
    perms[3] = (mode & 0x0040) ? 'x' : '-';
    perms[4] = (mode & 0x0020) ? 'r' : '-';
    perms[5] = (mode & 0x0010) ? 'w' : '-';
    perms[6] = (mode & 0x0008) ? 'x' : '-';
    perms[7] = (mode & 0x0004) ? 'r' : '-';
    perms[8] = (mode & 0x0002) ? 'w' : '-';
    perms[9] = (mode & 0x0001) ? 'x' : '-';
    perms[10] = '\0';
    ser_puts("  ");
    ser_puts(perms);
    ser_puts("  uid=");
    ser_put_dec(uid);
    ser_puts("  gid=");
    ser_put_dec(gid);
    ser_puts("  size=");
    ser_put_dec(size);
    ser_puts("\n");
    ser_flush();
}

static void cmd_help(void) {
    ser_puts("Commands:\n");
    ser_puts("  help            - this message\n");
    ser_puts("  ls              - list files\n");
    ser_puts("  cat <file>      - read a file from disk\n");
    ser_puts("  write <f> <txt> - write text to a file\n");
    ser_puts("  rm <file>       - delete a file\n");
    ser_puts("  load <file>     - load model into memory\n");
    ser_puts("  gen <prompt>    - generate text\n");
    ser_puts("  exec <file.bin> - run a sandbox program\n");
    ser_puts("  info            - system info\n");
    ser_puts("  whoami              - show current user\n");
    ser_puts("  su [user]           - switch user\n");
    ser_puts("  passwd [user]       - change password\n");
    ser_puts("  useradd <u> <p> <uid> - add user (root)\n");
    ser_puts("  chmod <mode> <file>   - change permissions\n");
    ser_puts("  chown <uid:gid> <file> - change owner (root)\n");
    ser_puts("  stat <file>           - show file details\n");
    ser_puts("  logout              - log out\n");
    ser_puts("  ps              Show running processes\n");
    ser_puts("  kill <pid>      Terminate a process\n");
    ser_puts("  shutdown        - halt system\n");
    ser_puts("  groups          - show group memberships\n");
    ser_puts("  usermod <uid> <gid> - add user to group (root)\n");
    ser_flush();
}

/* ── Command: ls ─────────────────────────────────────── */
static void cmd_ls(void) {
    int st = fs_list_sync("/");
    if (st != 0) {
        ser_puts("  Error listing directory\n");
        ser_flush();
        return;
    }
    /* Variable-length entries: [2B entry_len][1B name_len][1B type][4B size][name][NUL] */
    uint32_t count = RD32(fs_data, FS_LENGTH);
    uint32_t total_bytes = RD32(fs_data, FS_FILESIZE);
    uint32_t off = 0;
    uint8_t *entries = (uint8_t *)(fs_data + FS_DATA);
    for (uint32_t i = 0; i < count; i++) {
        if (off + 8 > total_bytes) break;
        uint16_t elen = entries[off] | (entries[off+1] << 8);
        if (elen == 0) break;
        uint8_t nlen = entries[off + 2];
        uint32_t size = entries[off+4] | (entries[off+5]<<8) | (entries[off+6]<<16) | (entries[off+7]<<24);
        char name[256];
        int p = nlen < 255 ? nlen : 255;
        for (int j = 0; j < p; j++) name[j] = (char)entries[off + 8 + j];
        name[p] = '\0';
        /* Format: name     size */
        ser_puts("  ");
        ser_puts(name);
        /* Pad to 16 chars */
        for (int j = p; j < 16; j++) ser_putc(' ');
        ser_put_dec(size);
        ser_puts(" bytes\n");
        off += elen;
    }
    ser_flush();
}

/* ── Command: cat <file> ─────────────────────────────── */

static void auth_groups_sync(void);
static int auth_usermod_sync(uint32_t target_uid, uint32_t gid);
static void cmd_groups(void) {
    auth_groups_sync();
    int st = (int)RD32(auth_io, AUTH_STATUS);
    if (st != 0) {
        ser_puts("Error querying groups\n"); ser_flush();
        return;
    }
    uint32_t count = RD32(auth_io, AUTH_UID);
    volatile uint32_t *gids = (volatile uint32_t *)(auth_io + AUTH_DATA);
    for (uint32_t i = 0; i < count; i++) {
        if (i > 0) ser_puts(" ");
        ser_put_dec(gids[i]);
    }
    ser_puts("\n"); ser_flush();
}

static void cmd_usermod(const char *args) {
    /* Parse: usermod <username> <gid> */
    char uname[32];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < 31) { uname[i] = args[i]; i++; }
    uname[i] = '\0';
    while (args[i] == ' ') i++;

    /* Parse gid */
    uint32_t gid = 0;
    while (args[i] >= '0' && args[i] <= '9') {
        gid = gid * 10 + (args[i] - '0');
        i++;
    }
    if (gid == 0 && args[i-1] != '0') {
        ser_puts("Usage: usermod <username> <gid>\n"); ser_flush();
        return;
    }

    /* Look up uid by username — ask auth server via whoami-style query */
    /* For simplicity, find uid from /etc/passwd convention:
       root=0, user=1000, guest=1001 or use uid directly */
    /* Actually use the username field in auth_io to look up */
    /* For now, require uid directly: usermod <uid> <gid> */
    uint32_t target_uid = 0;
    int j = 0;
    while (uname[j] >= '0' && uname[j] <= '9') {
        target_uid = target_uid * 10 + (uname[j] - '0');
        j++;
    }
    if (j == 0) {
        ser_puts("Usage: usermod <uid> <gid>\n"); ser_flush();
        return;
    }

    int rc = auth_usermod_sync(target_uid, gid);
    if (rc == 0) {
        ser_puts("Added uid "); ser_put_dec(target_uid);
        ser_puts(" to group "); ser_put_dec(gid);
        ser_puts("\n"); ser_flush();
    } else {
        ser_puts("Failed (not root or user not found)\n"); ser_flush();
    }
}

static void cmd_cat(const char *filename) {
    /* Permission check */
    if (auth_check_file_sync(filename, ACCESS_READ) != 0) {
        ser_puts("Permission denied\n"); ser_flush();
        return;
    }
    ser_puts("Opening ");
    ser_puts(filename);
    ser_puts("...\n");
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
    ser_puts(" bytes\n");
    ser_flush();

    uint32_t offset = 0;
    while (offset < size) {
        uint32_t chunk = size - offset;
        if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
        st = fs_read_sync(fd, offset, chunk);
        if (st != 0 && st != 3 /* EOF */) break;
        uint32_t got = RD32(fs_data, FS_LENGTH);
        if (got == 0) break;
        /* Print data */
        volatile char *data = (volatile char *)(fs_data + FS_DATA);
        for (uint32_t i = 0; i < got; i++) ser_putc(data[i]);
        ser_flush();
        offset += got;
    }
    ser_puts("\n");
    fs_close_sync(fd);
    ser_puts("  File closed.\n");
    ser_flush();
}

/* ── Command: write <file> <text> ────────────────────── */
static void cmd_write(const char *args) {
    /* Parse: filename text */
    char fname[64];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < 63) { fname[i] = args[i]; i++; }
    fname[i] = '\0';
    while (args[i] == ' ') i++;
    const char *text = &args[i];
    int tlen = my_strlen(text);

    if (tlen == 0) {
        ser_puts("Usage: write <file> <text>\n"); ser_flush();
        return;
    }

    /* Permission check */
    if (auth_check_file_sync(fname, ACCESS_WRITE) != 0) {
        ser_puts("Permission denied\n"); ser_flush();
        return;
    }

    /* Delete existing file if present */
    fs_delete_sync(fname);

    /* Set creator uid/gid AFTER delete (delete clobbers shared memory) */
    fs_set_creator_sync(current_uid, current_gid);

    /* Create new file */
    int st = fs_create_sync(fname);
    if (st != 0) {
        ser_puts("  Create failed.\n"); ser_flush();
        return;
    }
    uint32_t fd = RD32(fs_data, FS_FD);

    /* Copy text into fs_data buffer */
    volatile char *data = (volatile char *)(fs_data + FS_DATA);
    for (int j = 0; j < tlen; j++) data[j] = text[j];
    WR32(fs_data, FS_LENGTH, tlen);

    st = fs_write_sync(fd, tlen);
    if (st != 0) {
        ser_puts("  Write failed.\n"); ser_flush();
    } else {
        uint32_t wrote = RD32(fs_data, FS_LENGTH);
        ser_puts("  Wrote ");
        ser_put_dec(wrote);
        ser_puts(" bytes.\n");
        ser_flush();
    }
    fs_close_sync(fd);
}

/* ── Command: rm <file> ──────────────────────────────── */
static void cmd_rm(const char *filename) {
    /* Permission check */
    if (auth_check_file_sync(filename, ACCESS_WRITE) != 0) {
        ser_puts("Permission denied\n"); ser_flush();
        return;
    }
    int st = fs_delete_sync(filename);
    if (st != 0) {
        ser_puts("  File not found.\n"); ser_flush();
    } else {
        ser_puts("  Deleted.\n"); ser_flush();
    }
}


/* ── Groups query ─────────────────────────────────────── */
void auth_groups_sync(void) {
    WR32(auth_io, AUTH_CMD, AUTH_CMD_GROUPS);
    WR32(auth_io, AUTH_SESSION, current_session);
    microkit_ppcall(CH_AUTH, microkit_msginfo_new(0, 0));
}

/* ── Usermod: add user to group ──────────────────────── */
int auth_usermod_sync(uint32_t target_uid, uint32_t gid) {
    WR32(auth_io, AUTH_CMD, AUTH_CMD_USERMOD);
    WR32(auth_io, AUTH_SESSION, current_session);
    WR32(auth_io, AUTH_UID, target_uid);
    WR32(auth_io, AUTH_GID, gid);
    microkit_ppcall(CH_AUTH, microkit_msginfo_new(0, 0));
    return (int)RD32(auth_io, AUTH_STATUS);
}
