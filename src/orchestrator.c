/*
 * AIOS Orchestrator
 *
 * Central coordinator PD. Manages boot sequence, file loading,
 * model loading, interactive shell, and LLM inference requests.
 *
 * Copyright (c) 2025 AIOS Project
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <microkit.h>
#include "aios/channels.h"
#include "aios/ipc.h"
#include "aios/ring.h"
#include "aios/serial.h"

/* ── Shared-memory regions (set by Microkit loader) ── */
uintptr_t rx_buf;
uintptr_t tx_buf;
uintptr_t blk_data;
uintptr_t fs_data;
uintptr_t model_data;
uintptr_t llm_io;

/* ── State machine ──────────────────────────────────── */
typedef enum {
    BOOT_OPENING,
    BOOT_READING,
    BOOT_CLOSING,
    LOAD_OPENING,
    LOAD_READING,
    LOAD_CLOSING,
    LOAD_NOTIFY_LLM,
    TOK_OPENING,
    TOK_READING,
    TOK_CLOSING,
    TOK_NOTIFY_LLM,
    GEN_WAITING,
    WRITE_WAIT_CREATE,
    WRITE_WAIT_DATA,
    WRITE_WAIT_CLOSE,
    DELETE_WAIT,
    RUNNING,
} orch_state_t;

static orch_state_t orch_state = BOOT_OPENING;
static uint32_t cur_fd       = 0;
static uint32_t cur_filesize = 0;
static uint32_t cur_offset   = 0;

/* Model loading */
static uint32_t model_loaded_bytes = 0;
static uint32_t model_total_bytes  = 0;
static uint32_t last_progress_pct  = 0;

/* Tokenizer loading */
static uint32_t tok_loaded_bytes    = 0;
static uint32_t tok_total_bytes     = 0;
static uint32_t tok_offset_in_model = 0;

/* Ready flag */
static int model_ready = 0;

static inline void dsb_sy(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}



/* ── FS client (non-blocking) ──────────────────────── */
static void fs_open(const char *filename) {
    WR32(fs_data, FS_CMD, FS_CMD_OPEN);
    char *dst = (char *)(fs_data + FS_FILENAME);
    int i = 0;
    while (filename[i] && i < 255) { dst[i] = filename[i]; i++; }
    dst[i] = '\0';
    microkit_notify(CH_FS);
}

static void fs_read(uint32_t fd, uint32_t offset, uint32_t len) {
    if (len > FS_DATA_MAX) len = FS_DATA_MAX;
    WR32(fs_data, FS_CMD,    FS_CMD_READ);
    WR32(fs_data, FS_FD,     fd);
    WR32(fs_data, FS_OFFSET, offset);
    WR32(fs_data, FS_LENGTH, len);
    microkit_notify(CH_FS);
}

static void fs_close(uint32_t fd) {
    WR32(fs_data, FS_CMD, FS_CMD_CLOSE);
    WR32(fs_data, FS_FD,  fd);
    microkit_notify(CH_FS);
}

/* ── Boot: read hello.txt ──────────────────────────── */
static void handle_boot_open_reply(void) {
    uint32_t status = RD32(fs_data, FS_STATUS);
    if (status == FS_ST_OK) {
        cur_fd       = RD32(fs_data, FS_FD);
        cur_filesize = RD32(fs_data, FS_FILESIZE);
        cur_offset   = 0;
        ser_puts("  File size: ");
        ser_put_dec(cur_filesize);
        ser_puts(" bytes\n");
        uint32_t chunk = cur_filesize;
        if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
        orch_state = BOOT_READING;
        fs_read(cur_fd, 0, chunk);
    } else {
        ser_puts("  File not found.\n");
        microkit_notify(CH_SERIAL);
        orch_state = RUNNING;
        ser_puts("AIOS> ");
        microkit_notify(CH_SERIAL);
    }
}

static void handle_boot_read_reply(void) {
    uint32_t status = RD32(fs_data, FS_STATUS);
    uint32_t got    = RD32(fs_data, FS_LENGTH);
    
    if ((status == FS_ST_OK || status == FS_ST_EOF) && got > 0) {
        ser_puts("  Contents: ");
        volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
        for (uint32_t i = 0; i < got; i++) ser_putc((char)src[i]);
        if (got > 0 && (char)src[got - 1] != '\n') ser_putc('\n');
        microkit_notify(CH_SERIAL);
        cur_offset += got;
        if (cur_offset < cur_filesize) {
            uint32_t chunk = cur_filesize - cur_offset;
            if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
            fs_read(cur_fd, cur_offset, chunk);
        } else {
            orch_state = BOOT_CLOSING;
            fs_close(cur_fd);
        }
    } else {
        orch_state = BOOT_CLOSING;
        fs_close(cur_fd);
    }
}

static void handle_boot_close_reply(void) {
    ser_puts("  File closed.\n\n");
    ser_puts("AIOS> ");
    orch_state = RUNNING;
}

/* ── Model loading ─────────────────────────────────── */
static void start_model_load(const char *filename) {
    model_loaded_bytes = 0;
    model_total_bytes  = 0;
    last_progress_pct  = 0;
    model_ready        = 0;
    orch_state = LOAD_OPENING;
    fs_open(filename);
}

static void handle_load_open_reply(void) {
    uint32_t status = RD32(fs_data, FS_STATUS);
    if (status != FS_ST_OK) {
        ser_puts("  Model file not found!\n");
        ser_puts("AIOS> ");
        orch_state = RUNNING;
        return;
    }
    cur_fd          = RD32(fs_data, FS_FD);
    cur_filesize    = RD32(fs_data, FS_FILESIZE);
    cur_offset      = 0;
    model_total_bytes = cur_filesize;

    ser_puts("  File size: ");
    ser_put_dec(cur_filesize);
    ser_puts(" bytes (");
    ser_put_dec(cur_filesize / 1024);
    ser_puts(" KiB)\n");

    if (cur_filesize > MODEL_DATA_MAX) {
        ser_puts("  ERROR: model too large for memory region!\n");
        orch_state = LOAD_CLOSING;
        fs_close(cur_fd);
        return;
    }

    ser_puts("  Loading: [");
    microkit_notify(CH_SERIAL);
    orch_state = LOAD_READING;
    uint32_t chunk = cur_filesize;
    if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
    fs_read(cur_fd, 0, chunk);
}

static void handle_load_read_reply(void) {
    uint32_t status = RD32(fs_data, FS_STATUS);
    uint32_t got    = RD32(fs_data, FS_LENGTH);

    if ((status == FS_ST_OK || status == FS_ST_EOF) && got > 0) {
        volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
        volatile uint8_t *dst = (volatile uint8_t *)(model_data + model_loaded_bytes);
        for (uint32_t i = 0; i < got; i++) dst[i] = src[i];
        model_loaded_bytes += got;
        cur_offset += got;

        uint32_t pct = 0;
        if (model_total_bytes > 0)
            pct = (uint32_t)((uint64_t)model_loaded_bytes * 100 / model_total_bytes);
        while (last_progress_pct + 2 <= pct) {
            ser_putc('#');
            last_progress_pct += 2;
        }
        microkit_notify(CH_SERIAL);

        if (cur_offset >= cur_filesize || status == FS_ST_EOF) {
            ser_puts("] 100%\n");
            ser_puts("  Loaded ");
            ser_put_dec(model_loaded_bytes);
            ser_puts(" bytes into memory\n");
            microkit_notify(CH_SERIAL);
            orch_state = LOAD_CLOSING;
            fs_close(cur_fd);
        } else {
            uint32_t chunk = cur_filesize - cur_offset;
            if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
            fs_read(cur_fd, cur_offset, chunk);
        }
    } else {
        ser_puts("] ERROR\n");
        microkit_notify(CH_SERIAL);
        orch_state = LOAD_CLOSING;
        fs_close(cur_fd);
    }
}

static void handle_load_close_reply(void) {
    if (model_loaded_bytes > 0) {
        dsb_sy();  // ensure all model writes are visible
        ser_puts("  Notifying LLM server...\n");
        microkit_notify(CH_SERIAL);
        WR32(llm_io, LLM_CMD, LLM_CMD_LOAD_DONE);
        WR32(llm_io, LLM_MODELSZ, model_loaded_bytes);
        orch_state = LOAD_NOTIFY_LLM;
        microkit_notify(CH_LLM);
    } else {
        ser_puts("  Load failed.\n");
        ser_puts("AIOS> ");
        orch_state = RUNNING;
    }
}

static void handle_llm_load_reply(void) {
    uint32_t status = RD32(llm_io, LLM_STATUS);
    if (status == 0) {
        ser_puts("  LLM server acknowledged model.\n");
        /* Auto-load tokenizer */
        ser_puts("  Loading tokenizer...\n");
        microkit_notify(CH_SERIAL);
        tok_offset_in_model = (model_loaded_bytes + 15) & ~15UL;
        tok_loaded_bytes = 0;
        tok_total_bytes  = 0;
        cur_offset       = 0;
        orch_state = TOK_OPENING;
        fs_open("TOK.BIN");
    } else {
        ser_puts("  LLM server rejected model!\n");
        ser_puts("\nAIOS> ");
        orch_state = RUNNING;
    }
}

/* ── Tokenizer loading ─────────────────────────────── */
static void handle_tok_open_reply(void) {
    uint32_t status = RD32(fs_data, FS_STATUS);
    if (status != FS_ST_OK) {
        ser_puts("  Tokenizer file not found!\n");
        ser_puts("AIOS> ");
        orch_state = RUNNING;
        return;
    }
    cur_fd       = RD32(fs_data, FS_FD);
    cur_filesize = RD32(fs_data, FS_FILESIZE);
    cur_offset   = 0;
    tok_total_bytes = cur_filesize;

    ser_puts("  Tokenizer: ");
    ser_put_dec(cur_filesize);
    ser_puts(" bytes\n");
    microkit_notify(CH_SERIAL);

    if (tok_offset_in_model + cur_filesize > MODEL_DATA_MAX) {
        ser_puts("  ERROR: no room for tokenizer!\n");
        orch_state = TOK_CLOSING;
        fs_close(cur_fd);
        return;
    }

    orch_state = TOK_READING;
    uint32_t chunk = cur_filesize;
    if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
    fs_read(cur_fd, 0, chunk);
}

static void handle_tok_read_reply(void) {
    uint32_t status = RD32(fs_data, FS_STATUS);
    uint32_t got    = RD32(fs_data, FS_LENGTH);

    if ((status == FS_ST_OK || status == FS_ST_EOF) && got > 0) {
        volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
        volatile uint8_t *dst = (volatile uint8_t *)(model_data + tok_offset_in_model + tok_loaded_bytes);
        for (uint32_t i = 0; i < got; i++) dst[i] = src[i];
        tok_loaded_bytes += got;
        cur_offset += got;

        if (cur_offset >= cur_filesize || status == FS_ST_EOF) {
            ser_puts("  Tokenizer loaded.\n");
            microkit_notify(CH_SERIAL);
            orch_state = TOK_CLOSING;
            fs_close(cur_fd);
        } else {
            uint32_t chunk = cur_filesize - cur_offset;
            if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
            fs_read(cur_fd, cur_offset, chunk);
        }
    } else {
        ser_puts("  Tokenizer read error!\n");
        orch_state = TOK_CLOSING;
        fs_close(cur_fd);
    }
}

static void handle_tok_close_reply(void) {
    if (tok_loaded_bytes > 0) {
        dsb_sy();  // ensure all writes to model_data are visible
        WR32(llm_io, LLM_CMD, LLM_CMD_LOAD_TOK);
        WR32(llm_io, LLM_TOK_OFF, tok_offset_in_model);
        WR32(llm_io, LLM_TOK_SZ, tok_loaded_bytes);
        orch_state = TOK_NOTIFY_LLM;
        microkit_notify(CH_LLM);
    } else {
        ser_puts("  Tokenizer load failed.\n");
        ser_puts("AIOS> ");
        orch_state = RUNNING;
    }
}

static void handle_tok_llm_reply(void) {
    uint32_t status = RD32(llm_io, LLM_STATUS);
    if (status == 0) {
        ser_puts("  Tokenizer acknowledged. Ready for inference!\n");
        model_ready = 1;
    } else {
        ser_puts("  Tokenizer rejected!\n");
    }
    ser_puts("\nAIOS> ");
    orch_state = RUNNING;
}

/* ── Generation reply ──────────────────────────────── */

static void handle_gen_reply(void) {
    uint32_t status = RD32(llm_io, LLM_STATUS);

    if (status == LLM_ST_TOKEN) {
        /* Print the token piece */
        char *piece = (char *)(llm_io + LLM_OUTPUT);
        ser_puts(piece);
        microkit_notify(CH_SERIAL);
        /* Request next token */
        WR32(llm_io, LLM_CMD, LLM_CMD_NEXT_TOK);
        microkit_notify(CH_LLM);
        /* Stay in GEN_WAITING state */
    } else if (status == LLM_ST_DONE) {
        ser_puts("\n\nAIOS> ");
        microkit_notify(CH_SERIAL);
        orch_state = RUNNING;
    } else {
        ser_puts("\n  Generation error.\n");
        ser_puts("AIOS> ");
        microkit_notify(CH_SERIAL);
        orch_state = RUNNING;
    }
}

/* ── Interactive shell ─────────────────────────────── */
#define INPUT_MAX 128
static char input_line[INPUT_MAX];
static int  input_pos = 0;

static int str_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static void process_command(void) {
    input_line[input_pos] = '\0';

    if (input_pos == 0) {
        /* empty — just re-prompt */
    }
    
    /* ── write <filename> <text> ─────────────────────────── */
    if (str_starts_with(input_line, "write ")) {
        char *p = input_line + 6;
        char fname[64];
        int fi = 0;
        while (*p && *p != ' ' && fi < 63) fname[fi++] = *p++;
        fname[fi] = 0;
        if (*p == ' ') p++; /* skip space before text */

        /* Copy filename into fs_data */
        for (int i = 0; i < fi; i++)
            *(char *)(fs_data + FS_FILENAME + i) = fname[i];
        *(char *)(fs_data + FS_FILENAME + fi) = '\0';

        /* Save the text to write — stash in fs_data+FS_DATA temporarily */
        int tlen = 0;
        while (p[tlen] && tlen < (int)FS_DATA_MAX - 1) {
            *(char *)(fs_data + FS_DATA + tlen) = p[tlen];
            tlen++;
        }
        *(char *)(fs_data + FS_DATA + tlen) = '\0';
        WR32(fs_data, FS_REQLEN, (uint32_t)tlen);

        /* Step 1: create the file */
        WR32(fs_data, FS_CMD, FS_CMD_CREATE);
        orch_state = WRITE_WAIT_CREATE;
        microkit_notify(CH_FS);
        input_pos = 0;
        input_line[0] = '\0';
        return;
    }

    /* ── rm <filename> / del <filename> ──────────────────── */
    else if (str_starts_with(input_line, "rm ") || str_starts_with(input_line, "del ")) {
        char *p = input_line + (input_line[0] == 'r' ? 3 : 4);
        int fi = 0;
        while (p[fi] && fi < 63) {
            *(char *)(fs_data + FS_FILENAME + fi) = p[fi];
            fi++;
        }
        *(char *)(fs_data + FS_FILENAME + fi) = '\0';

        WR32(fs_data, FS_CMD, FS_CMD_DELETE);
        orch_state = DELETE_WAIT;
        microkit_notify(CH_FS);
        input_pos = 0;
        input_line[0] = '\0';
        return;
    }
    else if (str_starts_with(input_line, "help")) {
        ser_puts("Commands:\n");
        ser_puts("  help            - this message\n");
        ser_puts("  cat <file>      - read a file from disk\n");
        ser_puts("  write <f> <txt> - write text to a file\n");
        ser_puts("  rm <file>       - delete a file\n");
        ser_puts("  load <file>     - load model into memory\n");
        ser_puts("  gen <prompt>    - generate text\n");
        ser_puts("  info            - system info\n");
        ser_puts("  shutdown        - halt system\n");
    }
    else if (str_starts_with(input_line, "info")) {
        ser_puts("AIOS v0.4 on seL4 14.0.0 (Microkit 2.1.0)\n");
        ser_puts("PDs: serial_driver, blk_driver, fs_server, orchestrator, llm_server, echo_server\n");
        if (model_ready) {
            ser_puts("Model: ");
            ser_put_dec(model_loaded_bytes);
            ser_puts(" bytes | Tokenizer: ");
            ser_put_dec(tok_loaded_bytes);
            ser_puts(" bytes | Ready\n");
        } else if (model_loaded_bytes > 0) {
            ser_puts("Model: ");
            ser_put_dec(model_loaded_bytes);
            ser_puts(" bytes loaded (tokenizer pending)\n");
        } else {
            ser_puts("Model: none loaded\n");
        }
    } else if (str_starts_with(input_line, "cat ")) {
        char *fname = &input_line[4];
        while (*fname == ' ') fname++;
        if (*fname == '\0') {
            ser_puts("Usage: cat <filename>\n");
        } else {
            ser_puts("Opening ");
            ser_puts(fname);
            ser_puts("...\n");
            orch_state = BOOT_OPENING;
            fs_open(fname);
            input_pos = 0;
            return;
        }
    } else if (str_starts_with(input_line, "load ")) {
        char *fname = &input_line[5];
        while (*fname == ' ') fname++;
        if (*fname == '\0') {
            ser_puts("Usage: load <filename>\n");
        } else {
            ser_puts("Loading model: ");
            ser_puts(fname);
            ser_puts("\n");
            start_model_load(fname);
            input_pos = 0;
            return;
        }
    } else if (str_starts_with(input_line, "gen ") || str_starts_with(input_line, "generate ")) {
            char *prompt = input_line;
            while (*prompt && *prompt != ' ') prompt++;
            while (*prompt == ' ') prompt++;
            if (*prompt == '\0') {
                ser_puts("Usage: gen <prompt>\n");
            } else if (!model_ready) {
                ser_puts("No model/tokenizer loaded. Run: load STORIES.BIN\n");
            } else {
                ser_puts("Generating...\n");
                microkit_notify(CH_SERIAL);
                char *dst = (char *)(llm_io + LLM_PROMPT);
                int i = 0;
                while (prompt[i] && i < 255) { dst[i] = prompt[i]; i++; }
                dst[i] = '\0';
                WR32(llm_io, LLM_MAX_STEPS, 256);
                WR32(llm_io, LLM_CMD, LLM_CMD_GENERATE);
                orch_state = GEN_WAITING;
                microkit_notify(CH_LLM);
                input_pos = 0;
                return;
            }
    } else if (str_starts_with(input_line, "shutdown")) {
        ser_puts("Shutting down AIOS...\n");
        ser_puts("System halted. Press Ctrl-A then X to exit QEMU.\n");
        microkit_notify(CH_SERIAL);
        for (;;) { __asm__ volatile("wfi"); }
    } else {
        ser_puts("Unknown command: ");
        ser_puts(input_line);
        ser_puts("\n  Type 'help' for commands.\n");
    }

    ser_puts("AIOS> ");
    input_pos = 0;
}

static void handle_serial_input(void) {
    ring_buf_t *rx = (ring_buf_t *)rx_buf;
    char c;
    while (ring_get(rx, &c)) {
        if (c == '\r' || c == '\n') {
            ser_putc('\n');
            microkit_notify(CH_SERIAL);
            process_command();
        } else if (c == 0x7F || c == '\b') {
            if (input_pos > 0) {
                input_pos--;
                ser_putc('\b'); ser_putc(' '); ser_putc('\b');
                microkit_notify(CH_SERIAL);
            }
        } else if (input_pos < INPUT_MAX - 1) {
            input_line[input_pos++] = c;
            ser_putc(c);
            microkit_notify(CH_SERIAL);
        }
    }
}

/* ── Microkit entry points ─────────────────────────── */
void init(void) {
    ser_puts("\n========================================\n");
    ser_puts("  AIOS Orchestrator v0.6\n");
    ser_puts("  Kernel:  seL4 14.0.0 (Microkit 2.1.0)\n");
    ser_puts("  Drivers: PL011 UART, virtio-blk, FAT16\n");
    ser_puts("  LLM:     llm_server (llama2.c engine)\n");
    ser_puts("========================================\n\n");
    ser_puts("Boot: reading hello.txt...\n");
    orch_state = BOOT_OPENING;
    fs_open("hello.txt");
}

void notified(microkit_channel ch) {
    switch (ch) {
    case CH_SERIAL:
        if (orch_state == RUNNING)
            handle_serial_input();
        break;

    case CH_FS:
        switch (orch_state) {
        case BOOT_OPENING:  handle_boot_open_reply();  break;
        case BOOT_READING:  handle_boot_read_reply();  break;
        case BOOT_CLOSING:  handle_boot_close_reply();  break;
        case LOAD_OPENING:  handle_load_open_reply();  break;
        case LOAD_READING:  handle_load_read_reply();  break;
        case LOAD_CLOSING:  handle_load_close_reply();  break;
        case TOK_OPENING:   handle_tok_open_reply();   break;
        case TOK_READING:   handle_tok_read_reply();   break;
        case TOK_CLOSING:   handle_tok_close_reply();  break;
                
                
/* ── Write support ──────────────────────────── */
        case WRITE_WAIT_CREATE: {
            int st = (int)RD32(fs_data, FS_STATUS);
            if (st != 0) {
                ser_puts("  Create failed\n");
                microkit_notify(CH_SERIAL);
                orch_state = RUNNING;
                ser_puts("AIOS> ");
                microkit_notify(CH_SERIAL);
                break;
            }
            ser_puts("  File created, writing data...\n");
            microkit_notify(CH_SERIAL);
            /* Send write command — fs_data already has the data
               from process_command */
            WR32(fs_data, FS_CMD, FS_CMD_WRITE);
            orch_state = WRITE_WAIT_DATA;
            microkit_notify(CH_FS);
            break;
        }
        case WRITE_WAIT_DATA: {
            int st = (int)RD32(fs_data, FS_STATUS);
            uint32_t wrote = RD32(fs_data, FS_READLEN);
            if (st != 0) {
                ser_puts("  Write failed\n");
                microkit_notify(CH_SERIAL);
            } else {
                ser_puts("  Wrote ");
                ser_put_dec(wrote);
                ser_puts(" bytes\n");
                microkit_notify(CH_SERIAL);
            }
            /* Close the file */
            WR32(fs_data, FS_CMD, FS_CMD_CLOSE);
            orch_state = WRITE_WAIT_CLOSE;
            microkit_notify(CH_FS);
            break;
        }
        case WRITE_WAIT_CLOSE: {
            ser_puts("  File saved.\n");
            microkit_notify(CH_SERIAL);
            orch_state = RUNNING;
            ser_puts("AIOS> ");
            microkit_notify(CH_SERIAL);
            break;
        }
        case DELETE_WAIT: {
            int st = (int)RD32(fs_data, FS_STATUS);
            if (st == 0)
                ser_puts("  Deleted.\n");
            else
                ser_puts("  Delete failed (file not found?)\n");
            microkit_notify(CH_SERIAL);
            orch_state = RUNNING;
            ser_puts("AIOS> ");
            microkit_notify(CH_SERIAL);
            break;
        }
        default: break;
        }
        break;

    case CH_LLM:
        switch (orch_state) {
        case LOAD_NOTIFY_LLM: handle_llm_load_reply(); break;
        case TOK_NOTIFY_LLM:  handle_tok_llm_reply();  break;
        case GEN_WAITING:     handle_gen_reply();       break;
        default: break;
        }
        break;

    default:
        break;
    }
}

