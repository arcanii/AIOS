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

/* v0.4.121: no-op heartbeat ping. Any server that recognises this label
 * should reply MR0=0 immediately. Used by the serverstats probe thread
 * to verify that each server's main loop is still draining its endpoint. */
#define SVC_PING        5

#define EXEC_RUN        20
#define EXEC_NICE       21
#define EXEC_RUN_BG     24
#define EXEC_FORK       25
#define EXEC_WAIT       26

#define THREAD_CREATE   30
#define THREAD_JOIN     31
/* v0.4.191: thread-exit faults arrive on the thread_server endpoint with a badge
 * >= THREAD_FAULT_BASE (encoding proc_idx*MAX_THREADS_PER_PROC + tidx), distinct
 * from client request badges (1..MAX_ACTIVE_PROCS) and seL4 fault labels. */
#define THREAD_FAULT_BASE  0x10000

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
#define PIPE_DUP_REFS   82
#define PIPE_MMAP_ANON  83  /* v0.4.104: alloc anonymous pages, map in caller vspace */
#define PIPE_MUNMAP_ANON 84 /* v0.4.104: unmap pages allocated via PIPE_MMAP_ANON */
#define PIPE_MPROTECT   85  /* v0.4.126: change rights via seL4_ARM_Page_Map remap */
#define PIPE_MMAP_FILE  86  /* v0.4.145: alloc + vfs_pread-fill file pages in caller vspace */
#define PIPE_MSYNC      87  /* v0.4.145: write file-backed mmap pages back via vfs_pwrite */
#define PIPE_MUNMAP_FILE 88 /* v0.4.146: free demand-paged file mmap + reservation + descriptor */
#define PIPE_GET_TIME    89 /* v0.4.166: read the wall-clock epoch offset (seconds) */
#define PIPE_SET_TIME    90 /* v0.4.166: set the wall-clock epoch offset (e.g. from SNTP) */
/* v0.4.258 SHM-ring pipes. These are pipe_ep-only; their numeric values overlap the
 * NET_* labels (net_ep) harmlessly, exactly like PIPE_SET_TIME(90) == NET_SOCKET(90). */
#define PIPE_MAP_RING    91 /* map the SPSC SHM-ring frame into the caller (MR1: dir 0=read, 1=write) */
#define PIPE_RING_WAKE   92 /* writer asks the server to wake a reader parked on an empty ring */

/* ---- NET IPC labels (90-109) -- canonical defs in aios/net_proto.h.
 * v0.4.229 (netd Stage 0): factored out so the in-root net server and the
 * future netd process share one definition; retired the unused NET_GETINFO
 * that collided with NET_CLEANUP_PID at 98. ---- */
#include "aios/net_proto.h"

/* ---- DISPLAY IPC labels (110-129) ---- */
#define DISP_FB_INFO     110
#define DISP_SHOW_FILE   111
#define DISP_TEXT        112
#define DISP_FILL_RECT   113
#define DISP_CLEAR       114
#define DISP_CUBE        115   /* software 3D: spinning wireframe cube demo */
#define DISP_CONSOLE     116   /* mirror tty output to the HDMI fb_console (len in MR0, chars MR1+) */
#define DISP_V3D_CLEAR   118   /* Phase 2: GPU clear to the live FB (MR0=FB-order color) + pixel probe; MR0=status */
#define DISP_V3D_TRI     119   /* Phase 3: GPU rainbow triangle to the live FB + probe; MR0=status */
#define DISP_V3D_CUBE    120   /* Phase 4a: GPU spinning cube (MR0=frames); MR0=status */
#define DISP_V3D_RELEASE 121   /* resume the console after a GPU op (un-suspend + clear) */

/* ── Limits ── */

/* Concurrent-process limits. v0.4.181 demand-pages the read-only ELF text
 * (pipe_server.c setup_demand_text): a process keeps resident only the code it
 * actually executes, not the whole statically-linked binary -- which lifts the
 * VKA/allocman wall and lets the table go to 64 (~30 parallel pipelines, up from
 * 16->48->22). On RPi4 (4 GB) the TABLE, not RAM, is the limit; on QEMU (458 MB,
 * old wall ~80 procs) the wall is now above 64. The next lever for going much
 * higher is .text SHARING across same-binary procs (one copy for N) -- BACKLOG
 * "harden under load" Phase 2. */
#define MAX_ACTIVE_PROCS     64
#define MAX_THREADS_PER_PROC 8
#define THREAD_STACK_PAGES   4
#define MAX_ELF_SEGS         6
#define MAX_PIPES            64   /* each pipeline consumes a pipe */
#define PIPE_BUF_SIZE        4096
#define MAX_WAIT_PENDING     24
#define MAX_ZOMBIES          64
#define MAX_EXEC_ARGS        12
#define MAX_PIPE_READ_BLOCKED 4

/* v0.4.110: COW fork -- per-process tracking of copy-on-write ranges. */
#define MAX_COW_RANGES       4

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* ── Types ── */

typedef struct {
    int active;
    int tid;
    vka_object_t tcb;
    /* v0.4.191: the thread faults onto the shared thread_server endpoint via a
     * badged mint (badge = THREAD_FAULT_BASE + proc_idx*MAX_THREADS_PER_PROC +
     * tidx), so the server multiplexes thread-exit faults with client requests
     * in one event loop instead of blocking on a per-thread endpoint. */
    cspacepath_t fault_mint;
    vka_object_t ipc_frame;
    vka_object_t stack_frames[THREAD_STACK_PAGES];
    int exited;            /* thread faulted/exited; caps freed; retval captured */
    seL4_Word retval;      /* x0 at exit (pthread return value) */
    int join_pending;      /* a joiner's reply cap is saved in join_reply */
    cspacepath_t join_reply;
} aios_thread_t;

typedef struct {
    uintptr_t vaddr;
    size_t    memsz;
    size_t    filesz;  /* v0.4.110: needed to bound COW range to data portion */
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
    int ignore_next_fault;  /* v0.4.84: suppress TCB fault during exec */
    int fault_on_pipe_ep;
    int stdout_pipe_id;
    int stdin_pipe_id;
    uint32_t sig_pending;        /* pending signal bitmask */
    int exit_cb_ran;             /* v0.4.87: aios_exit_cb already closed pipes */
    int pipe_write_closed;       /* v0.4.87: PIPE_CLOSE_WRITE already called */
    /* v0.4.106: demand-paged BSS range. Pages in [start, end) are reserved
     * but unmapped at process load. On VM fault inside the range, the
     * fault handler in exec_thread allocates a frame and maps it. */
    uintptr_t bss_lazy_start;    /* page-aligned, inclusive */
    uintptr_t bss_lazy_end;      /* page-aligned, exclusive */
    void *bss_reservation;       /* opaque reservation_t.res for fault map */
    /* v0.4.109: count of pages allocated to this process via fault handler
     * (and any other vka_audit_frame calls per-process). Used to decrement
     * vka_live_frames in cleanup paths so /proc/vka shows accurate counts. */
    int audit_pages_allocated;
    /* v0.4.110: COW fork -- ranges (writable LOAD segments) where pages
     * are R/O dups of parent frames. On write fault inside a range the
     * fault handler allocates a fresh frame, copies, and remaps R/W. */
    uintptr_t cow_starts[MAX_COW_RANGES];   /* page-aligned, inclusive */
    uintptr_t cow_ends[MAX_COW_RANGES];     /* page-aligned, exclusive */
    void     *cow_reservations[MAX_COW_RANGES]; /* opaque reservation_t.res; NULL means parent-side range (no vspace tracking entry) */
    int       num_cow_ranges;
    /* v0.4.124 Step 3 promoted-frame tracking lives in cow.c globals to
     * avoid shifting active_proc_t's layout (which would amplify the
     * baseline BSS-fault noise). */
} active_proc_t;

typedef struct {
    int active;
    char buf[PIPE_BUF_SIZE];      /* fallback static buffer */
    char *shm_buf;                /* v0.4.65: mapped shared frame (or buf) */
    vka_object_t shm_frame;       /* v0.4.65: backing frame object */
    int shm_valid;                /* v0.4.65: 1 if shm_frame allocated */
    char *xfer_buf;               /* v0.4.66: transfer page mapped in root (READ dir) */
    vka_object_t xfer_frame;      /* v0.4.66: transfer frame object */
    int xfer_valid;               /* v0.4.66: 1 if xfer_frame allocated */
    seL4_CPtr xfer_copies[2];    /* v0.4.67: cap copies for child mappings */
    int xfer_copy_count;          /* v0.4.67: number of active copies */
    /* v0.4.257 coalescing: a SEPARATE write-direction xfer page (writer->server) so a
     * writer and reader (different procs) never race on one shared page. */
    char *xfer_buf_w;
    vka_object_t xfer_frame_w;
    int xfer_valid_w;
    seL4_CPtr xfer_copies_w[2];
    int xfer_copy_count_w;
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
    int caller_pid;               /* v0.4.87: PID of blocked reader (signal wakeup) */
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

/* Log drive */
extern ext2_ctx_t ext2_log;

extern volatile uint32_t *uart;

/* Network state */
extern uint8_t net_mac[6];
extern int net_available;        /* network is up + serving (published on netd DEVD_READY) */
/* netd Stage 3 (DESIGN_NETD s8): split the flag. net_hw_present = a NIC was
 * provisioned (set by plat_net_prov / plat_net_init); it gates the netd spawn and
 * the boot banner. net_available + net_ep_cap are published only once the net
 * stack is actually serving, so a child spawned before READY gets -ENOTSUP. */
extern int net_hw_present;
extern seL4_CPtr net_ep_cap;
extern seL4_CPtr net_drv_ntfn_cap;   /* RX IRQ ntfn; bound to the net_server TCB (v0.4.230) */
extern seL4_CPtr net_kick_ntfn_cap;  /* badge=2 kick copy of net_drv_ntfn (GENET .irqoff) */

struct net_rx_ring;
extern struct net_rx_ring net_rx_ring;

/* netd Stage 3 (DESIGN_NETD s6): root mapping of the netd /proc/net stats page
 * (the fs thread renders /proc/net + serverstats reads the SRV_NET heartbeat
 * IPC-free). NULL unless netd spawned + mapped it. */
struct netd_stats;
extern struct netd_stats *netd_stats_root;

/* Display state */
extern uint32_t *gpu_fb;
extern uint64_t gpu_fb_pa;
extern int gpu_available;
extern uint32_t gpu_width;
extern uint32_t gpu_height;
extern seL4_CPtr disp_ep_cap;
/* Badged notification bound to the display_server TCB: the fs-thread /proc/v3d.test
 * verb seL4_Signals it (NOT a Call -- non-MCS reply-cap safety) to wake display_server
 * so it runs the pending GPU clear. 0 until the display thread is started. */
extern seL4_CPtr disp_wake_ntfn_cap;

/* The framebuffer is mapped CACHEABLE (writes are ~10x faster than non-cacheable,
 * which matters for the scrolling HDMI console). After writing gpu_fb, the dirty
 * region MUST be cleaned to the point of coherency so the display controller (which
 * reads physical memory) sees the update -- these do that. gpu_fb_flush cleans a byte
 * range; gpu_fb_flush_all cleans the whole framebuffer. */
void gpu_fb_flush(void *start, uint32_t bytes);
void gpu_fb_flush_all(void);
/* Clean+invalidate one 4 KB FB page (page_idx from gpu_fb base) for the V3D pixel
 * probe -- forces a CPU read to fetch GPU-written pixels from RAM. */
void gpu_fb_invalidate_page(uint32_t page_idx);

/* Crypto server endpoint (crypto_server thread) */
extern seL4_CPtr crypto_ep_cap;

extern volatile int fg_pid;
extern volatile seL4_CPtr fg_fault_ep;
extern volatile int fg_killed;
extern volatile int fg_sigint_sent;

extern wait_pending_t wait_pending[MAX_WAIT_PENDING];
extern vka_object_t wait_reply_objects[MAX_WAIT_PENDING];
extern zombie_t zombies[MAX_ZOMBIES];
extern int wait_pending_init;

/* ── Cross-module function declarations ── */
/* Add declarations here as functions are extracted from aios_root.c */

/* Boot phases (src/boot/)
 *
 * v0.4.102: boot_fs_init returns status for graceful degradation:
 *   BOOT_FS_OK       -- normal boot, / mounted from disk
 *   BOOT_FS_DEGRADED -- only /proc available (no disk fs); recovery mode
 *   BOOT_FS_FATAL    -- block device init failed entirely
 */
#define BOOT_FS_OK        0
#define BOOT_FS_DEGRADED  1
#define BOOT_FS_FATAL     2

extern int aios_boot_status;

int  boot_fs_init(void);
void boot_start_services(vka_object_t *fault_ep);

/* Block I/O via platform HAL (src/plat/blk_hal.h) */

/* Spawn utilities (src/boot/spawn_util.c) */
int spawn_with_args(const char *name, uint8_t prio,
                    sel4utils_process_t *proc,
                    vka_object_t *fault_ep,
                    int ep_count, seL4_CPtr *eps,
                    seL4_CPtr *child_slots);
int spawn_simple(const char *name, uint8_t prio,
                 sel4utils_process_t *proc,
                 vka_object_t *fault_ep);

/* v0.4.257 Stage S: assign a freshly-spawned USER process to a core (round-robin 1..N-1;
 * /proc/coresched toggles it). Root servers stay on core 0 -- do NOT call this for them. */
void aios_assign_core(seL4_CPtr tcb);
int  coresched_cmd(const char *args, char *buf, int bufsize);
extern volatile int g_proc_distribute;

/* v0.4.258 SHM-ring pipes: direct SPSC producer<->consumer ring so a pipeline can
 * span cores (the pipe_server is out of the per-chunk data path). Decided per-pipe at
 * PIPE_CREATE; ships DEFAULT OFF (the cross-core coherency path needs real-HW soak).
 * /proc/shmring.1 arms it for pipes created afterwards, .0 disarms. */
extern volatile int g_shm_ring;
int  shmring_cmd(const char *args, char *buf, int bufsize);

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
/* v0.4.229 (netd Stage 0): sacrificial net-socket-cleanup proxy thread + the
 * setter boot uses to hand it (and handle_child_fault) its wakeup notification.
 * Decouples the blocking NET_CLEANUP_PID Call from the pipe_server main loop so
 * a slow/wedged net server parks only the proxy, not all process management. */
void net_cleanup_proxy_fn(void *arg0, void *arg1, void *ipc_buf);
void pipe_set_net_cleanup_ntfn(seL4_CPtr ntfn);
void boot_net_init(void);
#ifdef AIOS_NETD
/* netd de-monolithization (DESIGN_NETD Stage 3). netd_prov() provisions the NIC
 * root-side (boot_net_init, before the banner); spawn_netd() spawns the isolated
 * netd process and hands it net_ep + the device caps. Both no-ops to nothing when
 * AIOS_NETD is OFF (the whole spawn_netd.c is compiled out). */
int  netd_prov(void);
void spawn_netd(seL4_CPtr net_ep);
#endif
void boot_display_init(void);
/* v0.4.230: the net driver thread merged into net_server, which drains the HW
 * RX ring via plat_net_drain() (plat/net_hal.h) at its loop top. */
void net_server_fn(void *arg0, void *arg1, void *ipc_buf);
void display_server_fn(void *arg0, void *arg1, void *ipc_buf);
void crypto_server_main(void *arg0, void *arg1, void *ipc_buf);

void boot_load_config(void);
void aios_system_shutdown(void);
void aios_system_reboot(void);   /* v0.4.163: BCM2711 watchdog reset (RPi4) */

/* v0.4.166: wall-clock epoch offset (seconds) so wall_time = uptime + offset.
 * 0 until set (e.g. by the SNTP client via PIPE_SET_TIME). aios_wall_now()
 * returns current wall-clock seconds for in-root-task users (e.g. fs mtimes). */
extern long aios_wall_offset_sec;
long aios_wall_now(void);

/* v0.4.121: server health probe (src/servers/serverstats.c) */
void serverstats_init(void);
int  serverstats_format(char *buf, int bufsize);

/* v0.4.188: periodic write-back flusher (src/servers/flush_server.c) */
void flush_server_init(void);
int  flush_server_format(char *buf, int bufsize);

/* session-11: BCM2711 system-timer service for BLOCKING timed waits (the stall cure).
 * A dedicated core-0 server thread binds the timer compare-1 IRQ (GIC SPI INTID 97) to
 * its TCB and parks sleepers' reply caps (seL4_CNode_SaveCaller) in a deadline queue,
 * replying at the deadline -- so a "sleep" BLOCKS instead of seL4_Yield-spinning, letting
 * core 0 idle and stop evicting blocked threads' resume lines. RPi4-only: arms only when
 * the system-timer MMIO + IRQ bind succeed (NULL/absent on qemu-arm-virt), else clients
 * fall back to the old yield-spin via aios_timer_sleep_us(). src/servers/timer_server.c. */
#define TIMER_SLEEP   1            /* request label on timer_ep_cap; MR0 = relative delay (microseconds) */
extern seL4_CPtr     timer_ep_cap; /* request endpoint (root threads Call directly; 0 until armed) */
extern volatile int  aios_timer_ready; /* 1 once the system timer + IRQ are armed (else yield-fallback) */
void timer_server_init(void);
void aios_timer_sleep_us(uint64_t us); /* block (or yield-fallback) for us microseconds */
int  timer_server_format(char *buf, int bufsize);
/* /proc/timersleep[.0|.1] -- gate whether NEWLY-spawned user procs receive the timer cap
 * (so their nanosleep BLOCKS vs yield-spins). DEFAULT 0: flash, validate the Phase-1 canary
 * (/proc/timer ready=1), THEN .1 to activate Phase-2 + netstall-A/B it on the SAME image. */
extern volatile int g_timer_userspace_enable;
int timersleep_cmd(const char *args, char *buf, int bufsize);

#endif /* AIOS_ROOT_SHARED_H */
