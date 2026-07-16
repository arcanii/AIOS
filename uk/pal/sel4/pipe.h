/*
 * uk/pal/sel4/pipe.h -- the Phase C.5 in-root-task pipe byte ring. pipe.c DEFINES pal_host_pipe and
 * the byte-ring ops; this header is only the seam boot.c (pal_host_write) and tarfs.c (pal_host_read/
 * close) need to ROUTE a pipe-end handle to the ring instead of the console/tarfs.
 */
#ifndef AIOS_PAL_SEL4_PIPE_H
#define AIOS_PAL_SEL4_PIPE_H

#include "pal.h"

/* Is `f` one of our pipe-end backing handles? (A disjoint, high handle range -- see pipe.c.) */
int  pipe_is_handle(pal_file_t f);

/* The ring ops behind the PAL contract for pipe-end handles. Match the Linux non-blocking pipe the
 * kernel's do_read/do_write/pipe_settle path expects: read -> bytes / 0 (EOF, all writers closed) /
 * PAL_EWOULDBLOCK (empty, a writer open); write -> bytes accepted / PAL_EWOULDBLOCK (full) /
 * PAL_EPIPE (all readers closed). */
long pipe_read (pal_file_t f, void *buf, size_t n);
long pipe_write(pal_file_t f, const void *buf, size_t n);
int  pipe_close(pal_file_t f);

#endif
