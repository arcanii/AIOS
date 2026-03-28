/* AIOS Orchestrator — synchronous rewrite
 * All filesystem operations use microkit_ppcall (blocking).
 * State machine only needed for: LLM generation (async), sandbox exec (async).
 */
#include <microkit.h>
#include "aios/ipc.h"
#include "aios/channels.h"
#include "sys/syscall.h"
#include "aios/ring.h"

/* ── Shared memory regions (set by Microkit loader) ─── */
uintptr_t tx_buf;
uintptr_t rx_buf;
uintptr_t fs_data;
uintptr_t model_data;
uintptr_t llm_io;
uintptr_t sandbox_io;
uintptr_t sandbox_code;

/* ── Memory helpers ──────────────────────────────────── */
static __attribute__((unused)) void my_memcpy(void *dst, const void *src, unsigned long n) {
    char *d = dst; const char *s = src;
    while (n--) *d++ = *s++;
}
static __attribute__((unused)) void my_memset(void *dst, int c, unsigned long n) {
    char *d = dst;
    while (n--) *d++ = (char)c;
}
static int my_strlen(const char *s) {
    int n = 0; while (*s++) n++;
    return n;
}
static int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static int str_starts_with(const char *s, const char *prefix) {
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}

/* ── Serial I/O ──────────────────────────────────────── */
static void ser_putc(char c) {
    ring_buf_t *tx = (ring_buf_t *)tx_buf;
    ring_put(tx, (uint8_t)c);
}
static void ser_puts(const char *s) {
    ring_buf_t *tx = (ring_buf_t *)tx_buf;
    while (*s) ring_put(tx, (uint8_t)*s++);
}
static void ser_put_dec(unsigned int n) {
    char buf[12]; int i = 0;
    if (n == 0) { ser_putc('0'); return; }
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i--) ser_putc(buf[i]);
}
static __attribute__((unused)) void ser_put_hex(unsigned long n) {
    char buf[17]; int i = 0;
    if (n == 0) { ser_puts("0"); return; }
    while (n) { int d = n & 0xf; buf[i++] = d < 10 ? '0'+d : 'a'+d-10; n >>= 4; }
    while (i--) ser_putc(buf[i]);
}
#define ser_flush() microkit_notify(CH_SERIAL)

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

static int fs_list_sync(void) {
    WR32(fs_data, FS_CMD, FS_CMD_LIST);
    microkit_ppcall(CH_FS, microkit_msginfo_new(0, 0));
    return (int)RD32(fs_data, FS_STATUS);
}

/* ── Input buffer ────────────────────────────────────── */
#define INPUT_MAX 256
static char input_line[INPUT_MAX];
static int  input_pos = 0;

/* ── Orchestrator state (minimal — only for async ops) ─ */
enum {
    RUNNING,
    EXEC_RUNNING,   /* sandbox is executing */
    GEN_WAITING,    /* LLM generating tokens */
};
static int orch_state = RUNNING;

/* ── Model loading state ─────────────────────────────── */
static uint32_t llm_model_loaded = 0;

/* ── Exec state ──────────────────────────────────────── */
static uint32_t exec_loaded_bytes = 0;

/* ── Command: help ───────────────────────────────────── */
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
    ser_puts("  shutdown        - halt system\n");
    ser_flush();
}

/* ── Command: ls ─────────────────────────────────────── */
static void cmd_ls(void) {
    int st = fs_list_sync();
    if (st != 0) {
        ser_puts("  Error listing directory\n");
        ser_flush();
        return;
    }
    /* fs_data+FS_DATA has directory listing: 16-byte entries (11 name + 1 attr + 4 size) */
    uint32_t count = RD32(fs_data, FS_LENGTH);
    uint8_t *entries = (uint8_t *)(fs_data + FS_DATA);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *ent = entries + i * 16;
        /* 8.3 name */
        char name[13];
        int p = 0;
        for (int j = 0; j < 8; j++) {
            if (ent[j] != ' ') name[p++] = ent[j];
        }
        if (ent[8] != ' ') {
            name[p++] = '.';
            for (int j = 8; j < 11; j++) {
                if (ent[j] != ' ') name[p++] = ent[j];
            }
        }
        name[p] = '\0';
        uint32_t size = ent[12] | (ent[13]<<8) | (ent[14]<<16) | (ent[15]<<24);
        /* Format: name     size */
        ser_puts("  ");
        ser_puts(name);
        /* Pad to 16 chars */
        for (int j = p; j < 14; j++) ser_putc(' ');
        ser_put_dec(size);
        ser_puts(" bytes\n");
    }
    ser_flush();
}

/* ── Command: cat <file> ─────────────────────────────── */
static void cmd_cat(const char *filename) {
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

    /* Delete existing file if present */
    fs_delete_sync(fname);

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
    int st = fs_delete_sync(filename);
    if (st != 0) {
        ser_puts("  File not found.\n"); ser_flush();
    } else {
        ser_puts("  Deleted.\n"); ser_flush();
    }
}

/* ── Command: exec <file.bin> ────────────────────────── */
static void cmd_exec(const char *filename) {
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
    volatile uint8_t *code_dst = (volatile uint8_t *)sandbox_code;
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

    ser_puts("  Loaded ");
    ser_put_dec(exec_loaded_bytes);
    ser_puts(" bytes into sandbox. Executing...\n");
    ser_flush();

    /* Tell sandbox to run */
    WR32(sandbox_io, SBX_CODE_SIZE, exec_loaded_bytes);
    WR32(sandbox_io, SBX_CMD, SBX_CMD_RUN);
    orch_state = EXEC_RUNNING;
    microkit_notify(CH_SANDBOX);
}

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

    /* Tell LLM server to init */
    WR32(llm_io, 0, 1); /* LLM_CMD_INIT */
    microkit_notify(CH_LLM);
    llm_model_loaded = 1;

    ser_puts("  Model loaded, initializing LLM...\n");
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
    volatile char *dst = (volatile char *)(llm_io + 0x100);
    int i = 0;
    while (prompt[i] && i < 255) { dst[i] = prompt[i]; i++; }
    dst[i] = '\0';
    WR32(llm_io, 0, 2); /* LLM_CMD_GENERATE */
    orch_state = GEN_WAITING;
    microkit_notify(CH_LLM);
}

/* ── Command: info ───────────────────────────────────── */
static void cmd_info(void) {
    ser_puts("AIOS System Information:\n");
    ser_puts("  Kernel:     seL4 14.0.0 (Microkit 2.1.0)\n");
    ser_puts("  Arch:       AArch64 (Cortex-A53)\n");
    ser_puts("  RAM:        2 GiB\n");
    ser_puts("  Disk:       128 MiB FAT16\n");
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

/* ── Command dispatch ────────────────────────────────── */
static void process_command(void) {
    input_line[input_pos] = '\0';
    if (input_pos == 0) goto prompt;

    if (my_strcmp(input_line, "help") == 0) {
        cmd_help();
    } else if (my_strcmp(input_line, "ls") == 0 ||
               my_strcmp(input_line, "dir") == 0) {
        cmd_ls();
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
        char *fn = &input_line[5];
        while (*fn == ' ') fn++;
        if (*fn) {
            cmd_exec(fn);
            input_pos = 0;
            return; /* Don't print prompt yet — sandbox running */
        }
        else { ser_puts("Usage: exec <file.bin>\n"); ser_flush(); }
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
    } else if (my_strcmp(input_line, "shutdown") == 0) {
        cmd_shutdown();
    } else {
        ser_puts("Unknown command: ");
        ser_puts(input_line);
        ser_puts("\nType 'help' for commands.\n");
        ser_flush();
    }

prompt:
    ser_puts("AIOS> ");
    ser_flush();
    input_pos = 0;
}

/* ── Serial input handler ────────────────────────────── */
static char rx_getc_blocking(void) {
    ring_buf_t *rx = (ring_buf_t *)rx_buf;
    char c;
    while (!ring_get(rx, &c)) {
        asm volatile("wfe");
    }
    return c;
}

static void handle_serial_input(void) {
    ring_buf_t *rx = (ring_buf_t *)rx_buf;
    char c;
    while (ring_get(rx, &c)) {
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
static void handle_exec_done(void) {
    (void)RD32(sandbox_io, SBX_STATUS);
    uint32_t exit_code = RD32(sandbox_io, SBX_EXIT_CODE);
    uint32_t out_len = RD32(sandbox_io, SBX_OUTPUT_LEN);

    if (out_len > 0) {
        ser_puts("--- program output ---\n");
        ser_flush();
        volatile char *out = (volatile char *)(sandbox_io + SBX_OUTPUT);
        for (uint32_t i = 0; i < out_len && i < 4096; i++) ser_putc(out[i]);
        ser_puts("\n--- end output ---\n");
        ser_flush();
    }
    ser_puts("  Exit code: ");
    ser_put_dec(exit_code);
    ser_puts("\n");
    ser_flush();
    orch_state = RUNNING;
    ser_puts("AIOS> ");
    ser_flush();
}

/* ── Handle LLM generation reply ─────────────────────── */
static void handle_gen_reply(void) {
    uint32_t status = RD32(llm_io, 0x04);
    if (status == 1) { /* LLM_ST_TOKEN */
        volatile char *tok = (volatile char *)(llm_io + 0x100);
        for (int i = 0; tok[i] && i < 256; i++) ser_putc(tok[i]);
        ser_flush();
        /* Request next token */
        WR32(llm_io, 0, 3); /* LLM_CMD_NEXT */
        microkit_notify(CH_LLM);
    } else if (status == 2) { /* LLM_ST_DONE */
        ser_puts("\n[done]\n");
        ser_flush();
        orch_state = RUNNING;
        ser_puts("AIOS> ");
        ser_flush();
    } else {
        ser_puts("\n[LLM error]\n");
        ser_flush();
        orch_state = RUNNING;
        ser_puts("AIOS> ");
        ser_flush();
    }
}

/* ── Protected procedure call handler (sandbox syscalls) */
microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    if (ch != CH_SANDBOX) {
        seL4_SetMR(0, -1);
        return microkit_msginfo_new(0, 1);
    }

    uint64_t syscall_nr = seL4_GetMR(0);
    int64_t result = -1;

    switch (syscall_nr) {
    case SYS_OPEN: {
        volatile char *path = (volatile char *)(sandbox_io + 0x200);
        char fname[256];
        int i = 0;
        while (path[i] && i < 255) { fname[i] = path[i]; i++; }
        fname[i] = '\0';
        int st = fs_open_sync(fname);
        if (st == 0) {
            result = RD32(fs_data, FS_FD);
            seL4_SetMR(1, RD32(fs_data, FS_FILESIZE));
        } else {
            result = -1;
        }
        break;
    }
    case SYS_READ: {
        uint32_t fd     = (uint32_t)seL4_GetMR(1);
        uint32_t offset = (uint32_t)seL4_GetMR(2);
        uint32_t len    = (uint32_t)seL4_GetMR(3);
        int st = fs_read_sync(fd, offset, len);
        if (st == 0 || st == 3) {
            uint32_t got = RD32(fs_data, FS_LENGTH);
            /* Copy data to sandbox_io+0x400 */
            volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
            volatile uint8_t *dst = (volatile uint8_t *)(sandbox_io + 0x400);
            for (uint32_t i = 0; i < got; i++) dst[i] = src[i];
            result = got;
        }
        break;
    }
    case SYS_WRITE: {
        uint32_t fd  = (uint32_t)seL4_GetMR(1);
        uint32_t len = (uint32_t)seL4_GetMR(2);
        /* Data is at sandbox_io+0x400 */
        volatile uint8_t *src = (volatile uint8_t *)(sandbox_io + 0x400);
        volatile uint8_t *dst = (volatile uint8_t *)(fs_data + FS_DATA);
        for (uint32_t i = 0; i < len && i < FS_DATA_MAX; i++) dst[i] = src[i];
        int st = fs_write_sync(fd, len);
        if (st == 0) result = RD32(fs_data, FS_LENGTH);
        break;
    }
    case SYS_CLOSE: {
        uint32_t fd = (uint32_t)seL4_GetMR(1);
        fs_close_sync(fd);
        result = 0;
        break;
    }
    case SYS_UNLINK: {
        volatile char *path = (volatile char *)(sandbox_io + 0x200);
        char fname[256];
        int i = 0;
        while (path[i] && i < 255) { fname[i] = path[i]; i++; }
        fname[i] = '\0';
        int st = fs_delete_sync(fname);
        result = (st == 0) ? 0 : -1;
        break;
    }
    case SYS_READDIR: {
        int st = fs_list_sync();
        if (st == 0) {
            uint32_t count = RD32(fs_data, FS_LENGTH);
            /* Copy listing to sandbox_io+0x400 */
            volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
            volatile uint8_t *dst = (volatile uint8_t *)(sandbox_io + 0x400);
            uint32_t bytes = count * 16;
            for (uint32_t i = 0; i < bytes; i++) dst[i] = src[i];
            result = count;
        }
        break;
    }
    case SYS_PUTC: {
        char c = (char)seL4_GetMR(1);
        ser_putc(c);
        ser_flush();
        result = 0;
        break;
    }
    case 32: { /* SYS_PUTS_DIRECT */
        volatile char *str = (volatile char *)(sandbox_io + 0x200);
        char buf[256];
        int i = 0;
        while (str[i] && i < 255) { buf[i] = str[i]; i++; }
        buf[i] = '\0';
        ser_puts(buf);
        ser_flush();
        result = 0;
        break;
    }
    case SYS_GETC: {
        result = (int64_t)(unsigned char)rx_getc_blocking();
        break;
    }
    case SYS_EXIT: {
        result = 0;
        break;
    }
    default:
        result = -1;
        break;
    }

    seL4_SetMR(0, result);
    return microkit_msginfo_new(0, 2);
}

/* ── Microkit entry points ─────────────────────────── */
void init(void) {
    ser_puts("\n========================================\n");
    ser_puts("  AIOS v0.7\n");
    ser_puts("  Kernel:  seL4 14.0.0 (Microkit 2.1.0)\n");
    ser_puts("  Drivers: PL011 UART, virtio-blk, FAT16\n");
    ser_puts("  LLM:     llm_server (llama2.c engine)\n");
    ser_puts("========================================\n\n");
    ser_flush();

    /* Boot: read hello.txt synchronously */
    ser_puts("Boot: reading hello.txt...\n");
    ser_flush();
    int st = fs_open_sync("hello.txt");
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
        ser_puts("  hello.txt not found\n");
    }
    ser_flush();

    /* Check for AUTOEXEC.BIN */
    st = fs_open_sync("AUTOEXEC.BIN");
    if (st == 0) {
        uint32_t fd = RD32(fs_data, FS_FD);
        fs_close_sync(fd);
        ser_puts("Boot: found AUTOEXEC.BIN, executing...\n");
        ser_flush();
        cmd_exec("AUTOEXEC.BIN");
        return;
    }

    ser_puts("\nType 'help' for commands.\n");
    ser_puts("AIOS> ");
    ser_flush();
}

void notified(microkit_channel ch) {
    switch (ch) {
    case CH_SERIAL:
        if (orch_state == RUNNING)
            handle_serial_input();
        break;

    case CH_SANDBOX:
        if (orch_state == EXEC_RUNNING)
            handle_exec_done();
        break;

    case CH_LLM:
        if (orch_state == GEN_WAITING)
            handle_gen_reply();
        break;

    default:
        break;
    }
}
