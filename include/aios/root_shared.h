#ifndef AIOS_ROOT_SHARED_H
#define AIOS_ROOT_SHARED_H

/*
 * AIOS root_shared.h -- Shared types, constants, and externs
 *
 * All server threads and extracted modules include this header
 * to access the root task shared state.
 */

#include <stdint.h>
#include <sel4/sel4.h>
#include <allocman/bootstrap.h>
#include <allocman/vka.h>
#include <sel4utils/vspace.h>
#include <sel4utils/process.h>
#include <sel4utils/process_config.h>
#include <simple/simple.h>
#include <vka/capops.h>
#include <vka/object.h>
#include "aios/ext2.h"

/* ── IPC protocol labels ── */

#define SER_KEY_PUSH    4

#define EXEC_RUN        20
#define EXEC_NICE       21
#define EXEC_RUN_BG     24
#define EXEC_FORK       25
#define EXEC_WAIT       26

#define THREAD_CREATE   30
#define THREAD_JOIN     31

#define PIPE_CREATE     60
#define PIPE_WRITE      61
#define PIPE_READ       62
#define PIPE_CLOSE      63
#define PIPE_KILL       64
#define PIPE_FORK       65
#define PIPE_GETPID     66
#define PIPE_WAIT       67
#define PIPE_EXIT       68
#define PIPE_EXEC       69
#define PIPE_CLOSE_WRITE 70
#define PIPE_DEBUG       71
#define PIPE_EXEC_WAIT   72
#define PIPE_CLOSE_READ  73
#define PIPE_SET_IDENTITY 74
#define PIPE_SIGNAL       75
#define PIPE_SIG_FETCH    76
#define PIPE_SHUTDOWN     77
#define PIPE_MAP_SHM     78
#define PIPE_WRITE_SHM   79
#define PIPE_READ_SHM    80
#define PIPE_SET_PIPES   81

/* ── Limits ── */

#define MAX_ACTIVE_PROCS     16
#define MAX_THREADS_PER_PROC 8
#define THREAD_STACK_PAGES   4
#define MAX_ELF_SEGS         6
#define MAX_PIPES            8
#define PIPE_BUF_SIZE        4096
#define MAX_WAIT_PENDING     4
#define MAX_ZOMBIES          8
#define MAX_EXEC_ARGS        12
#define MAX_PIPE_READ_BLOCKED 4

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* ── Types ── */

typedef struct {
    int active;
    int tid;
    vka_object_t tcb;
    vka_object_t fault_ep;
    vka_object_t ipc_frame;
    vka_object_t stack_frames[THREAD_STACK_PAGES];
    int exited;
} aios_thread_t;

typedef struct {
    uintptr_t vaddr;
    size_t    memsz;
    uint32_t  flags;   /* PF_X=1, PF_W=2, PF_R=4 */
} elf_seg_info_t;

typedef struct {
    int active;
    int pid;
    int ppid;
    uint32_t uid;
    uint32_t gid;
    sel4utils_process_t proc;
    vka_object_t fault_ep;
    int num_threads;
    aios_thread_t threads[MAX_THREADS_PER_PROC];
    int num_segs;
    elf_seg_info_t segs[MAX_ELF_SEGS];
    seL4_CPtr child_ser_slot;
    seL4_CPtr child_fs_slot;
    seL4_CPtr child_exec_slot;
    seL4_CPtr child_auth_slot;
    seL4_CPtr child_pipe_slot;
    seL4_CPtr child_thread_slot;
    int exit_status;
    int fault_on_pipe_ep;
    int stdout_pipe_id;
    int stdin_pipe_id;
    uint32_t sig_pending;        /* pending signal bitmask */
} active_proc_t;

typedef struct {
    int active;
    char buf[PIPE_BUF_SIZE];      /* fallback static buffer */
    char *shm_buf;                /* v0.4.65: mapped shared frame (or buf) */
    vka_object_t shm_frame;       /* v0.4.65: backing frame object */
    int shm_valid;                /* v0.4.65: 1 if shm_frame allocated */
    char *xfer_buf;               /* v0.4.66: transfer page mapped in root */
    vka_object_t xfer_frame;      /* v0.4.66: transfer frame object */
    int xfer_valid;               /* v0.4.66: 1 if xfer_frame allocated */
    seL4_CPtr xfer_copies[2];    /* v0.4.67: cap copies for child mappings */
    int xfer_copy_count;          /* v0.4.67: number of active copies */
    int head;
    int count;
    int read_closed;
    int write_closed;
    int read_refs;
    int write_refs;
} pipe_t;

typedef struct {
    int active;
    int waiting_pid;
    int child_pid;
    seL4_CPtr reply_cap;
} wait_pending_t;

typedef struct {
    int active;
    int pid;
    int ppid;
    int exit_status;
} zombie_t;

/* ── Shared global state (defined in aios_root.c) ── */

typedef struct {
    int active;
    int pipe_id;
    int max_len;
    int is_shm;                   /* v0.4.66: 1 if reader wants SHM reply */
    seL4_CPtr reply_cap;
} pipe_read_blocked_t;

extern vka_t vka;
extern vspace_t vspace;
extern simple_t simple;
extern allocman_t *allocman;

extern active_proc_t active_procs[MAX_ACTIVE_PROCS];
extern seL4_CPtr thread_ep_cap;
extern seL4_CPtr pipe_ep_cap;
extern seL4_CPtr auth_ep_cap;
extern seL4_CPtr fs_ep_cap;
extern vka_object_t serial_ep;
extern uint32_t aios_total_mem;

extern pipe_t pipes[MAX_PIPES];
extern char elf_buf[8 * 1024 * 1024];

extern ext2_ctx_t ext2;
extern volatile uint32_t *blk_vio;
extern uint8_t *blk_dma;
extern uint64_t blk_dma_pa;

/* Log drive (second virtio-blk) */
extern ext2_ctx_t ext2_log;
extern volatile uint32_t *blk_vio_log;
extern uint8_t *blk_dma_log;
extern uint64_t blk_dma_pa_log;

extern volatile uint32_t *uart;

extern volatile int fg_pid;
extern volatile seL4_CPtr fg_fault_ep;
extern volatile int fg_killed;

extern wait_pending_t wait_pending[MAX_WAIT_PENDING];
extern vka_object_t wait_reply_objects[MAX_WAIT_PENDING];
extern zombie_t zombies[MAX_ZOMBIES];
extern int wait_pending_init;

/* ── Cross-module function declarations ── */
/* Add declarations here as functions are extracted from aios_root.c */

/* Boot phases (src/boot/) */
void boot_fs_init(void);
void boot_start_services(vka_object_t *fault_ep);

/* Block I/O (src/boot/blk_io.c) */
int blk_read_sector(uint64_t sector, void *buf);
int blk_write_sector(uint64_t sector, const void *buf);

/* Log drive I/O (src/boot/blk_io.c) */
int blk_read_sector_log(uint64_t sector, void *buf);
int blk_write_sector_log(uint64_t sector, const void *buf);

/* Log drive init (src/boot/boot_log_init.c) */
void boot_log_drive_init(void *vio_vaddr, int log_slot);

/* Spawn utilities (src/boot/spawn_util.c) */
int spawn_with_args(const char *name, uint8_t prio,
                    sel4utils_process_t *proc,
                    vka_object_t *fault_ep,
                    int ep_count, seL4_CPtr *eps,
                    seL4_CPtr *child_slots);
int spawn_simple(const char *name, uint8_t prio,
                 sel4utils_process_t *proc,
                 vka_object_t *fault_ep);

int do_fork(int parent_idx);
void wait_init(void);
void reap_forked_child(int child_idx);
void reap_check(void);
int create_child_thread(int proc_idx, seL4_Word entry, seL4_Word arg, int *out_tid);
void thread_server_fn(void *arg0, void *arg1, void *ipc_buf);
void fs_thread_fn(void *arg0, void *arg1, void *ipc_buf);
void exec_thread_fn(void *arg0, void *arg1, void *ipc_buf);
int process_kill(int pid);
void pipe_server_fn(void *arg0, void *arg1, void *ipc_buf);
void aios_system_shutdown(void);

#endif /* AIOS_ROOT_SHARED_H */
