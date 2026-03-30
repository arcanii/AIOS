/* Filesystem sync operations
 * Auto-extracted from orchestrator.c
 * Lines 128-290
 */

/* ── Synchronous FS operations ───────────────────────── */

static int fs_open_sync(const char *filename) {
    WR32(fs_data, FS_CMD, FS_CMD_OPEN);
    char *dst = (char *)(fs_data + FS_FILENAME);
    int i = 0;
    while (filename[i] && i < 255) { dst[i] = filename[i]; i++; }
    dst[i] = '\0';
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    return (int)RD32(fs_data, FS_STATUS);
}

static int fs_read_sync(uint32_t fd, uint32_t offset, uint32_t len) {
    if (len > FS_DATA_MAX) len = FS_DATA_MAX;
    WR32(fs_data, FS_CMD, FS_CMD_READ);
    WR32(fs_data, FS_FD, fd);
    WR32(fs_data, FS_OFFSET, offset);
    WR32(fs_data, FS_LENGTH, len);
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    return (int)RD32(fs_data, FS_STATUS);
}

static int fs_write_sync(uint32_t fd, uint32_t len) {
    WR32(fs_data, FS_CMD, FS_CMD_WRITE);
    WR32(fs_data, FS_FD, fd);
    WR32(fs_data, FS_LENGTH, len);
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    return (int)RD32(fs_data, FS_STATUS);
}

static int fs_close_sync(uint32_t fd) {
    WR32(fs_data, FS_CMD, FS_CMD_CLOSE);
    WR32(fs_data, FS_FD, fd);
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    return (int)RD32(fs_data, FS_STATUS);
}

static int fs_create_sync(const char *filename) {
    WR32(fs_data, FS_CMD, FS_CMD_CREATE);
    char *dst = (char *)(fs_data + FS_FILENAME);
    int i = 0;
    while (filename[i] && i < 255) { dst[i] = filename[i]; i++; }
    dst[i] = '\0';
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    return (int)RD32(fs_data, FS_STATUS);
}

static int fs_delete_sync(const char *filename) {
    WR32(fs_data, FS_CMD, FS_CMD_DELETE);
    char *dst = (char *)(fs_data + FS_FILENAME);
    int i = 0;
    while (filename[i] && i < 255) { dst[i] = filename[i]; i++; }
    dst[i] = '\0';
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    return (int)RD32(fs_data, FS_STATUS);
}


/* ── Extended FS helpers ───────────────────────────── */
static int fs_stat_ex_sync(const char *filename, uint32_t *size,
                           uint32_t *uid, uint32_t *gid, uint32_t *mode,
                           uint32_t *mtime) {
    volatile char *dst = (volatile char *)(fs_data + FS_FILENAME);
    int i = 0;
    while (filename[i] && i < 63) { dst[i] = filename[i]; i++; }
    dst[i] = '\0';
    WR32(fs_data, FS_CMD, FS_CMD_STAT_EX);
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    if (size) *size = RD32(fs_data, FS_FILESIZE);
    if (uid)  *uid  = RD32(fs_data, FS_UID);
    if (gid)  *gid  = RD32(fs_data, FS_GID);
    if (mode) *mode = RD32(fs_data, FS_MODE);
    if (mtime) *mtime = RD32(fs_data, FS_MTIME);
    return (int)(int32_t)RD32(fs_data, FS_STATUS);
}

static int fs_chmod_sync(const char *filename, uint32_t mode) {
    volatile char *dst = (volatile char *)(fs_data + FS_FILENAME);
    int i = 0;
    while (filename[i] && i < 63) { dst[i] = filename[i]; i++; }
    dst[i] = '\0';
    WR32(fs_data, FS_MODE, mode);
    WR32(fs_data, FS_CMD, FS_CMD_CHMOD);
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    return (int)(int32_t)RD32(fs_data, FS_STATUS);
}

static int fs_chown_sync(const char *filename, uint32_t uid, uint32_t gid) {
    volatile char *dst = (volatile char *)(fs_data + FS_FILENAME);
    int i = 0;
    while (filename[i] && i < 63) { dst[i] = filename[i]; i++; }
    dst[i] = '\0';
    WR32(fs_data, FS_UID, uid);
    WR32(fs_data, FS_GID, gid);
    WR32(fs_data, FS_CMD, FS_CMD_CHOWN);
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    return (int)(int32_t)RD32(fs_data, FS_STATUS);
}

static void fs_set_creator_sync(uint32_t uid, uint32_t gid) {
    WR32(fs_data, FS_CREAT_UID, uid);
    WR32(fs_data, FS_CREAT_GID, gid);
}

static int fs_stat_sync(const char *filename) {
    volatile char *dst = (volatile char *)(fs_data + FS_FILENAME);
    int i = 0;
    while (filename[i] && i < 63) { dst[i] = filename[i]; i++; }
    dst[i] = '\0';
    WR32(fs_data, FS_CMD, FS_CMD_STAT);
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    return (int)RD32(fs_data, FS_STATUS);
}

static int fs_mkdir_sync(const char *dirname) {
    volatile char *dst = (volatile char *)(fs_data + FS_FILENAME);
    int i = 0;
    while (dirname[i] && i < 63) { dst[i] = dirname[i]; i++; }
    dst[i] = '\0';
    WR32(fs_data, FS_CMD, FS_CMD_MKDIR);
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    return (int)RD32(fs_data, FS_STATUS);
}

static int fs_rmdir_sync(const char *dirname) {
    volatile char *dst = (volatile char *)(fs_data + FS_FILENAME);
    int i = 0;
    while (dirname[i] && i < 63) { dst[i] = dirname[i]; i++; }
    dst[i] = '\0';
    WR32(fs_data, FS_CMD, FS_CMD_RMDIR);
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    return (int)RD32(fs_data, FS_STATUS);
}

static int fs_rename_sync(const char *oldname, const char *newname) {
    volatile char *dst = (volatile char *)(fs_data + FS_FILENAME);
    int i = 0;
    while (oldname[i] && i < 63) { dst[i] = oldname[i]; i++; }
    dst[i] = '\0';
    i++;
    int j = 0;
    while (newname[j] && i < 126) { dst[i] = newname[j]; i++; j++; }
    dst[i] = '\0';
    WR32(fs_data, FS_CMD, FS_CMD_RENAME);
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    return (int)RD32(fs_data, FS_STATUS);
}


static const char *fs_fsinfo_sync(void) {
    volatile uint8_t *sh = (volatile uint8_t *)fs_data;
    WR32(fs_data, FS_CMD, FS_CMD_FSINFO);
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    return (const char *)(sh + FS_FILENAME);
}

static int fs_list_sync(const char *path) {
    volatile char *dst = (volatile char *)(fs_data + FS_FILENAME);
    int i = 0;
    while (path[i] && i < 63) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    WR32(fs_data, FS_CMD, FS_CMD_LIST);
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    return (int)RD32(fs_data, FS_STATUS);
}
