#ifndef AIOS_PROC_SCHED_H
#define AIOS_PROC_SCHED_H

/*
 * AIOS Preemptive Process Scheduler
 *
 * Manages N logical processes across 8 physical sandbox slots.
 * The orchestrator acts as the scheduler, using:
 *   - microkit_pd_stop(slot)    to freeze a sandbox
 *   - microkit_pd_restart(slot) to resume a sandbox
 *   - memcpy to swap code+heap between sandbox and swap region
 *
 * Process states:
 *   FREE    - slot available
 *   QUEUED  - waiting to run, binary path stored
 *   READY   - suspended, full state in swap region
 *   RUNNING - active in a physical sandbox slot
 *   BLOCKED - waiting for I/O (still in sandbox slot)
 *   ZOMBIE  - exited, waiting for parent to reap
 */

#include <stdint.h>

#define SCHED_MAX_PROCS    256
#define SCHED_MAX_SLOTS    8
#define SCHED_CODE_SIZE    (4 * 1024 * 1024)   /* 4 MiB per sandbox */
#define SCHED_HEAP_SIZE    (16 * 1024 * 1024)   /* 16 MiB per sandbox */
#define SCHED_SWAP_SIZE    (256 * 1024 * 1024)  /* 256 MiB swap region */

/* Per-process swap: code(4M) + heap(16M) = 20 MiB max
 * But most processes use much less. Use a simple allocator. */
#define SCHED_SWAP_SLOT_SIZE  (SCHED_CODE_SIZE + SCHED_HEAP_SIZE)  /* 20 MiB worst case */

/* With lazy swap: only save actual used code + dirty heap */

enum proc_state {
    PROC_FREE = 0,
    PROC_QUEUED,    /* Binary path known, not yet loaded */
    PROC_READY,     /* State saved in swap, can be resumed */
    PROC_RUNNING,   /* Active in a sandbox slot */
    PROC_BLOCKED,   /* In sandbox slot, waiting for I/O */
    PROC_ZOMBIE     /* Exited, awaiting reap */
};

typedef struct {
    enum proc_state state;
    int pid;
    int parent_pid;
    int owner_uid;
    int exit_code;
    int slot;               /* Physical sandbox slot (-1 if not resident) */
    int foreground;
    uint32_t priority;      /* 0=highest, 255=lowest */

    /* Binary info (for QUEUED state - load from disk) */
    char filename[64];
    char args[256];
    char name[64];
    uint32_t loaded_bytes;  /* Size of code segment */

    /* Swap info (for READY state - saved in swap region) */
    uint32_t swap_offset;   /* Offset into swap region */
    uint32_t swap_code_sz;  /* Bytes of code saved */
    uint32_t swap_heap_sz;  /* Bytes of heap saved (only used portion) */

    /* Scheduling stats */
    uint32_t time_slices;   /* Number of times scheduled */
    uint32_t total_ticks;   /* Total CPU time consumed */
    uint32_t last_scheduled;/* Tick count when last scheduled */
} sched_proc_t;

typedef struct {
    sched_proc_t procs[SCHED_MAX_PROCS];
    int next_pid;
    int slot_to_proc[SCHED_MAX_SLOTS];  /* slot -> proc index, -1 if empty */
    uint32_t tick_count;                /* Global tick counter */
    uint32_t swap_bump;                 /* Next free byte in swap region */
} scheduler_t;

/* ── Initialization ─────────────────────────────────── */

static inline void sched_init(scheduler_t *s) {
    for (int i = 0; i < SCHED_MAX_PROCS; i++) {
        s->procs[i].state = PROC_FREE;
        s->procs[i].pid = 0;
        s->procs[i].slot = -1;
        s->procs[i].swap_offset = 0;
        s->procs[i].swap_code_sz = 0;
        s->procs[i].swap_heap_sz = 0;
        s->procs[i].time_slices = 0;
        s->procs[i].total_ticks = 0;
    }
    for (int i = 0; i < SCHED_MAX_SLOTS; i++)
        s->slot_to_proc[i] = -1;
    s->next_pid = 1;
    s->tick_count = 0;
    s->swap_bump = 0;
}

/* ── Process creation ───────────────────────────────── */

static inline int sched_alloc(scheduler_t *s) {
    for (int i = 0; i < SCHED_MAX_PROCS; i++)
        if (s->procs[i].state == PROC_FREE) return i;
    return -1;
}

static inline int sched_create(scheduler_t *s, const char *filename,
                                const char *args, int parent_pid,
                                int uid, int fg, uint32_t priority) {
    int idx = sched_alloc(s);
    if (idx < 0) return -1;  /* Process table full */

    sched_proc_t *p = &s->procs[idx];
    p->state = PROC_QUEUED;
    p->pid = s->next_pid++;
    p->parent_pid = parent_pid;
    p->owner_uid = uid;
    p->foreground = fg;
    p->priority = priority;
    p->slot = -1;
    p->exit_code = 0;
    p->loaded_bytes = 0;
    p->swap_offset = 0;
    p->swap_code_sz = 0;
    p->swap_heap_sz = 0;
    p->time_slices = 0;
    p->total_ticks = 0;
    p->last_scheduled = 0;

    /* Copy filename */
    int i = 0;
    while (filename[i] && i < 63) { p->filename[i] = filename[i]; i++; }
    p->filename[i] = '\0';

    /* Extract base name */
    const char *base = filename;
    for (const char *c = filename; *c; c++)
        if (*c == '/') base = c + 1;
    i = 0;
    while (base[i] && i < 31) { p->name[i] = base[i]; i++; }
    p->name[i] = '\0';

    /* Copy args */
    if (args) {
        i = 0;
        while (args[i] && i < 255) { p->args[i] = args[i]; i++; }
        p->args[i] = '\0';
    } else {
        p->args[0] = '\0';
    }

    return p->pid;
}

/* ── Slot management ────────────────────────────────── */

static inline int sched_find_free_slot(scheduler_t *s) {
    for (int i = 0; i < SCHED_MAX_SLOTS; i++)
        if (s->slot_to_proc[i] < 0) return i;
    return -1;
}

static inline void sched_assign_slot(scheduler_t *s, int proc_idx, int slot) {
    s->procs[proc_idx].slot = slot;
    s->procs[proc_idx].state = PROC_RUNNING;
    s->slot_to_proc[slot] = proc_idx;
    s->procs[proc_idx].time_slices++;
    s->procs[proc_idx].last_scheduled = s->tick_count;
}

static inline void sched_release_slot(scheduler_t *s, int slot) {
    int idx = s->slot_to_proc[slot];
    if (idx >= 0) {
        s->procs[idx].slot = -1;
        s->slot_to_proc[slot] = -1;
    }
}

/* ── Swap operations ────────────────────────────────── */
/* These return the functions to call - actual memcpy done by orchestrator
 * since it has access to sandbox memory regions */

/* Allocate swap space for a process */
static inline int sched_swap_alloc(scheduler_t *s, int proc_idx,
                                    uint32_t code_sz, uint32_t heap_sz) {
    uint32_t total = code_sz + heap_sz;
    if (s->swap_bump + total > SCHED_SWAP_SIZE)
        return -1;  /* Swap full */

    s->procs[proc_idx].swap_offset = s->swap_bump;
    s->procs[proc_idx].swap_code_sz = code_sz;
    s->procs[proc_idx].swap_heap_sz = heap_sz;
    s->swap_bump += total;

    /* Align to 4KB */
    s->swap_bump = (s->swap_bump + 0xFFF) & ~0xFFF;

    return 0;
}

/* Free swap space - for simplicity, use a bump allocator with compaction */
static inline void sched_swap_free(scheduler_t *s, int proc_idx) {
    s->procs[proc_idx].swap_offset = 0;
    s->procs[proc_idx].swap_code_sz = 0;
    s->procs[proc_idx].swap_heap_sz = 0;
    /* TODO: compaction when swap_bump gets too high */
}

/* ── Scheduling decisions ───────────────────────────── */

/* Find the best QUEUED or READY process to run next */
static inline int sched_pick_next(scheduler_t *s) {
    int best = -1;
    uint32_t best_prio = 256;
    uint32_t best_wait = 0;

    for (int i = 0; i < SCHED_MAX_PROCS; i++) {
        sched_proc_t *p = &s->procs[i];
        if (p->state != PROC_QUEUED && p->state != PROC_READY)
            continue;

        /* Foreground processes always get priority */
        uint32_t eff_prio = p->priority;
        if (p->foreground) eff_prio = 0;

        /* Among same priority, prefer longest waiting */
        uint32_t wait = s->tick_count - p->last_scheduled;

        if (eff_prio < best_prio ||
            (eff_prio == best_prio && wait > best_wait)) {
            best = i;
            best_prio = eff_prio;
            best_wait = wait;
        }
    }
    return best;
}

/* Find a running process to preempt (lowest priority, longest running) */
static inline int sched_pick_victim(scheduler_t *s) {
    int victim = -1;
    uint32_t worst_prio = 0;
    uint32_t longest_run = 0;

    for (int i = 0; i < SCHED_MAX_SLOTS; i++) {
        int idx = s->slot_to_proc[i];
        if (idx < 0) continue;

        sched_proc_t *p = &s->procs[idx];
        if (p->foreground) continue;  /* Don't preempt foreground */
        if (p->state == PROC_BLOCKED) continue;  /* Don't preempt blocked */

        uint32_t run_time = s->tick_count - p->last_scheduled;
        if (p->priority > worst_prio ||
            (p->priority == worst_prio && run_time > longest_run)) {
            victim = i;  /* Return slot number */
            worst_prio = p->priority;
            longest_run = run_time;
        }
    }
    return victim;
}

/* ── Process exit ───────────────────────────────────── */

static inline void sched_exit(scheduler_t *s, int slot, int exit_code) {
    int idx = s->slot_to_proc[slot];
    if (idx < 0) return;

    s->procs[idx].state = PROC_ZOMBIE;
    s->procs[idx].exit_code = exit_code;
    sched_swap_free(s, idx);
    sched_release_slot(s, slot);
}

static inline int sched_reap(scheduler_t *s, int pid, int *exit_code) {
    for (int i = 0; i < SCHED_MAX_PROCS; i++) {
        if (s->procs[i].pid == pid && s->procs[i].state == PROC_ZOMBIE) {
            if (exit_code) *exit_code = s->procs[i].exit_code;
            s->procs[i].state = PROC_FREE;
            s->procs[i].pid = 0;
            return 0;
        }
    }
    return -1;
}

/* ── Lookup helpers ─────────────────────────────────── */

static inline sched_proc_t *sched_find_pid(scheduler_t *s, int pid) {
    for (int i = 0; i < SCHED_MAX_PROCS; i++)
        if (s->procs[i].pid == pid && s->procs[i].state != PROC_FREE)
            return &s->procs[i];
    return (sched_proc_t *)0;
}

static inline sched_proc_t *sched_find_slot(scheduler_t *s, int slot) {
    int idx = s->slot_to_proc[slot];
    if (idx >= 0) return &s->procs[idx];
    return (sched_proc_t *)0;
}

/* ── Stats ──────────────────────────────────────────── */

static inline void sched_stats(scheduler_t *s, int *running, int *ready,
                                int *queued, int *blocked, int *zombie) {
    int r = 0, rd = 0, q = 0, b = 0, z = 0;
    for (int i = 0; i < SCHED_MAX_PROCS; i++) {
        switch (s->procs[i].state) {
            case PROC_RUNNING: r++;  break;
            case PROC_READY:   rd++; break;
            case PROC_QUEUED:  q++;  break;
            case PROC_BLOCKED: b++;  break;
            case PROC_ZOMBIE:  z++;  break;
            default: break;
        }
    }
    if (running) *running = r;
    if (ready)   *ready = rd;
    if (queued)  *queued = q;
    if (blocked) *blocked = b;
    if (zombie)  *zombie = z;
}


/* ── Queue drain: called when a slot is freed ───────── */
/* Returns the proc index to load next, or -1 if queue empty */
static inline int sched_drain_queue(scheduler_t *s) {
    return sched_pick_next(s);
}

/* ── Fork support ───────────────────────────────────── */
/* Create a child process as a copy of the parent in the given slot.
 * Returns child PID on success, -1 if process table full.
 * The caller must copy code+heap from parent slot to child slot. */
static inline int sched_fork(scheduler_t *s, int parent_slot) {
    sched_proc_t *parent = sched_find_slot(s, parent_slot);
    if (!parent) return -1;

    int child_idx = sched_alloc(s);
    if (child_idx < 0) return -1;

    sched_proc_t *child = &s->procs[child_idx];
    child->state = PROC_QUEUED;  /* Will be RUNNING once assigned a slot */
    child->pid = s->next_pid++;
    child->parent_pid = parent->pid;
    child->owner_uid = parent->owner_uid;
    child->foreground = 0;  /* Child runs in background */
    child->priority = parent->priority;
    child->loaded_bytes = parent->loaded_bytes;
    child->swap_offset = 0;
    child->swap_code_sz = 0;
    child->swap_heap_sz = 0;
    child->time_slices = 0;
    child->total_ticks = 0;
    child->last_scheduled = s->tick_count;

    /* Copy name */
    for (int i = 0; i < 64; i++) child->name[i] = parent->name[i];
    /* Copy filename */
    for (int i = 0; i < 64; i++) child->filename[i] = parent->filename[i];
    /* Copy args */
    for (int i = 0; i < 256; i++) child->args[i] = parent->args[i];

    return child->pid;
}

#endif /* AIOS_PROC_SCHED_H */
