/*
 * uk/pal/sel4/pipe.c -- the Phase C.5 PIPE BACKING: an in-root-task byte ring, the seL4 answer to the
 * Linux PAL's pipe2(O_NONBLOCK). This is the ONLY new work C.5 needs: the kernel already owns the
 * park/wake fixpoint (do_read/do_write park a guest PS_BLOCKED_READ/WRITE on PAL_EWOULDBLOCK and
 * pipe_settle wakes it via pal_guest_return -- which, since C.2, replies over a PARKED guest's saved
 * SaveCaller reply cap; the wait() park already proved that path). The kernel holds NO pipe bytes: it
 * calls pal_host_pipe to make a backing, then pal_host_read/write on the two ends, exactly as it does
 * a host pipe. So a pipe here is a ring buffer with two ends; the ends' PAL contract must match the
 * Linux non-blocking pipe the kernel is written against:
 *   read  -> bytes copied (>0)                                    data available
 *         -> 0                                                    EOF: empty AND all write ends closed
 *         -> PAL_EWOULDBLOCK                                      empty but a write end is still open
 *   write -> bytes accepted (may be < n: the kernel loops/parks)  space available
 *         -> PAL_EWOULDBLOCK                                      full (0 accepted) and readers remain
 *         -> PAL_EPIPE                                            all read ends closed (SIGPIPE/-EPIPE)
 *
 * Refcounting is the KERNEL's job: it shares one backing across dup2/fork (ofile refcount) and calls
 * pal_host_close EXACTLY ONCE per end, when that end's ofile refcount hits 0. So each pipe has one
 * read-end handle + one write-end handle, each closed once; the ring is reclaimed when both are gone.
 * Buffered bytes survive a write-end close (a reader drains them, THEN sees EOF -- POSIX).
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pal.h"
#include "pipe.h"

/* The handle space: disjoint from the console (0/1/2) and the tarfs (3 .. 3+255). Read end of pipe i
 * = PIPE_FD_BASE + 2*i, write end = +1. 300 clears the tarfs's 256-slot table with room to spare. */
#define PIPE_FD_BASE 300
#define PIPE_MAX     16                 /* concurrent pipes (a shell pipeline uses a handful) */
#define PIPE_RING    (16 * 1024)        /* per-pipe capacity; smaller than Linux's 64K just means more
                                         * park/wake cycles for a big transfer, never wrong bytes */

struct pipe {
    int      used;
    int      read_open, write_open;
    unsigned head;                      /* next byte to read */
    unsigned count;                     /* bytes buffered (0 .. PIPE_RING) */
    unsigned char buf[PIPE_RING];
};
static struct pipe g_pipes[PIPE_MAX];

int pipe_is_handle(pal_file_t f) {
    return f >= PIPE_FD_BASE && f < PIPE_FD_BASE + 2 * PIPE_MAX;
}
static struct pipe *pfind(pal_file_t f, int *is_write) {
    if (!pipe_is_handle(f)) return NULL;
    long k = (long)f - PIPE_FD_BASE;
    struct pipe *pp = &g_pipes[k / 2];
    if (!pp->used) return NULL;
    *is_write = (int)(k & 1);
    return pp;
}

/* pal_host_pipe: allocate a fresh ring, hand back its two end handles (read = base+2i, write = +1). */
int pal_host_pipe(pal_file_t *rd, pal_file_t *wr) {
    for (int i = 0; i < PIPE_MAX; i++) {
        struct pipe *pp = &g_pipes[i];
        if (pp->used) continue;
        pp->used = 1;
        pp->read_open = pp->write_open = 1;
        pp->head = pp->count = 0;
        *rd = (pal_file_t)(PIPE_FD_BASE + 2 * i);
        *wr = (pal_file_t)(PIPE_FD_BASE + 2 * i + 1);
        return 0;
    }
    return -AIOS_EMFILE;                 /* the pipe table is full (Linux pipe2 -> EMFILE/ENFILE) */
}

long pipe_read(pal_file_t f, void *buf, size_t n) {
    int is_write;
    struct pipe *pp = pfind(f, &is_write);
    if (!pp || is_write) return -AIOS_EBADF;      /* gone, or a read on a write-end (single-dir fd) */
    if (n == 0) return 0;                         /* POSIX read(...,0) is an immediate no-op -- guard
                                                   * BEFORE the empty test, else an empty pipe parks a
                                                   * 0-length read forever (review catch) */
    if (pp->count == 0) return pp->write_open ? PAL_EWOULDBLOCK : 0;   /* empty: block, or EOF */
    unsigned take = pp->count < n ? pp->count : (unsigned)n;
    unsigned first = PIPE_RING - pp->head;                            /* bytes to the ring's end */
    if (first >= take) {
        memcpy(buf, pp->buf + pp->head, take);
    } else {                                                          /* wrap */
        memcpy(buf, pp->buf + pp->head, first);
        memcpy((char *)buf + first, pp->buf, take - first);
    }
    pp->head = (pp->head + take) % PIPE_RING;
    pp->count -= take;
    return (long)take;
}

long pipe_write(pal_file_t f, const void *buf, size_t n) {
    int is_write;
    struct pipe *pp = pfind(f, &is_write);
    if (!pp || !is_write) return -AIOS_EBADF;     /* gone, or a write on a read-end (single-dir fd) */
    if (n == 0) return 0;                          /* 0-length write is a no-op (defensive: the kernel's
                                                    * pipe_write_some already shortcuts len==0) */
    if (!pp->read_open) return PAL_EPIPE;                             /* no readers -> SIGPIPE/-EPIPE */
    unsigned space = PIPE_RING - pp->count;
    if (space == 0) return PAL_EWOULDBLOCK;                           /* full -> the kernel parks */
    unsigned put = space < n ? space : (unsigned)n;
    unsigned tail = (pp->head + pp->count) % PIPE_RING;
    unsigned first = PIPE_RING - tail;                               /* bytes to the ring's end */
    if (first >= put) {
        memcpy(pp->buf + tail, buf, put);
    } else {                                                          /* wrap */
        memcpy(pp->buf + tail, buf, first);
        memcpy(pp->buf, (const char *)buf + first, put - first);
    }
    pp->count += put;
    return (long)put;                                                /* may be < n: the kernel loops/parks */
}

int pipe_close(pal_file_t f) {
    int is_write;
    struct pipe *pp = pfind(f, &is_write);
    if (!pp) return 0;                                                /* already gone / not a pipe */
    if (is_write) pp->write_open = 0; else pp->read_open = 0;
    if (!pp->read_open && !pp->write_open) pp->used = 0;              /* both ends gone -> reclaim */
    return 0;
}
