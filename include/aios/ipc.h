/*
 * AIOS – Shared-memory IPC layouts
 *
 * Offsets into the shared buffers (fs_data, blk_data, llm_io).
 * Both producer and consumer must agree on these.
 */
#ifndef AIOS_IPC_H
#define AIOS_IPC_H

#include <stdint.h>

/* ── Helper macros ─────────────────────────────── */
#define WR32(base, off, v)  (*(volatile uint32_t *)((base) + (off)) = (v))
#define RD32(base, off)     (*(volatile uint32_t *)((base) + (off)))

/* ── fs_data layout (4 KiB page) ───────────────── */
#define FS_CMD          0x000
#define FS_STATUS       0x004
#define FS_FD           0x008
#define FS_OFFSET       0x00C
#define FS_LENGTH       0x010
#define FS_FILESIZE     0x014
#define FS_FILENAME     0x100   /* 256 bytes for filename */
#define FS_DATA         0x200   /* file data payload      */
#define FS_DATA_MAX     3584    /* 0x1000 - 0x200         */

/* fs commands */
#define FS_CMD_OPEN     1
#define FS_CMD_READ     2
#define FS_CMD_CLOSE    3

/* fs status codes */
#define FS_ST_OK        0
#define FS_ST_NOT_FOUND 1
#define FS_ST_IO_ERR    2
#define FS_ST_EOF       3

/* ── blk_data layout (4 KiB page) ─────────────── */
#define BLK_CMD         0x000
#define BLK_SECTOR      0x004
#define BLK_STATUS      0x008
#define BLK_DATA        0x200   /* 512-byte sector payload */
#define BLK_SECTOR_SIZE 512

/* blk commands */
#define BLK_CMD_READ    0
#define BLK_CMD_WRITE   1

/* ── llm_io layout (4 KiB page) ───────────────── */
#define LLM_CMD         0x000
#define LLM_STATUS      0x004
#define LLM_MODELSZ     0x008
#define LLM_TOK_OFF     0x00C
#define LLM_TOK_SZ      0x010
#define LLM_MAX_STEPS   0x014
#define LLM_PROMPT      0x100   /* 256 bytes for prompt   */
#define LLM_OUTPUT      0x200   /* generated text output  */
#define LLM_OUTPUT_MAX  3584

/* llm commands */
#define LLM_CMD_LOAD_DONE  1
#define LLM_CMD_LOAD_TOK   2
#define LLM_CMD_GENERATE   3
#define LLM_CMD_NEXT_TOK   4   /* orchestrator -> llm: "ready for next token" */

/* Status codes for streaming */
#define LLM_ST_OK          0
#define LLM_ST_ERROR       1
#define LLM_ST_TOKEN       2   /* llm -> orch: here is a token piece */
#define LLM_ST_DONE        3   /* llm -> orch: generation finished */

/* ── model_data region ─────────────────────────── */
#define MODEL_DATA_MAX  0x8000000   /* 128 MiB */

#endif /* AIOS_IPC_H */

