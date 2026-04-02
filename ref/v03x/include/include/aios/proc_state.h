#ifndef AIOS_PROC_STATE_H
#define AIOS_PROC_STATE_H

/*
 * proc_state_t — Serialized process state for suspend/resume/fork
 *
 * Lives at the start of the sandbox heap (offset 0).
 * Contains all mutable sandbox globals, the CPU register context,
 * and a stack snapshot. The bump allocator starts after this
 * reserved region.
 *
 * Layout in heap:
 *   [0 .. sizeof(proc_state_t))           — process metadata + context
 *   [sizeof(proc_state_t) .. + stack_size) — stack snapshot bytes
 *   [PROC_STATE_RESERVE .. heap_end)       — user heap (bump allocator)
 */

#include <stdint.h>

#define MAX_PROC_FDS  16

/* Reserve 8 KB at heap start for proc_state + stack snapshot */
#define PROC_STATE_RESERVE  (8 * 1024)

/* Arch context: must match arch_context_t from arch/aarch64/context.h */
/* 13 uint64_t fields = 104 bytes: x19-x28, x29(fp), x30(lr), sp */
#define PROC_CTX_SIZE  104

typedef struct {
    /* Magic number to validate state */
    uint32_t magic;             /* PROC_STATE_MAGIC = 0x50524F43 */
    uint32_t version;           /* Structure version, currently 1 */

    /* Sandbox mutable globals */
    uint32_t heap_used;         /* Bump allocator watermark */
    uint32_t out_len;           /* Output buffer position */
    uint32_t puts_count;        /* puts call counter */
    uint32_t _pad0;

    uintptr_t stack_top;        /* SP recorded at run_program entry */

    /* File descriptor state */
    uint32_t fd_pos[MAX_PROC_FDS];
    uint32_t fd_size[MAX_PROC_FDS];

    /* CPU register context (arch_context_t) */
    uint8_t  ctx[PROC_CTX_SIZE];

    /* Stack snapshot metadata */
    uintptr_t saved_sp;         /* SP at save time */
    uint32_t  saved_stack_size; /* Bytes of stack saved */

    /* Syscall state — the result the process was about to receive */
    int32_t   pending_result;

    /* Process identity (from orchestrator) */
    int32_t   pid;
    int32_t   parent_pid;

    /* Flags */
    uint32_t  is_valid;         /* Non-zero if state is populated */
    uint32_t  suspended;        /* Non-zero if this is a suspend (vs fork) */

    /* Stack data follows immediately after this struct,
       up to saved_stack_size bytes */
} proc_state_t;

/*
 * proc_state_t size is approximately:
 *   4+4 + 4+4+4+4 + 8 + 64+64 + 104 + 8+4 + 4 + 4+4 + 4+4 = ~300 bytes
 *
 * With a max stack snapshot of ~7 KB, fits in PROC_STATE_RESERVE (8 KB).
 */

#define PROC_STATE_MAGIC  0x50524F43

/* Maximum stack snapshot size = reserve minus the struct header */
#define PROC_STACK_MAX  (PROC_STATE_RESERVE - (int)sizeof(proc_state_t))

#endif /* AIOS_PROC_STATE_H */
