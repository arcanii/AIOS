#ifndef AIOS_FILEHITS_H
#define AIOS_FILEHITS_H
/*
 * filehits.h -- v0.4.114 access-frequency profiler
 *
 * Records every vfs_read / vfs_pread call by path. Bounded fixed
 * table (no dynamic allocation), LRU-evicted when full. The output
 * (via /proc/filehits) tells us which files to add to the boot
 * warm-list and which to drop.
 *
 * Filtering: paths under /proc are skipped (they are virtual and
 * don't hit the cache anyway, and `cat /proc/filehits` would
 * pollute its own data).
 */
#include <stdint.h>
#include <stddef.h>

/* 80 chars is plenty for AIOS paths (deepest is ~30 chars). */
#define FILEHITS_MAX        96
#define FILEHITS_PATH_MAX   80

void filehits_record(const char *path, int bytes);

/* Format the top N entries (sorted by open count desc) into buf.
 * Returns bytes written. Used by /proc/filehits. */
int  filehits_format(char *buf, int bufsize, int top_n);

#endif
