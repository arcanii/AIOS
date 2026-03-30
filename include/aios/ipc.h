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
#define FS_CMD       0x000
#define FS_STATUS    0x004
#define FS_FD        0x008
#define FS_FILESIZE  0x00C
#define FS_REQLEN    0x010
#define FS_READLEN   0x014
#define FS_OFFSET    0x018   /* dedicated field: file offset    */
#define FS_UID       0x020   /* file owner uid                  */
#define FS_GID       0x024   /* file owner gid                  */
#define FS_MODE      0x028   /* file permission mode            */
#define FS_MTIME     0x02C   /* file modification time           */
#define FS_CREAT_UID 0x030   /* uid for file creation            */
#define FS_CREAT_GID 0x034   /* gid for file creation            */
#define FS_LENGTH    0x01C   /* dedicated field: request length  */
#define FS_FILENAME  0x040
#define FS_DATA      0x200
#define FS_DATA_MAX  65024

/* ── FS commands (orchestrator → fs_server) ─────────────── */
#define FS_CMD_OPEN      1
#define FS_CMD_READ      2
#define FS_CMD_CLOSE     3
#define FS_CMD_CREATE    4   /* NEW: create a file        */
#define FS_CMD_WRITE     5   /* NEW: write to open file   */
#define FS_CMD_DELETE    6
#define FS_CMD_LIST      7
#define FS_CMD_STAT      8
#define FS_CMD_SYNC      9
#define FS_CMD_FSINFO   10   /* returns FS name in FS_FILENAME */
#define FS_CMD_MKDIR    11
#define FS_CMD_RMDIR    12
#define FS_CMD_RENAME   13
#define FS_CMD_CHMOD    14
#define FS_CMD_CHOWN    15
#define FS_CMD_STAT_EX  16   /* extended stat: returns uid/gid/mode */

/* fs status codes */
#define FS_ST_OK        0
#define FS_ST_NOT_FOUND 1
#define FS_ST_IO_ERR    2
#define FS_ST_EOF       3

/* ── blk_data layout (4 KiB page) ─────────────── */
#define BLK_CMD         0x000
#define BLK_SECTOR      0x004
#define BLK_STATUS      0x008
#define BLK_COUNT       0x00C   /* number of sectors */
#define BLK_DATA_MAX    64512   /* 0x10000 - 0x200 */
#define BLK_DATA        0x200   /* 512-byte sector payload */
#define BLK_SECTOR_SIZE 512

/* blk commands */
#define BLK_CMD_READ    1
#define BLK_CMD_WRITE   2

/* ── llm_io layout (4 KiB page) ───────────────── */
#define LLM_CMD         0x000
#define LLM_STATUS      0x004
#define LLM_MODELSZ     0x008
#define LLM_TOK_OFF     0x00C
#define LLM_TOK_SZ      0x010
#define LLM_MAX_STEPS   0x014
#define LLM_PROMPT      0x100   /* 4 KiB for prompt (ends at 0x1100) */

#define LLM_PROMPT_MAX  4096
#define LLM_OUTPUT      0x1100  /* generated text output  */
#define LLM_OUTPUT_MAX  12032   /* 0x4000 - 0x1100 = 12032 */

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


/* ── sandbox_io layout (4 KiB page) ───────────── */
#define SBX_CMD         0x000
#define SBX_STATUS      0x004
#define SBX_CODE_SIZE   0x008   /* size of code in sandbox_code region */
#define SBX_EXIT_CODE   0x00C   /* exit code from program */
#define SBX_OUTPUT      0x100   /* 3840 bytes for program stdout */
#define SBX_OUTPUT_LEN  0x010
#define SBX_ARGS        0x014   /* null-terminated args string (max 236 bytes) */
#define SBX_ARGS_MAX    236
#define SBX_EXEC_PARENT_SIZE  0x0F0  /* saved parent code size */
#define SBX_EXEC_CHILD_SIZE   0x0F4  /* loaded child code size */
#define SBX_EXEC_MAGIC        0x45584543  /* "EXEC" */

#define SBX_OUTPUT_MAX  3840

#define SBX_CMD_NOP     0
#define SBX_CMD_RUN     1       /* execute code at sandbox_code */
#define SBX_CMD_HALT    2

#define SBX_ST_IDLE     0
#define SBX_ST_RUNNING  1
#define SBX_ST_DONE     2
#define SBX_ST_ERROR    3

/* ── Sandbox PPC syscall numbers ───────────────────── */

/* ── Network driver IPC layout ─────────────────────────── */
#define NET_CMD       0x000
#define NET_STATUS    0x004
#define NET_PKT_LEN   0x008
#define NET_MAC_OFF   0x010
#define NET_PKT_DATA  0x100
#define NET_DATA_MAX  (0x10000 - 0x100)

#define NET_CMD_NONE   0
#define NET_CMD_SEND   1
#define NET_CMD_RECV   2
#define NET_CMD_GET_MAC 3
#define NET_ST_OK      0
#define NET_ST_ERR    (-1)
#define NET_ST_NODATA  1
