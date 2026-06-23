/*
 * allocman_lock_host_test.c -- host repro of the libsel4allocman CSpace-slot
 * bitmap TEAR (the v0.4.178 "second SSH connection fails" SMP bug) and proof
 * that a lock around the allocator closes it.
 *
 * Models _cspace_single_level_alloc (deps/seL4_libs/libsel4allocman/src/cspace/
 * single_level.c:51-72): a lock-free read-modify-write on a free-bit bitmap
 * (1 = free, 0 = allocated). Two threads can read the same word, compute the
 * SAME free-bit index via CLZL, and both clear it -- handing out the SAME slot
 * cptr twice. On the real system the second writer then clobbers a saved reply
 * cap. The shared "last_entry" cursor tears the same way.
 *
 * Build + run:
 *   cc -O2 -pthread scripts/allocman_lock_host_test.c -o /tmp/altest && /tmp/altest
 *
 * Expect:
 *   MODE buggy  -> DUPLICATE slots detected  (reproduces the bug)
 *   MODE locked -> no duplicates, all unique  (proves the lock fixes it)
 * Exit 0 only if buggy reproduces the tear AND locked is clean (the lock is real
 * and load-bearing); otherwise FAIL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

#define BITS_PER_WORD ((int)(sizeof(size_t) * 8))
#define CLZL(x)       __builtin_clzl((unsigned long)(x))
#define BITW(n)       ((size_t)1 << (n))

/* mirrors the allocman single-level cspace, just the fields the RMW touches */
typedef struct {
    size_t *bitmap;        /* 1 = free, 0 = allocated */
    int     bitmap_length; /* number of words */
    size_t  last_entry;    /* shared scan cursor (also torn) */
    long    first_slot;
} cspace_t;

/* Widen the read->write window so the tear is reliable on the host. The real HW
 * window is just a couple of instructions, but two cores hit it constantly under
 * the v0.4.178 do_fork/SaveCaller storm; a yield here makes the same race
 * deterministic in a host test. */
static void widen(void) { sched_yield(); }

/* The buggy RMW, faithful to single_level.c:56-71 (no lock). Returns a slot, or
 * -1 if full. */
static long raw_alloc(cspace_t *cs)
{
    size_t i = cs->last_entry;
    if (cs->bitmap[i] == 0) {                       /* current word full */
        size_t start = i;
        do {
            i = (i + 1) % (size_t)cs->bitmap_length;
        } while (cs->bitmap[i] == 0 && i != start);
        if (i == start) {
            return -1;                              /* scanned all the way -> full */
        }
        cs->last_entry = i;
    }
    int index = BITS_PER_WORD - 1 - CLZL(cs->bitmap[i]);   /* (A) read free bit */
    widen();                                               /* <-- the race window */
    cs->bitmap[i] &= ~BITW(index);                         /* (B) clear -> allocate */
    return cs->first_slot + ((long)i * BITS_PER_WORD + index);
}

/* The fix: the exact spinlock AIOS already uses (v3d.c / posix_thread.c). */
static volatile unsigned char g_lock = 0;
static void vlock(void)   { while (__atomic_test_and_set(&g_lock, __ATOMIC_ACQUIRE)) sched_yield(); }
static void vunlock(void) { __atomic_clear(&g_lock, __ATOMIC_RELEASE); }

static long locked_alloc(cspace_t *cs)
{
    vlock();
    long r = raw_alloc(cs);
    vunlock();
    return r;
}

/* ---- harness ---- */
#define NTHREADS   4
#define PER_THREAD 1500

static cspace_t  g_cs;
static int       g_locked_mode;
static long     *g_out[NTHREADS];   /* per-thread returned slots */

static void *worker(void *arg)
{
    long tid = (long)arg;
    long *out = g_out[tid];
    for (int k = 0; k < PER_THREAD; k++) {
        out[k] = g_locked_mode ? locked_alloc(&g_cs) : raw_alloc(&g_cs);
    }
    return NULL;
}

/* Returns the number of DUPLICATE slot handouts (0 == clean). */
static int run_mode(int locked, const char *name)
{
    int total_slots = NTHREADS * PER_THREAD;
    int words = total_slots / BITS_PER_WORD + 64;   /* plenty of headroom, never full */

    size_t *bm = malloc((size_t)words * sizeof(size_t));
    memset(bm, 0xff, (size_t)words * sizeof(size_t)); /* all free (1s) -- as allocman create does */
    g_cs.bitmap = bm;
    g_cs.bitmap_length = words;
    g_cs.last_entry = 0;
    g_cs.first_slot = 1000;
    g_locked_mode = locked;
    g_lock = 0;

    for (int t = 0; t < NTHREADS; t++) {
        g_out[t] = malloc(sizeof(long) * PER_THREAD);
    }

    pthread_t th[NTHREADS];
    for (long t = 0; t < NTHREADS; t++) {
        pthread_create(&th[t], NULL, worker, (void *)t);
    }
    for (int t = 0; t < NTHREADS; t++) {
        pthread_join(th[t], NULL);
    }

    /* Count how many times each slot was handed out. */
    long max_slot = g_cs.first_slot + (long)words * BITS_PER_WORD;
    int *seen = calloc((size_t)max_slot + 1, sizeof(int));
    int allocated = 0, fulls = 0;
    for (int t = 0; t < NTHREADS; t++) {
        for (int k = 0; k < PER_THREAD; k++) {
            long s = g_out[t][k];
            if (s < 0) { fulls++; continue; }
            allocated++;
            if (s <= max_slot) seen[s]++;
        }
    }
    int dups = 0, dup_slots = 0;
    for (long s = 0; s <= max_slot; s++) {
        if (seen[s] > 1) { dups += seen[s] - 1; dup_slots++; }
    }
    printf("  MODE %-7s: threads=%d x %d = %d allocs, fulls=%d, duplicate-handouts=%d (over %d slots)\n",
           name, NTHREADS, PER_THREAD, allocated, fulls, dups, dup_slots);

    free(seen);
    for (int t = 0; t < NTHREADS; t++) free(g_out[t]);
    free(bm);
    return dups;
}

int main(void)
{
    printf("=== allocman cspace-slot bitmap tear: host repro + lock proof ===\n");
    int buggy_dups  = run_mode(0, "buggy");
    int locked_dups = run_mode(1, "locked");

    /* The test is meaningful only if the buggy path actually tears here; if the
     * host scheduler never interleaved the window, re-run (rare). */
    int ok = (buggy_dups > 0) && (locked_dups == 0);
    printf("RESULT: buggy tears=%d, locked tears=%d -> %s\n",
           buggy_dups, locked_dups, ok ? "OK (lock is real + load-bearing)" : "FAIL");
    if (buggy_dups == 0) {
        printf("  NOTE: buggy did not tear this run (scheduler did not interleave) -- re-run; the bug is timing-dependent\n");
    }
    return ok ? 0 : 1;
}
