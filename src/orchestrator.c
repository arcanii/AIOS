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
uintptr_t sandbox_io;
uintptr_t sandbox_code;

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
    AI_REF_OPENING,
    AI_REF_READING,
    AI_REF_CLOSING,
    AI_GEN_WAITING,
    AI_SAVE_CREATE,
    AI_SAVE_WRITE,
    AI_SAVE_CLOSE,
    EXEC_OPENING,
    EXEC_READING,
    EXEC_CLOSING,
    EXEC_RUNNING,
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

/* AI build state */
#define AI_GEN_BUF_MAX  4096
static char ai_gen_buf[AI_GEN_BUF_MAX];
static uint32_t ai_gen_len = 0;
static char ai_target_name[16];    /* e.g. "LS" */
static char ai_target_file[24];    /* e.g. "LS.C" */
static char ai_ref_file[24];       /* e.g. "R_LS.C" */
static char ai_ref_buf[2048];      /* reference file content */
static uint32_t ai_ref_len = 0;

/* Exec state */
static uint32_t exec_loaded_bytes = 0;

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
        ser_puts("  ai build <name> - AI generates a C program\n");
        ser_puts("  exec <file.bin> - run a sandbox program\n");
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
                while (prompt[i] && i < LLM_PROMPT_MAX - 1) { dst[i] = prompt[i]; i++; }
                dst[i] = '\0';
                WR32(llm_io, LLM_MAX_STEPS, 256);
                WR32(llm_io, LLM_CMD, LLM_CMD_GENERATE);
                orch_state = GEN_WAITING;
                microkit_notify(CH_LLM);
                input_pos = 0;
                return;
            }
    } else if (str_starts_with(input_line, "ai build ")) {
        char *name = input_line + 9;
        while (*name == ' ') name++;
        if (*name == '\0' || !model_ready) {
            if (!model_ready)
                ser_puts("No model loaded. Run: load STORIES.BIN\n");
            else
                ser_puts("Usage: ai build <name>\n");
            microkit_notify(CH_SERIAL);
        } else {
            /* Copy target name (uppercase, no extension) */
            int ni = 0;
            while (name[ni] && name[ni] != ' ' && name[ni] != '.' && ni < 15) {
                char c = name[ni];
                if (c >= 'a' && c <= 'z') c -= 32;
                ai_target_name[ni] = c;
                ni++;
            }
            ai_target_name[ni] = '\0';
            /* Build filenames: R_<NAME>.C and <NAME>.C */
            int j = 0;
            ai_ref_file[j++] = 'R'; ai_ref_file[j++] = '_';
            for (int k = 0; k < ni; k++) ai_ref_file[j++] = ai_target_name[k];
            ai_ref_file[j++] = '.'; ai_ref_file[j++] = 'C'; ai_ref_file[j] = '\0';
            j = 0;
            for (int k = 0; k < ni; k++) ai_target_file[j++] = ai_target_name[k];
            ai_target_file[j++] = '.'; ai_target_file[j++] = 'C'; ai_target_file[j] = '\0';
            ai_gen_len = 0;
            ai_ref_len = 0;
            ser_puts("AI Build: reading reference R_");
            ser_puts(ai_target_name);
            ser_puts(".C ...\n");
            microkit_notify(CH_SERIAL);
            orch_state = AI_REF_OPENING;
            fs_open(ai_ref_file);
            input_pos = 0;
            return;
        }
    } else if (str_starts_with(input_line, "exec ")) {
        char *fname = &input_line[5];
        while (*fname == ' ') fname++;
        if (*fname == '\0') {
            ser_puts("Usage: exec <file.bin>\n");
        } else {
            ser_puts("Loading program: ");
            ser_puts(fname);
            ser_puts("\n");
            microkit_notify(CH_SERIAL);
            exec_loaded_bytes = 0;
            orch_state = EXEC_OPENING;
            fs_open(fname);
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


/* ── AI Build: read reference file ─────────────────── */
static void handle_ai_ref_open_reply(void) {
    uint32_t status = RD32(fs_data, FS_STATUS);
    if (status == FS_ST_OK) {
        cur_fd       = RD32(fs_data, FS_FD);
        cur_filesize = RD32(fs_data, FS_FILESIZE);
        cur_offset   = 0;
        ai_ref_len   = 0;
        ser_puts("  Reference file: ");
        ser_put_dec(cur_filesize);
        ser_puts(" bytes\n");
        microkit_notify(CH_SERIAL);
        uint32_t chunk = cur_filesize;
        if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
        orch_state = AI_REF_READING;
        fs_read(cur_fd, 0, chunk);
    } else {
        ser_puts("  Reference file not found. Generating without context.\n");
        microkit_notify(CH_SERIAL);
        ai_ref_len = 0;
        /* Skip straight to generation */
        goto ai_start_gen;
    }
    return;
ai_start_gen:
    {
        /* Build prompt: instruction + (optional) reference */
        char *dst = (char *)(llm_io + LLM_PROMPT);
        int pi = 0;
        /* Instruction */
        const char *instr = "Write a C program called ";
        while (*instr && pi < LLM_PROMPT_MAX - 256) dst[pi++] = *instr++;
        for (int k = 0; ai_target_name[k] && pi < LLM_PROMPT_MAX - 256; k++)
            dst[pi++] = ai_target_name[k];
        const char *instr2 = ". Output only valid C code. ";
        while (*instr2 && pi < LLM_PROMPT_MAX - 200) dst[pi++] = *instr2++;
        if (ai_ref_len > 0) {
            const char *ctx = "Here is a reference:\n";
            while (*ctx && pi < LLM_PROMPT_MAX - ai_ref_len - 10)
                dst[pi++] = *ctx++;
            for (uint32_t k = 0; k < ai_ref_len && pi < LLM_PROMPT_MAX - 2; k++)
                dst[pi++] = ai_ref_buf[k];
        }
        dst[pi] = '\0';
        ser_puts("  Prompt (");
        ser_put_dec(pi);
        ser_puts(" bytes), generating...\n");
        microkit_notify(CH_SERIAL);
        ai_gen_len = 0;
        WR32(llm_io, LLM_MAX_STEPS, 512);
        WR32(llm_io, LLM_CMD, LLM_CMD_GENERATE);
        orch_state = AI_GEN_WAITING;
        microkit_notify(CH_LLM);
    }
}

static void handle_ai_ref_read_reply(void) {
    uint32_t status = RD32(fs_data, FS_STATUS);
    uint32_t got    = RD32(fs_data, FS_LENGTH);
    if ((status == FS_ST_OK || status == FS_ST_EOF) && got > 0) {
        volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
        for (uint32_t i = 0; i < got && ai_ref_len < sizeof(ai_ref_buf) - 1; i++)
            ai_ref_buf[ai_ref_len++] = (char)src[i];
        cur_offset += got;
        if (cur_offset >= cur_filesize || status == FS_ST_EOF) {
            ai_ref_buf[ai_ref_len] = '\0';
            orch_state = AI_REF_CLOSING;
            fs_close(cur_fd);
        } else {
            uint32_t chunk = cur_filesize - cur_offset;
            if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
            fs_read(cur_fd, cur_offset, chunk);
        }
    } else {
        ai_ref_buf[ai_ref_len] = '\0';
        orch_state = AI_REF_CLOSING;
        fs_close(cur_fd);
    }
}

static void handle_ai_ref_close_reply(void) {
    ser_puts("  Reference loaded (");
    ser_put_dec(ai_ref_len);
    ser_puts(" bytes).\n");
    microkit_notify(CH_SERIAL);
    /* Now build the prompt and start generation */
    char *dst = (char *)(llm_io + LLM_PROMPT);
    int pi = 0;
    /* Prompt: paste reference then start a new function */
    if (ai_ref_len > 0) {
        for (uint32_t k = 0; k < ai_ref_len && pi < LLM_PROMPT_MAX - 100; k++)
            dst[pi++] = ai_ref_buf[k];
    }
    dst[pi] = '\0';
    ser_puts("  Prompt (");
    ser_put_dec(pi);
    ser_puts(" bytes), generating...\n");
    microkit_notify(CH_SERIAL);
    ai_gen_len = 0;
    WR32(llm_io, LLM_MAX_STEPS, 512);
    WR32(llm_io, LLM_CMD, LLM_CMD_GENERATE);
    orch_state = AI_GEN_WAITING;
    microkit_notify(CH_LLM);
}

static void handle_ai_gen_reply(void) {
    uint32_t status = RD32(llm_io, LLM_STATUS);
    if (status == LLM_ST_TOKEN) {
        char *piece = (char *)(llm_io + LLM_OUTPUT);
        /* Print to console */
        ser_puts(piece);
        microkit_notify(CH_SERIAL);
        /* Accumulate into ai_gen_buf */
        for (int i = 0; piece[i] && ai_gen_len < AI_GEN_BUF_MAX - 1; i++)
            ai_gen_buf[ai_gen_len++] = piece[i];
        /* Request next token */
        WR32(llm_io, LLM_CMD, LLM_CMD_NEXT_TOK);
        microkit_notify(CH_LLM);
    } else if (status == LLM_ST_DONE) {
        ai_gen_buf[ai_gen_len] = '\0';
        ser_puts("\n  Generation done (");
        ser_put_dec(ai_gen_len);
        ser_puts(" bytes). Saving to ");
        ser_puts(ai_target_file);
        ser_puts("...\n");
        microkit_notify(CH_SERIAL);
        /* Write to disk: first create the file */
        char *fn = (char *)(fs_data + FS_FILENAME);
        int i = 0;
        while (ai_target_file[i]) { fn[i] = ai_target_file[i]; i++; }
        fn[i] = '\0';
        /* Copy generated code into fs_data */
        for (uint32_t k = 0; k < ai_gen_len && k < FS_DATA_MAX; k++)
            *(char *)(fs_data + FS_DATA + k) = ai_gen_buf[k];
        WR32(fs_data, FS_REQLEN, ai_gen_len < FS_DATA_MAX ? ai_gen_len : FS_DATA_MAX);
        WR32(fs_data, FS_CMD, FS_CMD_CREATE);
        orch_state = AI_SAVE_CREATE;
        microkit_notify(CH_FS);
    } else {
        ser_puts("\n  AI generation error.\n");
        ser_puts("AIOS> ");
        microkit_notify(CH_SERIAL);
        orch_state = RUNNING;
    }
}

static void handle_ai_save_create_reply(void) {
    int st = (int)RD32(fs_data, FS_STATUS);
    if (st != 0) {
        ser_puts("  Save failed (create error).\n");
        ser_puts("AIOS> ");
        microkit_notify(CH_SERIAL);
        orch_state = RUNNING;
        return;
    }
    WR32(fs_data, FS_CMD, FS_CMD_WRITE);
    orch_state = AI_SAVE_WRITE;
    microkit_notify(CH_FS);
}

static void handle_ai_save_write_reply(void) {
    int st = (int)RD32(fs_data, FS_STATUS);
    if (st != 0) {
        ser_puts("  Save failed (write error).\n");
    }
    WR32(fs_data, FS_CMD, FS_CMD_CLOSE);
    orch_state = AI_SAVE_CLOSE;
    microkit_notify(CH_FS);
}

static void handle_ai_save_close_reply(void) {
    ser_puts("  Saved ");
    ser_puts(ai_target_file);
    ser_puts(" (");
    ser_put_dec(ai_gen_len);
    ser_puts(" bytes) to disk.\n");
    ser_puts("AIOS> ");
    microkit_notify(CH_SERIAL);
    orch_state = RUNNING;
}


/* ── Exec handlers ─────────────────────────────────── */

static void handle_exec_open_reply(void) {
    uint32_t status = RD32(fs_data, FS_STATUS);
    if (status != FS_ST_OK) {
        ser_puts("  File not found.\n");
        microkit_notify(CH_SERIAL);
        orch_state = RUNNING;
        ser_puts("AIOS> ");
        microkit_notify(CH_SERIAL);
        return;
    }
    cur_fd = RD32(fs_data, FS_FD);
    cur_filesize = RD32(fs_data, FS_FILESIZE);
    ser_puts("  Program size: ");
    ser_put_dec(cur_filesize);
    ser_puts(" bytes\n");
    microkit_notify(CH_SERIAL);

    if (cur_filesize > 0x100000) {  /* 1 MiB sandbox_code limit */
        ser_puts("  Error: program too large (max 1 MiB)\n");
        microkit_notify(CH_SERIAL);
        orch_state = EXEC_CLOSING;
        fs_close(cur_fd);
        return;
    }

    exec_loaded_bytes = 0;
    cur_offset = 0;
    uint32_t chunk = cur_filesize;
    if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
    orch_state = EXEC_READING;
    fs_read(cur_fd, 0, chunk);
}

static void handle_exec_read_reply(void) {
    uint32_t status = RD32(fs_data, FS_STATUS);
    uint32_t got    = RD32(fs_data, FS_LENGTH);

    if ((status == FS_ST_OK || status == FS_ST_EOF) && got > 0) {
        /* Copy chunk into sandbox_code region */
        volatile uint8_t *src = (volatile uint8_t *)(fs_data + FS_DATA);
        volatile uint8_t *dst = (volatile uint8_t *)(sandbox_code + exec_loaded_bytes);
        for (uint32_t i = 0; i < got; i++) dst[i] = src[i];
        exec_loaded_bytes += got;
        cur_offset += got;

        if (cur_offset < cur_filesize) {
            uint32_t chunk = cur_filesize - cur_offset;
            if (chunk > FS_DATA_MAX) chunk = FS_DATA_MAX;
            fs_read(cur_fd, cur_offset, chunk);
        } else {
            orch_state = EXEC_CLOSING;
            fs_close(cur_fd);
        }
    } else {
        orch_state = EXEC_CLOSING;
        fs_close(cur_fd);
    }
}

static void handle_exec_close_reply(void) {
    if (exec_loaded_bytes == 0) {
        ser_puts("  No code loaded.\n");
        microkit_notify(CH_SERIAL);
        orch_state = RUNNING;
        ser_puts("AIOS> ");
        microkit_notify(CH_SERIAL);
        return;
    }
    ser_puts("  Loaded ");
    ser_put_dec(exec_loaded_bytes);
    ser_puts(" bytes into sandbox. Executing...\n");
    microkit_notify(CH_SERIAL);

    /* Tell sandbox to run */
    WR32(sandbox_io, SBX_CODE_SIZE, exec_loaded_bytes);
    WR32(sandbox_io, SBX_CMD, SBX_CMD_RUN);
    orch_state = EXEC_RUNNING;
    microkit_notify(CH_SANDBOX);
}

static void handle_exec_done(void) {
    uint32_t status = RD32(sandbox_io, SBX_STATUS);
    if (status == SBX_ST_DONE) {
        uint32_t exit_code = RD32(sandbox_io, SBX_EXIT_CODE);
        uint32_t out_len   = RD32(sandbox_io, SBX_OUTPUT_LEN);

        if (out_len > 0) {
            ser_puts("--- program output ---\n");
            microkit_notify(CH_SERIAL);
            volatile char *out = (volatile char *)(sandbox_io + SBX_OUTPUT);
            for (uint32_t i = 0; i < out_len && i < SBX_OUTPUT_MAX; i++)
                ser_putc(out[i]);
            if (out_len > 0 && out[out_len - 1] != '\n') ser_putc('\n');
            ser_puts("--- end output ---\n");
            microkit_notify(CH_SERIAL);
        }
        ser_puts("  Exit code: ");
        ser_put_dec(exit_code);
        ser_putc('\n');
        microkit_notify(CH_SERIAL);
    } else {
        ser_puts("  Execution error.\n");
        microkit_notify(CH_SERIAL);
    }
    orch_state = RUNNING;
    ser_puts("AIOS> ");
    microkit_notify(CH_SERIAL);
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
            WR32(fs_data, FS_LENGTH, RD32(fs_data, FS_REQLEN));
            WR32(fs_data, FS_CMD, FS_CMD_WRITE);
            orch_state = WRITE_WAIT_DATA;
            microkit_notify(CH_FS);
            break;
        }
        case WRITE_WAIT_DATA: {
            int st = (int)RD32(fs_data, FS_STATUS);
            uint32_t wrote = RD32(fs_data, FS_LENGTH);
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
        /* ── AI Build: file I/O ─────────────── */
        case AI_REF_OPENING: handle_ai_ref_open_reply();      break;
        case AI_REF_READING: handle_ai_ref_read_reply();      break;
        case AI_REF_CLOSING: handle_ai_ref_close_reply();     break;
        case AI_SAVE_CREATE: handle_ai_save_create_reply();   break;
        case AI_SAVE_WRITE:  handle_ai_save_write_reply();    break;
        case AI_SAVE_CLOSE:  handle_ai_save_close_reply();    break;
        case EXEC_OPENING:  handle_exec_open_reply();  break;
        case EXEC_READING:  handle_exec_read_reply();  break;
        case EXEC_CLOSING:  handle_exec_close_reply(); break;
        default: break;
        }
        break;

    case CH_LLM:
        switch (orch_state) {
        case LOAD_NOTIFY_LLM: handle_llm_load_reply(); break;
        case TOK_NOTIFY_LLM:  handle_tok_llm_reply();  break;
        case GEN_WAITING:     handle_gen_reply();       break;
        case AI_GEN_WAITING:  handle_ai_gen_reply();    break;
        default: break;
        }
        break;

    case CH_SANDBOX:
        if (orch_state == EXEC_RUNNING)
            handle_exec_done();
        break;

    default:
        break;
    }
}

