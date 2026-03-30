/* Exec and spawn infrastructure
 * Auto-extracted from orchestrator.c
 * Lines 1097-1293
 */

/* ── Command: exec <file.bin> ────────────────────────── */

/* ── Wait/spawn infrastructure ────────────────────────── */
#define MAX_WAIT_QUEUE 8
typedef struct {
    int active;
    int waiting_slot;      /* slot of the parent that is blocked */
    uint32_t wait_pid;     /* PID the parent is waiting for (0 = any child) */
} wait_entry_t;
__attribute__((unused)) static wait_entry_t wait_queue[MAX_WAIT_QUEUE];

/* Completed child exit codes (ring buffer for waitpid) */
#define MAX_EXIT_QUEUE 16
typedef struct {
    int active;
    uint32_t pid;
    uint32_t parent_pid;
    int32_t exit_code;
} exit_record_t;
static exit_record_t exit_queue[MAX_EXIT_QUEUE];

/* Internal: spawn a program into a new slot, return slot or -1 */
static int internal_spawn(const char *filename, const char *args,
                          uint32_t parent_pid, int is_foreground) {
    int st = fs_open_sync(filename);
    if (st != 0) return -1;
    uint32_t fd = RD32(fs_data, FS_FD);
    uint32_t size = RD32(fs_data, FS_FILESIZE);
    if (size == 0 || size > 0x100000) {
        fs_close_sync(fd);
        return -1;
    }

    int slot = find_free_slot();
    if (slot < 0) {
        fs_close_sync(fd);
        return -1;
    }

    uintptr_t cur_sio = sbx_io[slot];
    uintptr_t cur_scode = sbx_code[slot];
    volatile uint8_t *code_dst = (volatile uint8_t *)cur_scode;
    uint32_t offset = 0;
    while (offset < size) {
        uint32_t chunk = size - offset;
        if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
        st = fs_read_sync(fd, offset, chunk);
        if (st != 0 && st != 3) break;
        uint32_t got = RD32(fs_data, FS_LENGTH);
        if (got == 0) break;
        volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
        for (uint32_t j = 0; j < got; j++) code_dst[offset + j] = src[j];
        offset += got;
    }
    fs_close_sync(fd);

    /* Copy args */
    {
        volatile char *adst = (volatile char *)(cur_sio + SBX_ARGS);
        int ai = 0;
        if (args) {
            while (args[ai] && ai < SBX_ARGS_MAX - 1) {
                adst[ai] = args[ai]; ai++;
            }
        }
        adst[ai] = '\0';
    }

    /* Start the sandbox */
    WR32(cur_sio, SBX_CODE_SIZE, offset);
    WR32(cur_sio, SBX_CMD, SBX_CMD_RUN);
    proctab[slot].in_use = 1;
    proctab[slot].owner_uid = current_uid;
    proctab[slot].pid = next_pid++;
    proctab[slot].parent_pid = parent_pid;
    proctab[slot].loaded_bytes = offset;
    proctab[slot].foreground = is_foreground;
    /* Copy name */
    {
        int ni = 0;
        while (filename[ni] && ni < 63) {
            proctab[slot].name[ni] = filename[ni]; ni++;
        }
        proctab[slot].name[ni] = '\0';
    }
    microkit_notify(sbx_channel[slot]);
    return slot;
}

static void cmd_exec(const char *filename) {
    /* Check for background execution */
    int background = 0;
    char fname_buf[256];
    {
        int len = 0;
        while (filename[len] && len < 255) { fname_buf[len] = filename[len]; len++; }
        fname_buf[len] = 0;
        /* Strip trailing spaces and & */
        while (len > 0 && fname_buf[len-1] == ' ') fname_buf[--len] = 0;
        if (len > 0 && fname_buf[len-1] == '&') {
            fname_buf[--len] = 0;
            while (len > 0 && fname_buf[len-1] == ' ') fname_buf[--len] = 0;
            background = 1;
        }
        filename = fname_buf;
    }
    ser_puts("Loading program: ");
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
    ser_puts("  Program size: ");
    ser_put_dec(size);
    ser_puts(" bytes\n");
    ser_flush();

    if (size > 0x100000) {
        ser_puts("  Error: program too large (max 1 MiB)\n");
        ser_flush();
        fs_close_sync(fd);
        return;
    }

    /* Load into sandbox_code */
    int slot = find_free_slot();
    if (slot < 0) {
        ser_puts("  Error: no free process slots\n");
        ser_flush();
        fs_close_sync(fd);
        return;
    }
    uintptr_t cur_sio = sbx_io[slot];
    uintptr_t cur_scode = sbx_code[slot];
    volatile uint8_t *code_dst = (volatile uint8_t *)cur_scode;
    uint32_t offset = 0;
    while (offset < size) {
        uint32_t chunk = size - offset;
        if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
        st = fs_read_sync(fd, offset, chunk);
        if (st != 0 && st != 3) break;
        uint32_t got = RD32(fs_data, FS_LENGTH);
        if (got == 0) break;
        volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
        for (uint32_t i = 0; i < got; i++) code_dst[offset + i] = src[i];
        offset += got;
    }
    fs_close_sync(fd);
    exec_loaded_bytes = offset;

    LOG_INFO("program loaded into sandbox");
    ser_puts("  Loaded ");
    ser_put_dec(exec_loaded_bytes);
    ser_puts(" bytes into sandbox. Executing...\n");
    ser_flush();

    /* Copy args to sandbox_io for the program */
    {
        volatile char *adst = (volatile char *)(cur_sio + SBX_ARGS);
        int ai = 0;
        while (exec_args[ai] && ai < SBX_ARGS_MAX - 1) {
            adst[ai] = exec_args[ai]; ai++;
        }
        adst[ai] = '\0';
    }

    /* Tell sandbox to run */
    WR32(cur_sio, SBX_CODE_SIZE, exec_loaded_bytes);
    WR32(cur_sio, SBX_CMD, SBX_CMD_RUN);
    proctab[slot].in_use = 1;
    proctab[slot].owner_uid = current_uid;
    proctab[slot].pid = next_pid++;
    proctab[slot].loaded_bytes = exec_loaded_bytes;
    proctab[slot].foreground = background ? 0 : 1;
    {
        int ni = 0;
        const char *fn = filename;
        while (*fn && ni < 63) proctab[slot].name[ni++] = *fn++;
        proctab[slot].name[ni] = 0;
    }
    if (!background) {
        orch_state = EXEC_RUNNING;
    } else {
        ser_puts("  [");
        ser_put_dec((unsigned int)slot);
        ser_puts("] PID ");
        ser_put_dec(proctab[slot].pid);
        ser_puts("\n");
        print_prompt();
    }
    microkit_notify(sbx_channel[slot]);
}
