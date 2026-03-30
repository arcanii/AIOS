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
uintptr_t auth_io;
/* Per-sandbox memory regions (set by Microkit loader via setvar) */
uintptr_t sbx0_io;
uintptr_t sbx0_code;
uintptr_t sbx1_io;
uintptr_t sbx1_code;
uintptr_t sbx2_io;
uintptr_t sbx2_code;
uintptr_t sbx3_io;
uintptr_t sbx3_code;
uintptr_t sbx4_io;
uintptr_t sbx4_code;
uintptr_t sbx5_io;
uintptr_t sbx5_code;
uintptr_t sbx6_io;
uintptr_t sbx6_code;
uintptr_t sbx7_io;
uintptr_t sbx7_code;

/* Runtime arrays initialized in init() */
static uintptr_t sbx_io[NUM_SANDBOXES];
static uintptr_t sbx_code[NUM_SANDBOXES];

/* Process table */
typedef struct {
    int in_use;
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t loaded_bytes;
    int foreground;
    uint32_t owner_uid;
    char name[64];
} process_t;
static process_t proctab[NUM_SANDBOXES];
static uint32_t next_pid = 1;


/* Slot-to-channel mapping (not contiguous due to other PD channels) */
static const int sbx_channel[NUM_SANDBOXES] = {
    CH_SBX0, CH_SBX1, CH_SBX2, CH_SBX3,
    CH_SBX4, CH_SBX5, CH_SBX6, CH_SBX7,
};

static int find_free_slot(void) {
    for (int i = 0; i < NUM_SANDBOXES; i++)
        if (!proctab[i].in_use) return i;
    return -1;
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
#define LOG_LEVEL  LOG_LEVEL_INFO
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
        if (proctab[s].in_use && proctab[s].pid == pid) {
            /* Permission check: owner or root */
            if (auth_check_kill_sync(proctab[s].owner_uid) != 0) {
                ser_puts("  Permission denied: not owner\n");
                ser_flush();
                return;
            }
            microkit_pd_stop(s);
            microkit_pd_restart(s, SBX_ENTRY_POINT);
            ser_puts("  Killed PID ");
            ser_put_dec(pid);
            ser_puts(" (");
            ser_puts(proctab[s].name);
            ser_puts(")\n");
            if (proctab[s].foreground) {
                orch_state = RUNNING;
            }
            proctab[s].in_use = 0;
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
        if (proctab[i].in_use) {
            ser_puts("  ");
            ser_put_dec((unsigned int)i);
            ser_puts("     ");
            ser_put_dec(proctab[i].pid);
            ser_puts("  ");
            ser_puts(proctab[i].name);
            if (proctab[i].foreground) ser_puts(" (fg)");
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

#include "orch/orch_main.inc"
