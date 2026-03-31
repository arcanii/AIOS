/* AIOS Orchestrator — synchronous rewrite
 * All filesystem operations use microkit_ppcall (blocking).
 * State machine only needed for: LLM generation (async), sandbox exec (async).
 */
#include <microkit.h>
#include "aios/ipc.h"
#include "aios/channels.h"
#include "aios/auth.h"

#define SBX_ENTRY_POINT 0x200000  /* sandbox.elf entry point */
#include "sys/syscall.h"
#include "aios/version.h"
#include <kernel/gen_config.h>
#include "aios/ring.h"



/* ── Shared memory regions (set by Microkit loader) ─── */
uintptr_t tx_buf;
uintptr_t rx_buf;
uintptr_t sock_data;
uintptr_t fs_data;
uintptr_t model_data;
uintptr_t llm_io;
uintptr_t echo_io;
uintptr_t auth_io;
/* Per-sandbox memory regions (set by Microkit loader via setvar) */
uintptr_t sbx0_io;
uintptr_t sbx0_code;
uintptr_t sbx0_heap;
uintptr_t sbx1_io;
uintptr_t sbx1_code;
uintptr_t sbx1_heap;
uintptr_t sbx2_io;
uintptr_t sbx2_code;
uintptr_t sbx2_heap;
uintptr_t sbx3_io;
uintptr_t sbx3_code;
uintptr_t sbx3_heap;
uintptr_t sbx4_io;
uintptr_t sbx4_code;
uintptr_t sbx4_heap;
uintptr_t sbx5_io;
uintptr_t sbx5_code;
uintptr_t sbx5_heap;
uintptr_t sbx6_io;
uintptr_t sbx6_code;
uintptr_t sbx6_heap;
uintptr_t sbx7_io;
uintptr_t sbx7_code;
uintptr_t sbx7_heap;

/* Process swap region (256 MiB) */
uintptr_t swap_region;

/* Runtime arrays initialized in init() */
static uintptr_t sbx_io[NUM_SANDBOXES];
static uintptr_t sbx_code[NUM_SANDBOXES];
static uintptr_t sbx_heap[NUM_SANDBOXES];

/* Process table */
/* Process scheduler (replaces old proctab[8]) */
#include "proc_sched.h"
static scheduler_t sched;

/* Helper: get process for a sandbox slot */
static inline sched_proc_t *slot_proc(int slot) {
    int idx = sched.slot_to_proc[slot];
    if (idx >= 0) return &sched.procs[idx];
    return (sched_proc_t *)0;
}

/* Helper: check if slot has a running process */
static inline int slot_in_use(int slot) {
    return sched.slot_to_proc[slot] >= 0;
}

static void try_load_queued(int freed_slot);


/* Slot-to-channel mapping (not contiguous due to other PD channels) */
static const int sbx_channel[NUM_SANDBOXES] = {
    CH_SBX0, CH_SBX1, CH_SBX2, CH_SBX3,
    CH_SBX4, CH_SBX5, CH_SBX6, CH_SBX7,
};

static int find_free_slot(void) {
    return sched_find_free_slot(&sched);
}

static int ch_to_slot(microkit_channel ch) {
    int s = -1;
    for (int i = 0; i < NUM_SANDBOXES; i++) {
        if (sbx_channel[i] == (int)ch) { s = i; break; }
    }
    if (s >= 0 && s < NUM_SANDBOXES) return s;
    return -1;
}

/* ── Memory helpers ──────────────────────────────────── */

/* ── Memory helpers ── */
static __attribute__((unused)) void my_memcpy(void *dst, const void *src, unsigned long n) {
    char *d = dst; const char *s = src;
    while (n--) *d++ = *s++;
}
static __attribute__((unused)) void my_memset(void *dst, int c, unsigned long n) {

/* Provide memset for microkit_pd_restart (seL4_UserContext = {0}) */

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


#include "orch/orch_serial.inc"

/* ── Logging backend ─────────────────────────────────── */
#define LOG_MODULE "ORCH"
#define LOG_LEVEL  LOG_LEVEL_WARN
#include "aios/log.h"

void _log_puts(const char *s) { ser_puts(s); }
void _log_put_dec(unsigned long n) { ser_put_dec(n); }
void _log_flush(void) { ser_flush(); }
unsigned long _log_get_time(void) {
    uint64_t cnt, freq;
    asm volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    return (unsigned long)(cnt / freq);
}



#include "orch/orch_fs.inc"

/* ── Input buffer and state ── */
/* ── Input buffer ────────────────────────────────────── */
#define INPUT_MAX 256
static char input_line[INPUT_MAX];
static int  input_pos = 0;

/* ── Orchestrator state (minimal — only for async ops) ─ */

/* ── Auth state machine ────────────────────────────── */
enum {
    AUTH_LOGIN_USER,    /* waiting for username */
    AUTH_LOGIN_PASS,    /* waiting for password */
    AUTH_AUTHENTICATED  /* logged in */
};
static int auth_state = AUTH_LOGIN_USER;
static uint32_t current_session = 0;
static uint32_t current_uid = 0;
static uint32_t current_gid = 0;
static char hostname[64] = "aios";
static char current_shell[64] = "/bin/osh";
static char current_home[64] = "/root";
static int session_shell_slot = -1;  /* slot running login shell, -1 if none */


#include "orch/orch_pipe.inc"
#include "orch/orch_net.inc"

/* ── Execution and model state ── */
/* ── Model loading state ─────────────────────────────── */
static uint32_t llm_model_loaded = 0;
static uint32_t saved_tok_offset = 0;
static uint32_t saved_tok_size = 0;

/* ── Exec state ──────────────────────────────────────── */
static uint32_t exec_loaded_bytes = 0;
static char cwd[256] = "/";
static char exec_args[256];

/* Resolve a relative path against cwd into an absolute path */
static void resolve_path(const char *name, char *out, int out_size) {
    if (name[0] == '/') {
        /* Already absolute */
        int i = 0;
        while (name[i] && i < out_size - 1) { out[i] = name[i]; i++; }
        out[i] = '\0';
    } else {
        /* Prepend cwd */
        int j = 0, k = 0;
        while (cwd[k] && j < out_size - 2) out[j++] = cwd[k++];
        if (j > 1) out[j++] = '/';
        k = 0;
        while (name[k] && j < out_size - 1) out[j++] = name[k++];
        out[j] = '\0';
    }
}


/* ── Process commands ── */
/* ── Command: kill ────────────────────────────────────── */
static int auth_check_kill_sync(uint32_t target_uid);
static int auth_check_file_sync(const char *path, uint32_t mode);
static void cmd_kill(const char *arg) {
    /* Parse PID */
    unsigned int pid = 0;
    int i = 0;
    while (arg[i] >= '0' && arg[i] <= '9') {
        pid = pid * 10 + (arg[i] - '0');
        i++;
    }
    if (pid == 0) {
        ser_puts("  Usage: kill <pid>\n");
        return;
    }
    /* Find slot with matching PID */
    for (int s = 0; s < NUM_SANDBOXES; s++) {
        if (slot_in_use(s) && slot_proc(s)->pid == pid) {
            /* Permission check: owner or root */
            if (auth_check_kill_sync(slot_proc(s)->owner_uid) != 0) {
                ser_puts("  Permission denied: not owner\n");
                ser_flush();
                return;
            }
            microkit_pd_stop(s);
            microkit_pd_restart(s, SBX_ENTRY_POINT);
            ser_puts("  Killed PID ");
            ser_put_dec(pid);
            ser_puts(" (");
            ser_puts(slot_proc(s)->name);
            ser_puts(")\n");
            if (slot_proc(s)->foreground) {
                orch_state = RUNNING;
            }
            sched_release_slot(&sched, s);
            return;
        }
    }
    ser_puts("  No process with PID ");
    ser_put_dec(pid);
    ser_puts("\n");
}

/* ── Command: ps ─────────────────────────────────────── */
static void cmd_ps(void) {
    ser_puts("  SLOT  PID  NAME\n");
    int any = 0;
    for (int i = 0; i < NUM_SANDBOXES; i++) {
        if (slot_in_use(i)) {
            ser_puts("  ");
            ser_put_dec((unsigned int)i);
            ser_puts("     ");
            ser_put_dec(slot_proc(i)->pid);
            ser_puts("  ");
            ser_puts(slot_proc(i)->name);
            if (slot_proc(i)->foreground) ser_puts(" (fg)");
            else ser_puts(" (bg)");
            ser_puts("\n");
            any = 1;
        }
    }
    if (!any) ser_puts("  (no running processes)\n");
}

/* ── Command: help ───────────────────────────────────── */


#include "orch/orch_auth_cmd.inc"
#include "orch/orch_fs_cmd.inc"
#include "orch/orch_exec.inc"
#include "orch/orch_llm.inc"
#include "orch/orch_dispatch.inc"
#include "orch/orch_input.inc"
#include "orch/orch_syscall.inc"

#include "orch/orch_log.inc"
#include "orch/orch_main.inc"

/* ── Queue drain: load next queued process into a freed slot ── */
static void try_load_queued(int freed_slot) {
    int next_idx = sched_drain_queue(&sched);
    if (next_idx < 0) return;  /* Nothing queued or ready */

    sched_proc_t *p = &sched.procs[next_idx];

    uintptr_t cur_sio  = sbx_io[freed_slot];
    uintptr_t cur_scode = sbx_code[freed_slot];

    if (p->state == PROC_READY) {
        /* ── Resume from swap ─────────────────────────── */

        uint32_t swap_off = p->swap_offset;
        volatile uint8_t *swap_base = (volatile uint8_t *)swap_region;

        /* Copy code from swap back to sandbox */
        volatile uint8_t *dst_code = (volatile uint8_t *)cur_scode;
        for (uint32_t j = 0; j < p->swap_code_sz; j++)
            dst_code[j] = swap_base[swap_off + j];

        /* Copy heap from swap back to sandbox */
        volatile uint8_t *dst_heap = (volatile uint8_t *)sbx_heap[freed_slot];
        uint32_t heap_swap_off = swap_off + p->swap_code_sz;
        for (uint32_t j = 0; j < p->swap_heap_sz; j++)
            dst_heap[j] = swap_base[heap_swap_off + j];

        /* Free swap space */
        sched_swap_free(&sched, next_idx);

        /* Assign slot and tell sandbox to resume */
        sched_assign_slot(&sched, next_idx, freed_slot);
        WR32(cur_sio, SBX_CODE_SIZE, p->loaded_bytes);
        WR32(cur_sio, SBX_CMD, SBX_CMD_RESUME);

        /* Restart the sandbox PD (it may be in a halted/exited state) */
        microkit_pd_stop(freed_slot);
        microkit_pd_restart(freed_slot, SBX_ENTRY_POINT);

        /* After restart, sandbox runs init() then waits for notification */
        microkit_notify(sbx_channel[freed_slot]);
        return;
    }

    /* ── Load from disk (PROC_QUEUED) ─────────────── */
    int st = fs_open_sync(p->filename);
    if (st < 0) {
        /* Try /bin/ prefix */
        char bin_path[128];
        bin_path[0]='/'; bin_path[1]='b'; bin_path[2]='i';
        bin_path[3]='n'; bin_path[4]='/';
        int j = 5;
        for (int k = 0; p->filename[k] && j < 120; k++) bin_path[j++] = p->filename[k];
        bin_path[j] = '\0';
        st = fs_open_sync(bin_path);
    }
    if (st < 0) {
        ser_puts("SCHED: failed to load queued process: ");
        ser_puts(p->filename);
        ser_puts("\n");
        p->state = PROC_FREE;
        return;
    }

    int fd = st;
    volatile uint8_t *code_dst = (volatile uint8_t *)cur_scode;

    uint32_t offset = 0;
    while (1) {
        uint32_t chunk = 0x400000 - offset;
        if (chunk > 65024) chunk = 65024;
        st = fs_read_sync(fd, offset, chunk);
        if (st < 0) break;
        uint32_t got = RD32(fs_data, 0);
        if (got == 0) break;
        volatile uint8_t *src = (volatile uint8_t *)(fs_data + 4);
        for (uint32_t j = 0; j < got; j++) code_dst[offset + j] = src[j];
        offset += got;
        if (got < chunk) break;
    }
    fs_close_sync(fd);

    if (offset == 0) {
        ser_puts("SCHED: empty binary: ");
        ser_puts(p->filename);
        ser_puts("\n");
        p->state = PROC_FREE;
        return;
    }

    p->loaded_bytes = offset;
    sched_assign_slot(&sched, next_idx, freed_slot);

    WR32(cur_sio, SBX_CODE_SIZE, offset);
    WR32(cur_sio, SBX_CMD, SBX_CMD_RUN);
    microkit_notify(sbx_channel[freed_slot]);
}


